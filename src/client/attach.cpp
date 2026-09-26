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

// Kitty keyboard flags are popped on both screens: before leaving the alternate screen and
// again on the main screen.
constexpr std::string_view kModeResets =
    "\x1b[<99u"
    "\x1b[?1049l"
    "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1005l\x1b[?1006l\x1b[?1015l\x1b[?1016l"
    "\x1b[?1l\x1b>"
    "\x1b[?2004l"
    "\x1b[?1004l"
    "\x1b[?25h"
    "\x1b[?7h"
    "\x1b[0m"
    "\x1b[<99u"
    "\x1b[>4m";

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

// Classifies a complete CSI sequence (parameters, final byte). Detach keys: kitty
// `CSI 92;<m>[:<ev>]u` whose only modifier besides Caps Lock / Num Lock is Ctrl (press or no
// event type; repeat and release are swallowed) and xterm modifyOtherKeys `CSI 27;5;92~`.
KeyKind classify_csi(std::string_view params, char final) {
  if (final == '~') return params == "27;5;92" ? KeyKind::Detach : KeyKind::Other;
  if (final != 'u') return KeyKind::Other;
  const std::size_t semi = params.find(';');
  if (semi == std::string_view::npos) return KeyKind::Other;
  const std::string_view key = params.substr(0, semi);
  if (key != "92" && !key.starts_with("92:")) return KeyKind::Other;  // 92:<alternate keys>
  std::string_view mods = params.substr(semi + 1);
  mods = mods.substr(0, mods.find(';'));  // drop the associated-text field
  const std::size_t colon = mods.find(':');
  const std::string_view m = mods.substr(0, colon);
  const std::string_view ev = colon == std::string_view::npos ? "1" : mods.substr(colon + 1);
  if (!all_digits(m) || !all_digits(ev)) return KeyKind::Other;
  const int bits = to_int(m) - 1;
  if (bits < 0 || (bits & ~(64 | 128)) != 4) return KeyKind::Other;
  switch (to_int(ev)) {
    case 1: return KeyKind::Detach;
    case 2:
    case 3: return KeyKind::Swallow;
    default: return KeyKind::Other;
  }
}

// True if unfinished CSI parameters may still become a detach or swallowed key.
bool could_be_key(std::string_view params) {
  constexpr std::string_view kitty = "92", xterm = "27;5;92";
  return kitty.starts_with(params) || xterm.starts_with(params) || params.starts_with("92;") ||
         params.starts_with("92:");
}

// Classifies the key sequence at the start of `rest` (rest[0] == ESC); sets `len` to its
// length for Detach / Swallow. Partial: a split sequence that may still become one.
KeyKind classify_key(std::string_view rest, std::size_t& len) {
  if (rest.size() < 2 || rest[1] != '[') return KeyKind::Other;
  std::size_t i = 2;
  while (i < rest.size() && rest[i] >= 0x30 && rest[i] <= 0x3F) ++i;
  const std::string_view params = rest.substr(2, i - 2);
  if (i == rest.size()) return rest.size() >= 3 && could_be_key(params) ? KeyKind::Partial : KeyKind::Other;
  len = i + 1;
  return classify_csi(params, rest[i]);
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
  Passthrough(int sock, FrameDecoder& decoder, int sig_fd, int out_fd, Clock::time_point filter_until)
      : sock_(sock), decoder_(decoder), sig_fd_(sig_fd), out_fd_(out_fd), filter_until_(filter_until) {}

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
      if (out_pos_ > 0 && out_pos_ >= out_.size() / 2) {
        out_.erase(0, out_pos_);
        out_pos_ = 0;
      }
      out_.append(reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size());
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
  InputScan scan = scan_input(input);
  send_input(scan.forward);
  if (scan.detach) return Outcome::Detached;
  scan.held += reply_tail;
  if (!scan.held.empty()) {
    held_ = std::move(scan.held);
    held_since_ = Clock::now();
  }
  return lost_ ? std::optional(Outcome::Lost) : std::nullopt;
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

InputScan scan_input(std::string_view input) {
  InputScan scan;
  std::size_t start = 0;  // first byte not yet copied to `forward`
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\x1c') {
      scan.forward.append(input.substr(start, i - start));
      scan.detach = true;
      return scan;
    }
    if (input[i] != '\x1b') continue;
    std::size_t len = 0;
    switch (classify_key(input.substr(i), len)) {
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

bool find_detach_key(const char* buf, std::size_t len, std::size_t& pos) {
  const std::string_view input(buf, len);
  for (pos = 0; pos < len; ++pos) {
    std::size_t seq_len = 0;
    if (input[pos] == '\x1c') return true;
    if (input[pos] == '\x1b' && classify_key(input.substr(pos), seq_len) == KeyKind::Detach) return true;
  }
  pos = 0;
  return false;
}

AttachResult attach_session(int daemon_fd, std::uint32_t session_id, std::string_view prelude) {
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

  Passthrough passthrough(daemon_fd, decoder, sig_fd.get(), out_fd ? out_fd.get() : STDOUT_FILENO,
                          started + kReplyFilterTime);
  const Outcome outcome = passthrough.run();
  out_fd.reset();

  tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
  write_all(STDOUT_FILENO, kModeResets.data(), kModeResets.size());
  const std::string& resets = passthrough.color_resets();
  if (!resets.empty()) write_all(STDOUT_FILENO, resets.data(), resets.size());
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

ViewResult view_session(int daemon_fd, std::uint32_t session_id) {
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

  std::string resets;
  FrameDecoder decoder;
  bool shown = false;
  if (send_frame(daemon_fd, make_frame(MsgType::View, PayloadWriter().u32(session_id).take()))) {
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
    // Any key returns; a late reply to the list screen's query is not a key.
    char buf[256];
    std::string input;
    for (;;) {
      const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
      if (n < 0 && errno == EINTR) continue;
      if (n > 0 && Clock::now() < filter_until) {
        input.append(buf, std::size_t(n));
        std::string tail = strip_list_replies(input);
        if (input.empty()) {
          input = std::move(tail);
          continue;
        }
      }
      break;
    }
    tcflush(STDIN_FILENO, TCIFLUSH);
    result.ok = true;
  } else if (result.error.empty()) {
    result.error = "lost connection to daemon";
  }

  tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
  write_all(STDOUT_FILENO, kModeResets.data(), kModeResets.size());
  if (!resets.empty()) write_all(STDOUT_FILENO, resets.data(), resets.size());
  return result;
}

int attach(int daemon_fd, std::uint32_t session_id, const std::string& name) {
  const AttachResult r = attach_session(daemon_fd, session_id);
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
