#include "client/attach.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "common/fd.hpp"
#include "common/protocol.hpp"

namespace pmux {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kHoldTime = std::chrono::milliseconds(20);
constexpr int kDetachReplyTimeoutMs = 1000;
// For this long after an attach / view starts, replies to the list screen's own terminal
// query are dropped from the input (see strip_list_replies).
constexpr auto kReplyFilterTime = std::chrono::milliseconds(500);
// The daemon is not read while this much output still waits for the terminal.
constexpr std::size_t kMaxPendingOutput = 256 * 1024;

constexpr int kWheelLines = 3;

// A session is shown on the terminal's alternate screen; the main screen keeps what it showed
// before (and its scrollback). All session output goes to the alternate screen, so the resets
// happen there (kitty keyboard flags have a stack per screen), then the main screen returns.
constexpr std::string_view kEnterAltScreen = "\x1b[?1049h";
constexpr std::string_view kModeResets =
    "\x1b[<99u"
    "\x1b[?9l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1005l\x1b[?1006l\x1b[?1015l\x1b[?1016l"
    "\x1b[?1l\x1b>"
    "\x1b[?2004l"
    "\x1b[?1004l"
    "\x1b[?25h"
    "\x1b[?7h"
    "\x1b[0m"
    "\x1b[>4m";
constexpr std::string_view kLeaveAltScreen = "\x1b[?1049l";
// pmux's own mouse modes: button presses / releases and the wheel, SGR encoding.
constexpr std::string_view kPmuxMouse = "\x1b[?1000h\x1b[?1006h";

// Mode resets and OSC color resets on the alternate screen, then back to the main screen.
void restore_terminal(const std::string& color_resets) {
  std::string out(kModeResets);
  out += color_resets;
  out += kLeaveAltScreen;
  write_all(STDOUT_FILENO, out.data(), out.size());
}

// The terminal's mouse modes for a STATE: pmux's while the app tracks no mouse, else exactly
// the app's tracking mode and encodings.
std::string mouse_modes(const ClientState& st) {
  std::string out;
  if (st.mouse == 0) {
    if (st.flags & kStateMouseUtf8) out += "\x1b[?1005l";
    if (st.flags & kStateMouseUrxvt) out += "\x1b[?1015l";
    if (st.flags & kStateMousePixels) out += "\x1b[?1016l";
    out += kPmuxMouse;
    return out;
  }
  out += "\x1b[?" + std::to_string(st.mouse) + "h";
  out += st.flags & kStateMouseSgr ? "\x1b[?1006h" : "\x1b[?1006l";
  if (st.flags & kStateMouseUtf8) out += "\x1b[?1005h";
  if (st.flags & kStateMouseUrxvt) out += "\x1b[?1015h";
  if (st.flags & kStateMousePixels) out += "\x1b[?1016h";
  return out;
}

// SGR for the scroll position indicator: reverse video, in the accent color (a 256-color
// approximation of the list screen's) unless the theme is ansi.
std::string indicator_style(const Config& config) {
  if (config.theme == ThemeMode::Ansi) return "\x1b[0;7m";
  const bool light = config.theme == ThemeMode::Light;
  int color = 0;
  switch (config.accent) {
    case Accent::Clay: color = light ? 131 : 173; break;
    case Accent::Blue: color = light ? 26 : 111; break;
    case Accent::Purple: color = light ? 98 : 141; break;
    case Accent::Teal: color = light ? 30 : 80; break;
  }
  return "\x1b[0;7;38;5;" + std::to_string(color) + "m";
}

// ` offset/total ` in the top right corner.
std::string indicator(const std::string& style, std::uint32_t offset, std::uint32_t total) {
  const int cols = [] {
    winsize ws{};
    return ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? int(ws.ws_col) : 80;
  }();
  const std::string text = " " + std::to_string(offset) + "/" + std::to_string(total) + " ";
  if (int(text.size()) > cols) return {};
  return "\x1b[1;" + std::to_string(cols - int(text.size()) + 1) + "H" + style + text + "\x1b[0m";
}

// Signals the passthrough loop reads from its signalfd.
sigset_t attach_signals() {
  sigset_t set;
  sigemptyset(&set);
  for (int sig : {SIGWINCH, SIGTERM, SIGHUP, SIGINT, SIGQUIT}) sigaddset(&set, sig);
  return set;
}

bool all_digits(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int to_int(std::string_view s) {
  int v = 0;
  for (char c : s) v = std::min(v * 10 + (c - '0'), 1 << 20);
  return v;
}

enum class KeyKind { Other, Detach, Swallow, Partial };

// Classifies kitty modifiers `<m>[:<ev>]`: Detach if the modifier bits besides Caps Lock / Num
// Lock are exactly `want` (press or no event type); repeat and release are swallowed.
KeyKind classify_mods(std::string_view mods, int want) {
  const std::size_t colon = mods.find(':');
  const std::string_view m = mods.substr(0, colon);
  const std::string_view ev = colon == std::string_view::npos ? "1" : mods.substr(colon + 1);
  if (!all_digits(m) || !all_digits(ev)) return KeyKind::Other;
  const int bits = to_int(m) - 1;
  if (bits < 0 || (bits & ~(64 | 128)) != want) return KeyKind::Other;
  switch (to_int(ev)) {
    case 1: return KeyKind::Detach;
    case 2:
    case 3: return KeyKind::Swallow;
    default: return KeyKind::Other;
  }
}

// Kitty modifier bits: Shift 1, Ctrl 4.
constexpr int kCtrl = 4;
constexpr int kCtrlShift = 5;

// Classifies a complete CSI sequence (parameters, final byte) for the detach key.
//   ctrl+left / ctrl+shift+left: `CSI 1;<m>[:<ev>]D` (xterm `CSI 1;5D` / `CSI 1;6D` included).
//   ctrl+backslash: kitty `CSI 92;<m>[:<ev>]u` and xterm modifyOtherKeys `CSI 27;5;92~`.
KeyKind classify_csi(std::string_view params, char final, DetachKey key) {
  if (key != DetachKey::CtrlBackslash) {
    if (final != 'D' || !params.starts_with("1;")) return KeyKind::Other;
    return classify_mods(params.substr(2), key == DetachKey::CtrlLeft ? kCtrl : kCtrlShift);
  }
  if (final == '~') return params == "27;5;92" ? KeyKind::Detach : KeyKind::Other;
  if (final != 'u') return KeyKind::Other;
  const std::size_t semi = params.find(';');
  if (semi == std::string_view::npos) return KeyKind::Other;
  const std::string_view code = params.substr(0, semi);
  if (code != "92" && !code.starts_with("92:")) return KeyKind::Other;  // 92:<alternate keys>
  std::string_view mods = params.substr(semi + 1);
  mods = mods.substr(0, mods.find(';'));  // drop the associated-text field
  return classify_mods(mods, kCtrl);
}

// True if unfinished CSI parameters may still become a detach or swallowed key.
bool could_be_key(std::string_view params, DetachKey key) {
  if (key != DetachKey::CtrlBackslash) {
    constexpr std::string_view arrow = "1;";
    return arrow.starts_with(params) || params.starts_with(arrow);
  }
  constexpr std::string_view kitty = "92", xterm = "27;5;92";
  return kitty.starts_with(params) || xterm.starts_with(params) || params.starts_with("92;") ||
         params.starts_with("92:");
}

// Classifies the key sequence at the start of `rest` (rest[0] == ESC); sets `len` to its
// length for Detach / Swallow. Partial: a split sequence of at least 3 bytes that may still
// become one (a lone ESC or `ESC [` is never held).
KeyKind classify_key(std::string_view rest, DetachKey key, std::size_t& len) {
  if (rest.size() >= 3 && rest[1] == 'O') {  // rxvt Ctrl+Left: ESC O d
    len = 3;
    return key == DetachKey::CtrlLeft && rest[2] == 'd' ? KeyKind::Detach : KeyKind::Other;
  }
  if (rest.size() < 2 || rest[1] != '[') return KeyKind::Other;
  std::size_t i = 2;
  while (i < rest.size() && rest[i] >= 0x30 && rest[i] <= 0x3F) ++i;
  const std::string_view params = rest.substr(2, i - 2);
  if (i == rest.size())
    return rest.size() >= 3 && could_be_key(params, key) ? KeyKind::Partial : KeyKind::Other;
  len = i + 1;
  return classify_csi(params, rest[i], key);
}

enum class MouseKind { Other, Report, Partial };

// Classifies an SGR mouse report `ESC [ < b ; x ; y M|m` at the start of `rest`; sets `len`
// and `button` (b) for Report. Partial: an unfinished one (`ESC [ <` and more).
MouseKind classify_mouse(std::string_view rest, std::size_t& len, int& button) {
  if (!rest.starts_with("\x1b[<")) return MouseKind::Other;
  std::size_t i = 3;
  while (i < rest.size() && rest[i] >= 0x30 && rest[i] <= 0x3F) ++i;
  if (i == rest.size()) return MouseKind::Partial;
  const char final = rest[i];
  const std::string_view params = rest.substr(3, i - 3);
  if (final != 'M' && final != 'm') return MouseKind::Other;
  std::size_t fields = 0;
  for (std::string_view p = params;; ++fields) {
    const std::size_t semi = p.find(';');
    if (!all_digits(p.substr(0, semi))) return MouseKind::Other;
    if (fields == 0) button = to_int(p.substr(0, semi));
    if (semi == std::string_view::npos) break;
    p.remove_prefix(semi + 1);
  }
  if (fields != 2) return MouseKind::Other;
  len = i + 1;
  return MouseKind::Report;
}

bool wheel(int button) {
  return (button & 64) && !(button & 128) && (button & 3) < 2;
}

bool wheel_up(int button) {
  return wheel(button) && (button & 3) == 0;
}

enum class ScrollKey { Up, Down, PageUp, PageDown, Top, Bottom, Exit, WheelUp, WheelDown, Swallow, Other, Partial };

// Classifies the key at the start of `rest` in scroll mode; sets `len` to its length (not for
// Other / Partial). Navigation keys in their plain, application (SS3) and kitty forms; Esc and
// q exit; mouse reports other than the wheel, focus reports and kitty key releases are
// swallowed. Partial: a split sequence of at least 3 bytes.
ScrollKey scroll_key(std::string_view rest, std::size_t& len) {
  len = 1;
  if (rest[0] == 'q') return ScrollKey::Exit;
  if (rest[0] != '\x1b') return ScrollKey::Other;
  if (rest.size() == 1) return ScrollKey::Exit;  // a lone ESC is the Esc key
  if (rest[1] == 'O') {
    if (rest.size() < 3) return ScrollKey::Other;
    len = 3;
    switch (rest[2]) {
      case 'A': return ScrollKey::Up;
      case 'B': return ScrollKey::Down;
      case 'H': return ScrollKey::Top;
      case 'F': return ScrollKey::Bottom;
      default: return ScrollKey::Other;
    }
  }
  if (rest[1] != '[') return ScrollKey::Other;
  int button = 0;
  switch (classify_mouse(rest, len, button)) {
    case MouseKind::Report:
      if (!wheel(button)) return ScrollKey::Swallow;
      return (button & 3) == 0 ? ScrollKey::WheelUp : ScrollKey::WheelDown;
    case MouseKind::Partial: return ScrollKey::Partial;
    case MouseKind::Other: break;
  }
  std::size_t i = 2;
  while (i < rest.size() && rest[i] >= 0x20 && rest[i] <= 0x3F) ++i;
  if (i == rest.size()) return rest.size() >= 3 ? ScrollKey::Partial : ScrollKey::Other;
  len = i + 1;
  const char final = rest[i];
  const std::string_view params = rest.substr(2, i - 2);
  if (params.empty() && (final == 'I' || final == 'O')) return ScrollKey::Swallow;  // focus
  // `code[;mods[:event]]`: unmodified keys only; kitty releases (event 3) are swallowed.
  const std::size_t semi = params.find(';');
  const std::string_view code = params.substr(0, semi);
  if (semi != std::string_view::npos) {
    const std::string_view mods = params.substr(semi + 1);
    const std::size_t colon = mods.find(':');
    const std::string_view m = mods.substr(0, colon);
    const std::string_view ev = colon == std::string_view::npos ? "1" : mods.substr(colon + 1);
    if (!all_digits(m) || !all_digits(ev)) return ScrollKey::Other;
    if (to_int(ev) == 3) return ScrollKey::Swallow;
    if (((to_int(m) - 1) & ~(64 | 128)) != 0) return ScrollKey::Other;
  }
  if (!code.empty() && !all_digits(code)) return ScrollKey::Other;
  const int n = code.empty() ? 1 : to_int(code);
  switch (final) {
    case 'A': return n == 1 ? ScrollKey::Up : ScrollKey::Other;
    case 'B': return n == 1 ? ScrollKey::Down : ScrollKey::Other;
    case 'H': return n == 1 ? ScrollKey::Top : ScrollKey::Other;
    case 'F': return n == 1 ? ScrollKey::Bottom : ScrollKey::Other;
    case '~':
      switch (n) {
        case 5: return ScrollKey::PageUp;
        case 6: return ScrollKey::PageDown;
        case 1:
        case 7: return ScrollKey::Top;
        case 4:
        case 8: return ScrollKey::Bottom;
        default: return ScrollKey::Other;
      }
    case 'u': return n == 27 || n == 113 ? ScrollKey::Exit : ScrollKey::Other;  // kitty Esc, q
    default: return ScrollKey::Other;
  }
}

// The SCROLL request for a navigation key (Exit for Esc / q); nullopt for other keys.
std::optional<std::pair<ScrollOp, int>> scroll_op(ScrollKey key) {
  switch (key) {
    case ScrollKey::Up: return std::pair(ScrollOp::Up, 1);
    case ScrollKey::Down: return std::pair(ScrollOp::Down, 1);
    case ScrollKey::WheelUp: return std::pair(ScrollOp::Up, kWheelLines);
    case ScrollKey::WheelDown: return std::pair(ScrollOp::Down, kWheelLines);
    case ScrollKey::PageUp: return std::pair(ScrollOp::PageUp, 0);
    case ScrollKey::PageDown: return std::pair(ScrollOp::PageDown, 0);
    case ScrollKey::Top: return std::pair(ScrollOp::Top, 0);
    case ScrollKey::Bottom: return std::pair(ScrollOp::Bottom, 0);
    case ScrollKey::Exit: return std::pair(ScrollOp::Exit, 0);
    default: return std::nullopt;
  }
}

bool send_scroll(int fd, ScrollOp op, int lines) {
  return send_frame(fd, make_frame(MsgType::Scroll, PayloadWriter().u8(std::uint8_t(op)).u16(std::uint16_t(lines)).take()));
}

// The list screen (FTXUI) asks for the cursor shape (DECRQSS DECSCUSR) whenever it resumes. If
// the reply `ESC P 0|1 $ r ... SP q ESC \` arrives only after the next attach started, it must
// not reach the process. Removes complete replies from `input`; an unterminated one at its end
// is removed too and returned (to be held back).
std::string strip_list_replies(std::string& input) {
  constexpr std::string_view heads[] = {"\x1bP1$r", "\x1bP0$r"};
  std::string out, held;
  std::size_t i = 0;
  while (i < input.size()) {
    const std::size_t at = input.find("\x1bP", i);
    if (at == std::string::npos) break;
    out.append(input, i, at - i);
    const std::string_view rest = std::string_view(input).substr(at);
    const bool head = std::any_of(std::begin(heads), std::end(heads), [&](std::string_view h) {
      return rest.size() < h.size() ? h.starts_with(rest) : rest.starts_with(h);
    });
    const std::size_t end = head ? rest.find("\x1b\\", 2) : std::string_view::npos;
    if (!head) {
      out.append(rest.substr(0, 2));
      i = at + 2;
    } else if (end == std::string_view::npos) {
      held = rest;
      i = input.size();
    } else {
      if (!rest.substr(0, end).ends_with(" q")) out.append(rest.substr(0, end + 2));
      i = at + end + 2;
    }
  }
  if (i < input.size()) out.append(input, i);
  input = std::move(out);
  return held;
}

struct Winsize {
  std::uint16_t rows = 0;
  std::uint16_t cols = 0;
};

Winsize terminal_size(int fd) {
  winsize ws{};
  if (ioctl(fd, TIOCGWINSZ, &ws) != 0) return {};
  return {ws.ws_row, ws.ws_col};
}

enum class Outcome { Detached, AttachedElsewhere, Exited, Lost, Signaled };

class Passthrough {
 public:
  // `out_fd`: where output goes; non-blocking unless it is stdout itself.
  Passthrough(int sock, FrameDecoder& decoder, int sig_fd, int out_fd, Clock::time_point filter_until,
              const Config& config)
      : sock_(sock),
        decoder_(decoder),
        sig_fd_(sig_fd),
        out_fd_(out_fd),
        filter_until_(filter_until),
        key_(config.detach_key),
        indicator_style_(indicator_style(config)) {}

  Outcome run();
  int exit_status() const { return exit_status_; }
  int signal() const { return signal_; }
  // OSC color resets the daemon sent with DETACH / EXITED.
  const std::string& color_resets() const { return color_resets_; }

 private:
  Outcome detach();
  Outcome finish(Outcome outcome);
  std::optional<Outcome> handle(const Frame& frame);
  bool read_socket();
  std::optional<Outcome> read_signals();
  std::optional<Outcome> read_stdin();
  std::optional<Outcome> process_input(std::string_view input);
  std::string_view scroll_input(std::string_view input);
  void on_mouse(int button);
  void on_state(const Frame& frame);
  void hold(std::string_view bytes);
  void append_output(std::string_view bytes);
  std::size_t pending_output() const { return out_.size() - out_pos_; }
  bool write_output();
  void flush_output();
  void send_input(std::string_view bytes);
  void send_resize();

  int sock_;
  FrameDecoder& decoder_;
  int sig_fd_;
  int out_fd_;
  Clock::time_point filter_until_;
  DetachKey key_;
  std::string indicator_style_;
  ClientState state_;  // from the last STATE (mouse == 0: pmux has the mouse)
  bool state_applied_ = false;
  bool scrolled_ = false;
  std::string out_;  // output not yet written to the terminal, from out_pos_
  std::size_t out_pos_ = 0;
  std::string held_;
  Clock::time_point held_since_;
  int exit_status_ = 0;
  int signal_ = 0;
  std::string color_resets_;
  bool lost_ = false;
};

// Each pass handles signals, input, output and the daemon, so neither a flood of output nor
// a slow terminal can delay the detach key or a resize.
Outcome Passthrough::run() {
  for (;;) {
    while (pending_output() < kMaxPendingOutput) {
      auto frame = decoder_.next();
      if (!frame) break;
      if (auto outcome = handle(*frame)) return finish(*outcome);
    }
    if (decoder_.bad() || lost_) return finish(Outcome::Lost);

    int timeout = -1;
    if (!held_.empty()) {
      const auto left = kHoldTime - (Clock::now() - held_since_);
      if (left <= Clock::duration::zero()) {
        send_input(std::exchange(held_, {}));
        continue;
      }
      timeout = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(left).count());
    }

    const bool want_output = pending_output() < kMaxPendingOutput;
    pollfd fds[4] = {{want_output ? sock_ : -1, POLLIN, 0},
                     {STDIN_FILENO, POLLIN, 0},
                     {sig_fd_, POLLIN, 0},
                     {pending_output() > 0 ? out_fd_ : -1, POLLOUT, 0}};
    if (poll(fds, 4, timeout) < 0) {
      if (errno == EINTR) continue;
      return Outcome::Lost;
    }
    if (fds[2].revents & POLLIN)
      if (auto outcome = read_signals()) return *outcome;
    if (fds[1].revents)
      if (auto outcome = read_stdin()) return *outcome == Outcome::Detached ? detach() : *outcome;
    if (fds[3].revents && !write_output()) return Outcome::Lost;
    if (fds[0].revents && !read_socket()) return finish(Outcome::Lost);
  }
}

// Asks the daemon to detach us and waits for its DETACH (or EXITED) reply with the color
// resets. Output not yet written, or arriving meanwhile, is dropped.
Outcome Passthrough::detach() {
  out_.clear();
  out_pos_ = 0;
  if (!send_frame(sock_, make_frame(MsgType::Detach))) return Outcome::Detached;
  const auto deadline = Clock::now() + std::chrono::milliseconds(kDetachReplyTimeoutMs);
  for (;;) {
    while (auto frame = decoder_.next()) {
      if (frame->type == MsgType::Detach || frame->type == MsgType::Exited) {
        handle(*frame);
        return Outcome::Detached;
      }
    }
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (decoder_.bad() || left <= 0) return Outcome::Detached;
    pollfd fd = {sock_, POLLIN, 0};
    const int n = poll(&fd, 1, static_cast<int>(left));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0 || !read_socket()) return Outcome::Detached;
  }
}

// Ends the session with its remaining output written.
Outcome Passthrough::finish(Outcome outcome) {
  flush_output();
  return outcome;
}

std::optional<Outcome> Passthrough::handle(const Frame& frame) {
  switch (frame.type) {
    case MsgType::Output:
    case MsgType::Snapshot:
      append_output({reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size()});
      return std::nullopt;
    case MsgType::State:
      on_state(frame);
      return std::nullopt;
    case MsgType::Exited: {
      PayloadReader r(frame.payload);
      exit_status_ = r.i32();
      color_resets_ = r.str();
      return Outcome::Exited;
    }
    case MsgType::Detach:
      color_resets_ = PayloadReader(frame.payload).str();
      return Outcome::AttachedElsewhere;
    case MsgType::Error:
      return Outcome::Lost;
    default:
      return std::nullopt;
  }
}

void Passthrough::append_output(std::string_view bytes) {
  if (out_pos_ > 0 && out_pos_ >= out_.size() / 2) {
    out_.erase(0, out_pos_);
    out_pos_ = 0;
  }
  out_.append(bytes);
}

// Applies the terminal's mouse modes (in stream order with the output) and draws the scroll
// position indicator.
void Passthrough::on_state(const Frame& frame) {
  PayloadReader r(frame.payload);
  ClientState st;
  st.flags = r.u8();
  st.mouse = r.u16();
  const std::uint32_t offset = r.u32();
  const std::uint32_t total = r.u32();
  if (!r.ok()) return;
  const bool reapply = st.flags & kStateReapply;
  scrolled_ = st.flags & kStateScrolled;
  st.flags &= std::uint8_t(~(kStateReapply | kStateScrolled));
  if (reapply || !state_applied_ || st != state_) append_output(mouse_modes(st));
  state_ = st;
  state_applied_ = true;
  if (scrolled_) append_output(indicator(indicator_style_, offset, total));
}

bool Passthrough::read_socket() {
  char buf[65536];
  const ssize_t n = read(sock_, buf, sizeof buf);
  if (n < 0 && (errno == EINTR || errno == EAGAIN)) return true;
  if (n <= 0) return false;
  decoder_.feed(buf, std::size_t(n));
  return true;
}

// SIGWINCH forwards the size; SIGTERM / SIGHUP / SIGINT / SIGQUIT end the attach as a detach.
std::optional<Outcome> Passthrough::read_signals() {
  signalfd_siginfo info;
  bool winch = false;
  int term = 0;
  while (read(sig_fd_, &info, sizeof info) == sizeof info) {
    if (info.ssi_signo == SIGWINCH) winch = true;
    else if (term == 0) term = static_cast<int>(info.ssi_signo);
  }
  if (term != 0) {
    signal_ = term;
    detach();
    return Outcome::Signaled;
  }
  if (winch) send_resize();
  return std::nullopt;
}

// Writes as much pending output as the terminal takes without blocking.
bool Passthrough::write_output() {
  while (pending_output() > 0) {
    const std::size_t len = std::min<std::size_t>(pending_output(), 65536);
    const ssize_t n = write(out_fd_, out_.data() + out_pos_, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return errno == EAGAIN;
    }
    out_pos_ += std::size_t(n);
  }
  out_.clear();
  out_pos_ = 0;
  return true;
}

void Passthrough::flush_output() {
  while (pending_output() > 0) {
    pollfd fd = {out_fd_, POLLOUT, 0};
    if (poll(&fd, 1, -1) < 0 && errno != EINTR) return;
    if (!write_output()) return;
  }
}

std::optional<Outcome> Passthrough::read_stdin() {
  char buf[4096];
  const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
  if (n < 0 && (errno == EINTR || errno == EAGAIN)) return std::nullopt;
  if (n <= 0) return Outcome::Detached;

  std::string input = std::exchange(held_, {});
  input.append(buf, std::size_t(n));
  std::string reply_tail;
  if (Clock::now() < filter_until_) reply_tail = strip_list_replies(input);
  if (auto outcome = process_input(input)) return outcome;
  hold(reply_tail);
  return lost_ ? std::optional(Outcome::Lost) : std::nullopt;
}

// Only the detach key and, while pmux has the mouse, mouse reports are intercepted; in scroll
// mode also the navigation keys.
std::optional<Outcome> Passthrough::process_input(std::string_view input) {
  while (!input.empty()) {
    if (scrolled_) {
      input = scroll_input(input);
      continue;
    }
    const InputScan scan = scan_input(input, key_, state_.mouse == 0);
    send_input(scan.forward);
    if (scan.detach) return Outcome::Detached;
    if (scan.mouse < 0) {
      hold(scan.held);
      break;
    }
    on_mouse(scan.mouse);
    input.remove_prefix(scan.consumed);
  }
  return std::nullopt;
}

// Handles scroll mode keys; returns the input from the first other key on (scroll mode is then
// left: the daemon leaves it on INPUT), or nothing.
std::string_view Passthrough::scroll_input(std::string_view input) {
  while (!input.empty() && scrolled_) {
    std::size_t len = 0;
    const ScrollKey key = scroll_key(input, len);
    if (key == ScrollKey::Partial) {
      hold(input);
      return {};
    }
    if (key == ScrollKey::Other) {
      scrolled_ = false;
      return input;
    }
    if (const auto op = scroll_op(key)) {
      if (!send_scroll(sock_, op->first, op->second)) lost_ = true;
      if (op->first == ScrollOp::Exit || op->first == ScrollOp::Bottom) scrolled_ = false;
    }
    input.remove_prefix(len);
  }
  return input;
}

// A mouse report while pmux has the mouse: the wheel scrolls (primary screen) or becomes cursor
// keys (alternate screen); everything else is swallowed.
void Passthrough::on_mouse(int button) {
  if (!wheel(button)) return;
  if (state_.flags & kStateAltScreen) {
    const bool app_keys = state_.flags & kStateCursorKeys;
    const std::string_view key = wheel_up(button) ? (app_keys ? "\x1bOA" : "\x1b[A")
                                                  : (app_keys ? "\x1bOB" : "\x1b[B");
    std::string keys;
    for (int i = 0; i < kWheelLines; ++i) keys += key;
    send_input(keys);
  } else if (wheel_up(button) && !send_scroll(sock_, ScrollOp::Up, kWheelLines)) {
    lost_ = true;
  }
}

void Passthrough::hold(std::string_view bytes) {
  if (bytes.empty()) return;
  if (held_.empty()) held_since_ = Clock::now();
  held_ += bytes;
}

void Passthrough::send_input(std::string_view bytes) {
  if (!bytes.empty() && !send_frame(sock_, bytes_frame(MsgType::Input, bytes))) lost_ = true;
}

void Passthrough::send_resize() {
  const Winsize ws = terminal_size(STDIN_FILENO);
  if (ws.rows == 0 || ws.cols == 0) return;
  if (!send_frame(sock_, make_frame(MsgType::Resize, PayloadWriter().u16(ws.rows).u16(ws.cols).take())))
    lost_ = true;
}

}  // namespace

void block_attach_signals() {
  const sigset_t set = attach_signals();
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

InputScan scan_input(std::string_view input, DetachKey key, bool mouse) {
  InputScan scan;
  std::size_t start = 0;  // first byte not yet copied to `forward`
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\x1c' && key == DetachKey::CtrlBackslash) {
      scan.forward.append(input.substr(start, i - start));
      scan.detach = true;
      return scan;
    }
    if (input[i] != '\x1b') continue;
    std::size_t len = 0;
    if (mouse) {
      int button = 0;
      const MouseKind kind = classify_mouse(input.substr(i), len, button);
      if (kind != MouseKind::Other) scan.forward.append(input.substr(start, i - start));
      if (kind == MouseKind::Partial) {
        scan.held = input.substr(i);
        return scan;
      }
      if (kind == MouseKind::Report) {
        scan.mouse = button;
        scan.consumed = i + len;
        return scan;
      }
    }
    switch (classify_key(input.substr(i), key, len)) {
      case KeyKind::Other:
        break;
      case KeyKind::Detach:
        scan.forward.append(input.substr(start, i - start));
        scan.detach = true;
        return scan;
      case KeyKind::Swallow:
        scan.forward.append(input.substr(start, i - start));
        i += len - 1;
        start = i + 1;
        break;
      case KeyKind::Partial:
        scan.forward.append(input.substr(start, i - start));
        scan.held = input.substr(i);
        return scan;
    }
  }
  scan.forward.append(input.substr(start));
  return scan;
}

AttachResult attach_session(int daemon_fd, std::uint32_t session_id, const Config& config,
                            std::string_view prelude) {
  AttachResult result;
  if (!isatty(STDIN_FILENO)) {
    result.error = "stdin is not a terminal";
    return result;
  }
  const auto started = Clock::now();

  const sigset_t mask = attach_signals();
  sigset_t saved_mask;
  pthread_sigmask(SIG_BLOCK, &mask, &saved_mask);
  UniqueFd sig_fd(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  auto restore_mask = [&] {
    sig_fd.reset();
    pthread_sigmask(SIG_SETMASK, &saved_mask, nullptr);
  };

  Winsize ws = terminal_size(STDIN_FILENO);
  if (ws.rows == 0 || ws.cols == 0) ws = {24, 80};
  FrameDecoder decoder;
  const char* tty = ttyname(STDIN_FILENO);
  const auto request = PayloadWriter().u32(session_id).u16(ws.rows).u16(ws.cols).str(tty ? tty : "").take();
  std::optional<Frame> reply;
  if (send_frame(daemon_fd, make_frame(MsgType::Attach, request)))
    reply = recv_frame(daemon_fd, decoder, 5000);
  if (!reply || reply->type != MsgType::Ok) {
    restore_mask();
    if (!reply) result.error = "no reply from daemon";
    else result.error = PayloadReader(reply->payload).str();
    if (result.error.empty()) result.error = "attach failed";
    return result;
  }

  // A private non-blocking handle on the terminal, so a slow terminal never blocks the loop
  // (O_NONBLOCK on stdout itself would affect every process sharing it).
  UniqueFd out_fd;
  if (isatty(STDOUT_FILENO)) out_fd.reset(open("/proc/self/fd/1", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));

  termios saved{};
  tcgetattr(STDIN_FILENO, &saved);
  termios raw = saved;
  cfmakeraw(&raw);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  if (!prelude.empty()) write_all(STDOUT_FILENO, prelude.data(), prelude.size());
  write_all(STDOUT_FILENO, kEnterAltScreen.data(), kEnterAltScreen.size());

  Passthrough passthrough(daemon_fd, decoder, sig_fd.get(), out_fd ? out_fd.get() : STDOUT_FILENO,
                          started + kReplyFilterTime, config);
  const Outcome outcome = passthrough.run();
  out_fd.reset();

  tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
  restore_terminal(passthrough.color_resets());
  restore_mask();

  result.wait_status = passthrough.exit_status();
  result.signal = passthrough.signal();
  switch (outcome) {
    case Outcome::Detached: result.outcome = AttachOutcome::Detached; break;
    case Outcome::AttachedElsewhere: result.outcome = AttachOutcome::AttachedElsewhere; break;
    case Outcome::Exited: result.outcome = AttachOutcome::Exited; break;
    case Outcome::Lost: result.outcome = AttachOutcome::Lost; break;
    case Outcome::Signaled: result.outcome = AttachOutcome::Signaled; break;
  }
  return result;
}

ViewResult view_session(int daemon_fd, std::uint32_t session_id, const Config& config) {
  ViewResult result;
  if (!isatty(STDIN_FILENO)) {
    result.error = "stdin is not a terminal";
    return result;
  }
  const auto filter_until = Clock::now() + kReplyFilterTime;
  termios saved{};
  tcgetattr(STDIN_FILENO, &saved);
  termios raw = saved;
  cfmakeraw(&raw);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  std::string prologue(kEnterAltScreen);
  prologue += kPmuxMouse;  // for the wheel
  write_all(STDOUT_FILENO, prologue.data(), prologue.size());

  Winsize ws = terminal_size(STDIN_FILENO);
  if (ws.rows == 0 || ws.cols == 0) ws = {24, 80};
  std::string resets;
  FrameDecoder decoder;
  bool shown = false;
  const auto request = PayloadWriter().u32(session_id).u16(ws.rows).u16(ws.cols).take();
  if (send_frame(daemon_fd, make_frame(MsgType::View, request))) {
    while (auto frame = recv_frame(daemon_fd, decoder, 5000)) {
      if (frame->type == MsgType::Snapshot) {
        write_all(STDOUT_FILENO, frame->payload.data(), frame->payload.size());
        continue;
      }
      if (frame->type == MsgType::Ok) {
        resets = PayloadReader(frame->payload).str();
        shown = true;
      } else if (frame->type == MsgType::Error) {
        result.error = PayloadReader(frame->payload).str();
      }
      break;
    }
  }
  if (shown) {
    // Navigation keys and the wheel scroll; any other key returns (clicks are ignored). A late
    // reply to the list screen's query is not a key.
    const std::string style = indicator_style(config);
    std::string pending;  // unhandled input: a possibly split key
    bool done = false;
    while (!done) {
      pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {daemon_fd, POLLIN, 0}};
      const int n = poll(fds, 2, pending.empty() ? -1 : int(kHoldTime.count()));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;  // a split key timed out: it is a key
      if (fds[1].revents) {
        char buf[65536];
        const ssize_t got = read(daemon_fd, buf, sizeof buf);
        if (got <= 0 && !(got < 0 && (errno == EINTR || errno == EAGAIN))) break;
        if (got > 0) decoder.feed(buf, std::size_t(got));
        while (auto frame = decoder.next()) {
          if (frame->type == MsgType::Snapshot) {
            write_all(STDOUT_FILENO, frame->payload.data(), frame->payload.size());
          } else if (frame->type == MsgType::State) {
            PayloadReader r(frame->payload);
            const bool scrolled = r.u8() & kStateScrolled;
            r.u16();
            const std::uint32_t offset = r.u32();
            const std::uint32_t total = r.u32();
            const std::string mark = scrolled && r.ok() ? indicator(style, offset, total) : "";
            write_all(STDOUT_FILENO, mark.data(), mark.size());
          }
        }
      }
      if (!fds[0].revents) continue;
      char buf[256];
      const ssize_t got = read(STDIN_FILENO, buf, sizeof buf);
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) break;
      pending.append(buf, std::size_t(got));
      std::string tail;
      if (Clock::now() < filter_until) tail = strip_list_replies(pending);
      while (!pending.empty() && !done) {
        std::size_t len = 0;
        const ScrollKey key = scroll_key(pending, len);
        if (key == ScrollKey::Partial) break;
        const auto op = scroll_op(key);
        if (key == ScrollKey::Other || key == ScrollKey::Exit) done = true;
        else if (op) send_scroll(daemon_fd, op->first, op->second);
        pending.erase(0, len);
      }
      pending += tail;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
    result.ok = true;
  } else if (result.error.empty()) {
    result.error = "lost connection to daemon";
  }

  tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
  restore_terminal(resets);
  return result;
}

int attach(int daemon_fd, std::uint32_t session_id, const std::string& name, const Config& config) {
  const AttachResult r = attach_session(daemon_fd, session_id, config);
  const int status = r.wait_status;
  switch (r.outcome) {
    case AttachOutcome::Detached:
      std::fprintf(stderr, "[detached from %s]\n", name.c_str());
      return 0;
    case AttachOutcome::AttachedElsewhere:
      std::fprintf(stderr, "[detached from %s: attached elsewhere]\n", name.c_str());
      return 0;
    case AttachOutcome::Exited:
      if (WIFSIGNALED(status))
        std::fprintf(stderr, "[%s killed by signal %d]\n", name.c_str(), WTERMSIG(status));
      else
        std::fprintf(stderr, "[%s exited: %d]\n", name.c_str(), WEXITSTATUS(status));
      return 0;
    case AttachOutcome::Signaled:
      return 128 + r.signal;
    case AttachOutcome::Failed:
      std::fprintf(stderr, "pmux: %s\n", r.error.c_str());
      return 1;
    case AttachOutcome::Lost:
      break;
  }
  std::fprintf(stderr, "pmux: lost connection to daemon\n");
  return 1;
}

}  // namespace pmux
