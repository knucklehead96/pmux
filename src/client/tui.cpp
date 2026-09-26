#include "client/tui.hpp"

#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include "client/attach.hpp"
#include "client/connect.hpp"
#include "common/fd.hpp"
#include "common/protocol.hpp"

namespace pmux {

namespace {

using ftxui::Color;
using ftxui::Event;
using SteadyClock = std::chrono::steady_clock;

constexpr int kRequestTimeoutMs = 5000;
constexpr int kKillTimeoutMs = 10000;
constexpr auto kPollInterval = std::chrono::milliseconds(500);
constexpr auto kDoubleClick = std::chrono::milliseconds(400);
constexpr auto kLongNote = std::chrono::seconds(3);
constexpr int kMinCols = 40;  // smaller terminals only show "Terminal too small"
constexpr int kMinRows = 8;

// ---------------------------------------------------------------- theme

struct Rgb {
  double r = 0, g = 0, b = 0;  // 0..1
};

struct Theme {
  Color accent, secondary, running, bell, error, border, sel_bg;
  Color sel_fg = Color::Default;  // ansi only
};

Color to_color(const Rgb& c) {
  auto ch = [](double v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
  return Color::RGB(ch(c.r), ch(c.g), ch(c.b));
}

Rgb hex(std::uint32_t v) {
  return {((v >> 16) & 0xff) / 255.0, ((v >> 8) & 0xff) / 255.0, (v & 0xff) / 255.0};
}

Color hex_color(std::uint32_t v) {
  return Color::RGB(static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 8),
                    static_cast<std::uint8_t>(v));
}

double linear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relative_luminance(const Rgb& c) {
  return 0.2126 * linear(c.r) + 0.7152 * linear(c.g) + 0.0722 * linear(c.b);
}

// Parses "rgb:R/G/B" (1-4 hex digits per channel) from an OSC 11 reply.
std::optional<Rgb> parse_osc11(std::string_view reply) {
  const std::size_t at = reply.find("\x1b]11;rgb:");
  if (at == std::string_view::npos) return std::nullopt;
  std::size_t i = at + 9;
  double channels[3];
  for (int c = 0; c < 3; ++c) {
    if (c > 0) {
      if (i >= reply.size() || reply[i] != '/') return std::nullopt;
      ++i;
    }
    unsigned value = 0;
    int digits = 0;
    while (i < reply.size() && digits < 5 && std::isxdigit(static_cast<unsigned char>(reply[i]))) {
      const char ch = static_cast<char>(std::tolower(static_cast<unsigned char>(reply[i])));
      value = value * 16 + static_cast<unsigned>(ch <= '9' ? ch - '0' : ch - 'a' + 10);
      ++digits;
      ++i;
    }
    if (digits < 1 || digits > 4) return std::nullopt;
    channels[c] = value / (std::pow(16.0, digits) - 1);
  }
  return Rgb{channels[0], channels[1], channels[2]};
}

// True once `buf` holds a DA1 reply (ESC [ ? ... c).
bool has_da1_reply(std::string_view buf) {
  for (std::size_t at = buf.find("\x1b[?"); at != std::string_view::npos; at = buf.find("\x1b[?", at + 1)) {
    std::size_t i = at + 3;
    while (i < buf.size() && (std::isdigit(static_cast<unsigned char>(buf[i])) || buf[i] == ';')) ++i;
    if (i < buf.size() && buf[i] == 'c') return true;
  }
  return false;
}

// Turns the bytes read during the startup query into FTXUI events, in order: the terminal's
// replies (OSC / DCS / APC / PM / SOS strings, DA1 `CSI ? ... c`) are dropped, everything else
// was typed by the user meanwhile. Sequences are normalized the way FTXUI's own input parser
// does for the keys the list uses (CR -> Return, BS -> Backspace, SS3 arrows/Home/End -> CSI).
std::vector<Event> typed_events(std::string_view buf) {
  auto special = [](std::string seq) {
    if (seq == "\r") seq = "\n";
    else if (seq == "\x08") seq = "\x7f";
    else if (seq.size() == 3 && seq[1] == 'O' && std::strchr("ABCDHF", seq[2])) seq[1] = '[';
    return Event::Special(std::move(seq));
  };
  std::vector<Event> events;
  std::size_t i = 0;
  while (i < buf.size()) {
    const auto u = static_cast<unsigned char>(buf[i]);
    if (u == 0x1b && i + 1 < buf.size()) {
      const char kind = buf[i + 1];
      if (kind == '[') {
        std::size_t j = i + 2;
        while (j < buf.size() && buf[j] >= 0x30 && buf[j] <= 0x3F) ++j;
        while (j < buf.size() && buf[j] >= 0x20 && buf[j] <= 0x2F) ++j;
        if (j >= buf.size()) break;  // incomplete
        const bool da1 = i + 2 < j && buf[i + 2] == '?' && buf[j] == 'c';
        if (!da1) events.push_back(special(std::string(buf.substr(i, j + 1 - i))));
        i = j + 1;
      } else if (kind == ']' || kind == 'P' || kind == '_' || kind == '^' || kind == 'X') {
        std::size_t j = i + 2;
        while (j < buf.size() && buf[j] != '\a' && !(buf[j] == '\x1b' && j + 1 < buf.size() && buf[j + 1] == '\\')) ++j;
        i = j >= buf.size() ? j : j + (buf[j] == '\a' ? 1 : 2);
      } else if (kind == 'O' && i + 2 < buf.size()) {
        events.push_back(special(std::string(buf.substr(i, 3))));
        i += 3;
      } else {
        events.push_back(special(std::string(buf.substr(i, 2))));  // Alt+key
        i += 2;
      }
    } else if (u < 0x20 || u == 0x7f) {
      events.push_back(special(std::string(1, buf[i])));
      ++i;
    } else {
      const std::size_t n = u >= 0xF0 ? 4 : u >= 0xE0 ? 3 : u >= 0xC0 ? 2 : 1;
      events.push_back(Event::Character(std::string(buf.substr(i, n))));
      i += n;
    }
  }
  return events;
}

// Asks the terminal for its background color (OSC 11), fenced by DA1; waits up to 200 ms.
// Keys typed meanwhile are appended to `typed`.
std::optional<Rgb> query_background(std::vector<Event>& typed) {
  termios saved{};
  if (tcgetattr(STDIN_FILENO, &saved) != 0) return std::nullopt;
  termios raw = saved;
  cfmakeraw(&raw);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  constexpr std::string_view query = "\x1b]11;?\x1b\\\x1b[c";
  std::string buf;
  if (write_all(STDOUT_FILENO, query.data(), query.size())) {
    const auto deadline = SteadyClock::now() + std::chrono::milliseconds(200);
    while (!has_da1_reply(buf)) {
      const auto left =
          std::chrono::ceil<std::chrono::milliseconds>(deadline - SteadyClock::now()).count();
      if (left <= 0) break;
      pollfd pfd{STDIN_FILENO, POLLIN, 0};
      const int r = poll(&pfd, 1, static_cast<int>(left));
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      char chunk[256];
      const ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
      if (n <= 0) break;
      buf.append(chunk, std::size_t(n));
    }
  }
  tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  typed = typed_events(buf);
  return parse_osc11(buf);
}

Theme make_theme(const Config& config, std::vector<Event>& typed) {
  Theme t;
  if (config.theme == ThemeMode::Ansi) {
    t.accent = Color::Palette16(5);
    t.secondary = Color::Palette16(8);
    t.running = Color::Palette16(2);
    t.bell = Color::Palette16(3);
    t.error = Color::Palette16(1);
    t.border = Color::Palette16(8);
    t.sel_bg = Color::Palette16(4);
    t.sel_fg = Color::Palette16(15);
    return t;
  }

  std::optional<Rgb> bg;
  bool dark = config.theme != ThemeMode::Light;
  if (config.theme == ThemeMode::Auto) {
    bg = query_background(typed);
    dark = !bg || relative_luminance(*bg) < 0.5;
  }

  struct Pair {
    std::uint32_t dark, light;
  };
  auto pick = [&](Pair p) { return dark ? p.dark : p.light; };
  Pair accent{0xD97757, 0xC15F3C};
  switch (config.accent) {
    case Accent::Clay: break;
    case Accent::Blue: accent = {0x7AA2F7, 0x2F6FDB}; break;
    case Accent::Purple: accent = {0xB48EF0, 0x7C4DDB}; break;
    case Accent::Teal: accent = {0x5FC8C8, 0x1A8C8C}; break;
  }
  t.accent = hex_color(pick(accent));
  t.secondary = hex_color(pick({0x8B8B8B, 0x6B6B6B}));
  t.running = hex_color(pick({0x4EBA65, 0x2E8B47}));
  t.bell = hex_color(pick({0xE5B450, 0xB7791F}));
  t.error = hex_color(pick({0xE06C75, 0xC53030}));
  t.border = hex_color(pick({0x5C5C5C, 0xBDBDBD}));

  const Rgb sel = hex(pick({0x7AA2F7, 0x2F6FDB}));
  const Rgb base = bg ? *bg : hex(pick({0x1E1E1E, 0xFAFAF7}));
  const double a = dark ? 0.18 : 0.12;
  t.sel_bg = to_color({base.r * (1 - a) + sel.r * a, base.g * (1 - a) + sel.g * a,
                       base.b * (1 - a) + sel.b * a});
  return t;
}

// ---------------------------------------------------------------- text helpers

int text_width(const std::string& s) {
  return static_cast<int>(ftxui::Utf8ToGlyphs(s).size());
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) {
    if (!out.empty()) out += ' ';
    out += s;
  }
  return out;
}

std::string lower(std::string s) {
  for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

// Byte offset of a case-insensitive (ASCII) match, or npos.
std::size_t find_ci(const std::string& haystack, const std::string& needle_lower) {
  return lower(haystack).find(needle_lower);
}

void pop_glyph(std::string& s) {
  while (!s.empty()) {
    const auto ch = static_cast<unsigned char>(s.back());
    s.pop_back();
    if ((ch & 0xC0) != 0x80) break;
  }
}

// Longest prefix of `s` that fits in `width` columns; returns its byte length.
std::size_t fit_prefix(const std::string& s, int width) {
  const auto glyphs = ftxui::Utf8ToGlyphs(s);
  std::size_t bytes = 0;
  int cols = 0;
  for (std::size_t i = 0; i < glyphs.size(); ++i) {
    const int w = (i + 1 < glyphs.size() && glyphs[i + 1].empty()) ? 2 : 1;
    if (glyphs[i].empty()) continue;
    if (cols + w > width) break;
    cols += w;
    bytes += glyphs[i].size();
  }
  // Include zero-width bytes skipped by Utf8ToGlyphs? Glyph strings reproduce the input for
  // printable text, which is all we render.
  return bytes;
}

// Keeps the tail of `s` that fits in `width` columns.
std::string fit_tail(const std::string& s, int width) {
  auto glyphs = ftxui::Utf8ToGlyphs(s);
  int cols = 0;
  std::size_t start = glyphs.size();
  while (start > 0) {
    const int w = glyphs[start - 1].empty() ? 0 : 1;
    if (cols + w > width) break;
    cols += w;
    --start;
  }
  while (start < glyphs.size() && glyphs[start].empty()) ++start;
  std::string out;
  for (std::size_t i = start; i < glyphs.size(); ++i) out += glyphs[i];
  return out;
}

std::string abbreviate_home(const std::string& dir) {
  const std::string home = home_dir();
  if (home.size() > 1 && dir.starts_with(home) && (dir.size() == home.size() || dir[home.size()] == '/'))
    return "~" + dir.substr(home.size());
  return dir;
}

std::string format_duration(std::int64_t secs) {
  if (secs < 0) secs = 0;
  if (secs < 60) return std::to_string(secs) + "s";
  if (secs < 3600) return std::to_string(secs / 60) + "m";
  if (secs < 86400) return std::to_string(secs / 3600) + "h " + std::to_string(secs % 3600 / 60) + "m";
  return std::to_string(secs / 86400) + "d";
}

std::string signal_name(int sig) {
  if (const char* abbrev = sigabbrev_np(sig)) return std::string("SIG") + abbrev;
  return "signal " + std::to_string(sig);
}

// ---------------------------------------------------------------- canvas

struct Style {
  Color fg = Color::Default;
  Color bg = Color::Default;
  bool bold = false;
  bool dim = false;
  bool underline = false;
};

struct Cell {
  std::string ch = " ";
  Style style;
};

class Canvas {
 public:
  Canvas(int w, int h) : w_(w), h_(h), cells_(std::size_t(std::max(w, 0) * std::max(h, 0))) {}

  int width() const { return w_; }
  int height() const { return h_; }
  Cell& at(int x, int y) { return cells_[std::size_t(y * w_ + x)]; }
  const Cell& at(int x, int y) const { return cells_[std::size_t(y * w_ + x)]; }

  // Writes text at (x, y), clipped to [x, limit); returns the column after it.
  int put(int x, int y, const std::string& text, const Style& style, int limit = -1) {
    if (limit < 0 || limit > w_) limit = w_;
    if (y < 0 || y >= h_) return x;
    const auto glyphs = ftxui::Utf8ToGlyphs(text);
    for (std::size_t i = 0; i < glyphs.size() && x < limit; ++i) {
      const bool wide = i + 1 < glyphs.size() && glyphs[i + 1].empty();
      if (glyphs[i].empty()) continue;
      if (x < 0) {
        x += wide ? 2 : 1;
        continue;
      }
      if (wide && x + 1 >= limit) {
        at(x, y) = {" ", style};
        return x + 1;
      }
      at(x, y) = {glyphs[i], style};
      if (wide) at(x + 1, y) = {"", style};
      x += wide ? 2 : 1;
    }
    return x;
  }

  // Fills [x0, x1) of row y with blanks in `style`.
  void fill(int y, int x0, int x1, const Style& style) {
    if (y < 0 || y >= h_) return;
    for (int x = std::max(0, x0); x < std::min(x1, w_); ++x) at(x, y) = {" ", style};
  }

  void set_bg(int y, Color bg) {
    if (y < 0 || y >= h_) return;
    for (int x = 0; x < w_; ++x) at(x, y).style.bg = bg;
  }

  void dim_all() {
    for (auto& c : cells_) c.style.dim = true;
  }

 private:
  int w_, h_;
  std::vector<Cell> cells_;
};

class CanvasNode : public ftxui::Node {
 public:
  explicit CanvasNode(Canvas canvas) : canvas_(std::move(canvas)) {}

  void ComputeRequirement() override {
    requirement_.min_x = canvas_.width();
    requirement_.min_y = canvas_.height();
  }

  void Render(ftxui::Screen& screen) override {
    for (int y = 0; y < canvas_.height(); ++y) {
      for (int x = 0; x < canvas_.width(); ++x) {
        const int sx = box_.x_min + x, sy = box_.y_min + y;
        if (sx > box_.x_max || sy > box_.y_max) continue;
        const Cell& c = canvas_.at(x, y);
        ftxui::Pixel& p = screen.PixelAt(sx, sy);
        p.character = c.ch;
        p.foreground_color = c.style.fg;
        p.background_color = c.style.bg;
        p.bold = c.style.bold;
        p.dim = c.style.dim;
        p.underlined = c.style.underline;
      }
    }
    ftxui::Screen::Cursor cursor;
    cursor.shape = ftxui::Screen::Cursor::Hidden;
    screen.SetCursor(cursor);
  }

 private:
  Canvas canvas_;
};

// A run of styled text; lines are built from these.
struct Span {
  std::string text;
  Style style;
};

int spans_width(const std::vector<Span>& spans) {
  int w = 0;
  for (const auto& s : spans) w += text_width(s.text);
  return w;
}

// Writes spans starting at x, truncating with … to fit `max_width` columns.
int put_spans(Canvas& canvas, int x, int y, const std::vector<Span>& spans, int max_width, Style overlay_bg = {}) {
  auto with_bg = [&](Style s) {
    if (overlay_bg.bg != Color::Default) s.bg = overlay_bg.bg;
    if (overlay_bg.fg != Color::Default && s.fg == Color::Default) s.fg = overlay_bg.fg;
    return s;
  };
  if (max_width <= 0) return x;
  if (spans_width(spans) <= max_width) {
    for (const auto& s : spans) x = canvas.put(x, y, s.text, with_bg(s.style));
    return x;
  }
  int left = max_width - 1;
  for (const auto& s : spans) {
    const int w = text_width(s.text);
    if (w <= left) {
      x = canvas.put(x, y, s.text, with_bg(s.style));
      left -= w;
      continue;
    }
    x = canvas.put(x, y, s.text.substr(0, fit_prefix(s.text, left)), with_bg(s.style));
    return canvas.put(x, y, "…", with_bg(s.style));
  }
  return x;
}

// ---------------------------------------------------------------- daemon access

std::optional<Frame> request(int fd, const Frame& frame, int timeout_ms = kRequestTimeoutMs) {
  FrameDecoder decoder;
  if (fd < 0 || !send_frame(fd, frame)) return std::nullopt;
  return recv_frame(fd, decoder, timeout_ms);
}

std::string reply_error(const std::optional<Frame>& reply) {
  if (!reply) return "no reply from daemon";
  if (reply->type == MsgType::Error) return PayloadReader(reply->payload).str();
  return "unexpected reply from daemon";
}

std::optional<std::vector<ProcInfo>> fetch_list(int fd) {
  auto reply = request(fd, make_frame(MsgType::List));
  if (reply && reply->type == MsgType::ListReply) return decode_proc_list(reply->payload);
  return std::nullopt;
}

// Background LIST polling; hands results to the UI thread via PostEvent.
struct Poller {
  std::mutex m;
  std::condition_variable cv;
  bool stop = false;
  bool paused = false;  // while attached: FTXUI is uninstalled, don't post
  bool wake = false;
  std::optional<std::vector<ProcInfo>> latest;
  ftxui::ScreenInteractive* screen = nullptr;

  void request_refresh() {
    {
      std::lock_guard lock(m);
      wake = true;
    }
    cv.notify_all();
  }

  void run() {
    UniqueFd fd;
    for (;;) {
      {
        std::unique_lock lock(m);
        cv.wait_for(lock, kPollInterval, [&] { return stop || wake; });
        if (stop) return;
        wake = false;
        if (paused) continue;
      }
      if (!fd) fd = connect_daemon();
      auto list = fetch_list(fd.get());
      if (!list) fd.reset();
      std::lock_guard lock(m);
      if (stop) return;
      if (list) latest = std::move(list);
      if (!paused) screen->PostEvent(Event::Custom);
    }
  }
};

// ---------------------------------------------------------------- the list screen

enum class Status { Running, Idle, Bell, Exited, Signaled };

Status status_of(const ProcInfo& p) {
  if (p.exited) return WIFSIGNALED(p.wait_status) ? Status::Signaled : Status::Exited;
  if (p.bell) return Status::Bell;
  return p.idle_ms < 5000 ? Status::Running : Status::Idle;
}

struct Note {
  std::string text;
  bool error = false;
  SteadyClock::time_point until;
};

enum class Mode { List, Dialog, Rename, KillConfirm };

struct ListLine {
  enum Kind { Blank, Header, Row } kind = Blank;
  std::size_t group = 0;
  const ProcInfo* proc = nullptr;
};

struct Group {
  std::string dir;
  std::string display;
  std::vector<const ProcInfo*> rows;
};

class Tui {
 public:
  Tui(const Config& config, std::string launch_dir, Theme theme, ftxui::ScreenInteractive& screen,
      std::shared_ptr<Poller> poller, UniqueFd ctl)
      : config_(config),
        launch_dir_(std::move(launch_dir)),
        theme_(theme),
        screen_(screen),
        poller_(std::move(poller)),
        ctl_(std::move(ctl)) {}

  void set_procs(std::vector<ProcInfo> procs) {
    procs_ = std::move(procs);
    rebuild();
  }

  ftxui::Element render();
  bool on_event(const Event& event);
  // Events to handle before the first one FTXUI delivers (keys typed during startup).
  void set_pending(std::vector<Event> events) { pending_ = std::move(events); }

 private:
  bool handle_event(const Event& event);
  static bool too_small(int w, int h) { return w < kMinCols || h < kMinRows; }
  // model
  void rebuild();
  bool matches(const ProcInfo& p) const;
  const ProcInfo* find(std::uint32_t id) const;
  const ProcInfo* selected() const { return find(selected_); }
  void select_index(int index);
  int selected_index() const;
  void refresh_now();

  // actions
  void quit();
  void note(std::string text, bool error, SteadyClock::duration d);
  void attach_to(const ProcInfo& p, bool via_mouse);
  void attach_selected(bool via_mouse);
  void view_exited(const ProcInfo& p, bool via_mouse);
  void open_dialog();
  void submit_dialog();
  void start_rename();
  void submit_rename();
  void kill_selected();
  void confirm_kill();

  bool on_list_event(const Event& e);
  bool on_dialog_event(const Event& e);
  bool on_rename_event(const Event& e);
  bool on_kill_event(const Event& e);
  bool on_mouse(const Event& e);
  std::optional<Frame> ctl_request(const Frame& frame);

  // drawing
  void draw_header(Canvas& c);
  void draw_row(Canvas& c, int y, const ProcInfo& p, int name_w);
  void draw_footer(Canvas& c, int y);
  void draw_dialog(Canvas& c);
  std::vector<Span> keys(const std::vector<std::pair<std::string, std::string>>& pairs,
                         const std::string& sep) const;

  Style text() const { return {}; }
  Style secondary() const { return {theme_.secondary}; }
  Style accent(bool bold = false) const { return {theme_.accent, Color::Default, bold}; }
  Style error(bool bold = false) const { return {theme_.error, Color::Default, bold}; }

  const Config& config_;
  std::string launch_dir_;
  Theme theme_;
  ftxui::ScreenInteractive& screen_;
  std::shared_ptr<Poller> poller_;
  UniqueFd ctl_;

  std::vector<ProcInfo> procs_;
  std::vector<Group> groups_;
  std::vector<std::uint32_t> order_;  // visible rows, display order
  std::uint32_t selected_ = 0;
  int last_index_ = 0;
  std::string filter_;

  Mode mode_ = Mode::List;
  std::optional<Note> note_;

  // New process dialog
  std::string fields_[3];
  int focus_ = 0;
  std::string dialog_error_;

  // Rename
  std::string rename_buf_;
  std::uint32_t rename_id_ = 0;

  // Kill confirm
  std::uint32_t kill_id_ = 0;

  // Layout of the last frame
  int scroll_ = 0;
  int list_height_ = 1;
  std::vector<std::uint32_t> line_ids_;

  // Double-click detection
  std::uint32_t last_click_id_ = 0;
  SteadyClock::time_point last_click_;

  std::vector<Event> pending_;
};

const ProcInfo* Tui::find(std::uint32_t id) const {
  for (const auto& p : procs_)
    if (p.id == id) return &p;
  return nullptr;
}

bool Tui::matches(const ProcInfo& p) const {
  if (filter_.empty()) return true;
  const std::string f = lower(filter_);
  return find_ci(p.name, f) != std::string::npos ||
         find_ci(abbreviate_home(p.dir), f) != std::string::npos ||
         find_ci(join(p.argv), f) != std::string::npos || find_ci(p.fg_command, f) != std::string::npos;
}

void Tui::rebuild() {
  if (procs_.empty()) filter_.clear();
  std::map<std::string, Group> by_dir;
  for (const auto& p : procs_) {
    if (!matches(p)) continue;
    Group& g = by_dir[p.dir];
    g.dir = p.dir;
    g.rows.push_back(&p);
  }
  groups_.clear();
  order_.clear();
  for (auto& [dir, g] : by_dir) {
    g.display = abbreviate_home(dir);
    std::sort(g.rows.begin(), g.rows.end(), [](const ProcInfo* a, const ProcInfo* b) {
      return a->created_ms != b->created_ms ? a->created_ms < b->created_ms : a->id < b->id;
    });
    for (const ProcInfo* p : g.rows) order_.push_back(p->id);
    groups_.push_back(std::move(g));
  }

  if (order_.empty()) {
    if (!find(selected_)) selected_ = 0;
    return;
  }
  const auto it = std::find(order_.begin(), order_.end(), selected_);
  if (it != order_.end()) {
    last_index_ = int(it - order_.begin());
  } else if (find(selected_)) {
    // Filtered out: the first visible row takes over.
    selected_ = order_.front();
    last_index_ = 0;
  } else {
    // Gone: keep the same position.
    last_index_ = std::clamp(last_index_, 0, int(order_.size()) - 1);
    selected_ = order_[std::size_t(last_index_)];
  }
}

int Tui::selected_index() const {
  const auto it = std::find(order_.begin(), order_.end(), selected_);
  return it == order_.end() ? -1 : int(it - order_.begin());
}

void Tui::select_index(int index) {
  if (order_.empty()) return;
  index = std::clamp(index, 0, int(order_.size()) - 1);
  selected_ = order_[std::size_t(index)];
  last_index_ = index;
}

std::optional<Frame> Tui::ctl_request(const Frame& frame) {
  if (!ctl_) ctl_ = connect_daemon();
  auto reply = request(ctl_.get(), frame);
  if (!reply) {
    ctl_ = connect_daemon();
    reply = request(ctl_.get(), frame);
  }
  return reply;
}

void Tui::refresh_now() {
  auto reply = ctl_request(make_frame(MsgType::List));
  if (reply && reply->type == MsgType::ListReply)
    if (auto list = decode_proc_list(reply->payload)) set_procs(std::move(*list));
  poller_->request_refresh();
}

void Tui::quit() {
  {
    std::lock_guard lock(poller_->m);
    poller_->stop = true;
  }
  poller_->cv.notify_all();
  screen_.Exit();
}

void Tui::note(std::string text, bool error, SteadyClock::duration d) {
  note_ = Note{std::move(text), error, SteadyClock::now() + d};
}

void Tui::attach_to(const ProcInfo& proc, bool via_mouse) {
  const std::uint32_t id = proc.id;
  const std::string name = proc.name;
  UniqueFd fd = connect_daemon();
  if (!fd) {
    note("cannot connect to daemon", true, kLongNote);
    return;
  }
  {
    std::lock_guard lock(poller_->m);
    poller_->paused = true;
  }
  AttachResult result;
  screen_.WithRestoredIO([&] {
    if (via_mouse) {
      // Drop the rest of the click (release/motion reports) so the process never sees it.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      tcflush(STDIN_FILENO, TCIFLUSH);
    }
    result = attach_session(fd.get(), id);
  })();
  {
    std::lock_guard lock(poller_->m);
    poller_->paused = false;
  }
  fd.reset();

  note_.reset();
  switch (result.outcome) {
    case AttachOutcome::Detached: break;
    case AttachOutcome::AttachedElsewhere:
      note("detached from " + name + ": attached elsewhere", false, kLongNote);
      break;
    case AttachOutcome::Exited: {
      const int st = result.wait_status;
      if (WIFSIGNALED(st))
        note(name + " killed (" + signal_name(WTERMSIG(st)) + ")", true, kLongNote);
      else
        note(name + " exited: " + std::to_string(WEXITSTATUS(st)), WEXITSTATUS(st) != 0, kLongNote);
      break;
    }
    case AttachOutcome::Lost: note("lost connection to daemon", true, kLongNote); break;
    case AttachOutcome::Failed: note(result.error, true, kLongNote); break;
  }
  selected_ = id;
  refresh_now();
}

void Tui::attach_selected(bool via_mouse) {
  const ProcInfo* p = selected();
  if (!p) return;
  if (p->exited) {
    view_exited(*p, via_mouse);
    return;
  }
  attach_to(*p, via_mouse);
}

void Tui::view_exited(const ProcInfo& proc, bool via_mouse) {
  const std::uint32_t id = proc.id;
  UniqueFd fd = connect_daemon();
  if (!fd) {
    note("cannot connect to daemon", true, kLongNote);
    return;
  }
  {
    std::lock_guard lock(poller_->m);
    poller_->paused = true;
  }
  ViewResult result;
  screen_.WithRestoredIO([&] {
    if (via_mouse) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      tcflush(STDIN_FILENO, TCIFLUSH);
    }
    result = view_session(fd.get(), id);
  })();
  {
    std::lock_guard lock(poller_->m);
    poller_->paused = false;
  }
  fd.reset();
  note_.reset();
  if (!result.ok) note(result.error, true, kLongNote);
  selected_ = id;
  refresh_now();
}

void Tui::open_dialog() {
  std::string dir;
  if (const ProcInfo* p = selected(); p && !order_.empty()) dir = p->dir;
  else if (procs_.empty() && !config_.default_dir.empty()) dir = config_.default_dir;
  else dir = launch_dir_;
  std::string cmd = config_.default_cmd;
  if (cmd.empty()) {
    const char* shell = std::getenv("SHELL");
    cmd = shell && *shell ? shell : "/bin/sh";
  }
  fields_[0].clear();
  fields_[1] = abbreviate_home(dir);
  fields_[2] = cmd;
  focus_ = 0;
  dialog_error_.clear();
  mode_ = Mode::Dialog;
}

void Tui::submit_dialog() {
  auto trim = [](std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
  };
  const std::string name = trim(fields_[0]);
  const std::string dir = expand_value(trim(fields_[1]));
  const std::vector<std::string> argv = split_command(fields_[2]);
  if (argv.empty()) {
    dialog_error_ = "command is empty";
    return;
  }
  std::vector<std::string> env;
  for (char** e = environ; *e; ++e) env.emplace_back(*e);
  const auto size = ftxui::Terminal::Size();
  PayloadWriter w;
  w.str(name).str(dir).strs(argv).strs(env);
  w.u16(static_cast<std::uint16_t>(size.dimy)).u16(static_cast<std::uint16_t>(size.dimx));
  auto reply = ctl_request(make_frame(MsgType::New, w.take()));
  if (!reply || reply->type != MsgType::Ok) {
    dialog_error_ = reply_error(reply);
    return;
  }
  PayloadReader r(reply->payload);
  ProcInfo created;
  created.id = r.u32();
  created.name = r.str();
  mode_ = Mode::List;
  selected_ = created.id;
  refresh_now();
  attach_to(created, false);
}

void Tui::start_rename() {
  const ProcInfo* p = selected();
  if (!p) return;
  rename_id_ = p->id;
  rename_buf_ = p->name;
  note_.reset();
  mode_ = Mode::Rename;
}

void Tui::submit_rename() {
  auto reply = ctl_request(make_frame(MsgType::Rename, PayloadWriter().u32(rename_id_).str(rename_buf_).take()));
  if (!reply || reply->type != MsgType::Ok) {
    note(reply_error(reply), true, kLongNote);
    return;
  }
  mode_ = Mode::List;
  note_.reset();
  refresh_now();
}

void Tui::kill_selected() {
  const ProcInfo* p = selected();
  if (!p) return;
  if (p->exited) {
    auto reply = ctl_request(make_frame(MsgType::Remove, PayloadWriter().u32(p->id).take()));
    if (!reply || reply->type != MsgType::Ok) note(reply_error(reply), true, kLongNote);
    refresh_now();
    return;
  }
  kill_id_ = p->id;
  note_.reset();
  mode_ = Mode::KillConfirm;
}

void Tui::confirm_kill() {
  mode_ = Mode::List;
  const std::uint32_t id = kill_id_;
  std::thread([id, poller = poller_] {
    UniqueFd fd = connect_daemon();
    request(fd.get(), make_frame(MsgType::Kill, PayloadWriter().u32(id).take()), kKillTimeoutMs);
    poller->request_refresh();
  }).detach();
}

bool Tui::on_event(const Event& event) {
  if (!pending_.empty())
    for (const Event& e : std::exchange(pending_, {})) handle_event(e);
  return handle_event(event);
}

bool Tui::handle_event(const Event& event) {
  if (event == Event::Custom) {
    std::optional<std::vector<ProcInfo>> list;
    {
      std::lock_guard lock(poller_->m);
      list = std::exchange(poller_->latest, std::nullopt);
    }
    if (list) set_procs(std::move(*list));
    return true;
  }
  if (event == Event::CtrlC || event == Event::CtrlQ) {
    quit();
    return true;
  }
  if (event == Event::CtrlZ) return true;  // no job control: the poller must not race a suspend
  if (const auto size = ftxui::Terminal::Size(); too_small(size.dimx, size.dimy)) return true;
  if (event.is_mouse()) return on_mouse(event);
  switch (mode_) {
    case Mode::List: return on_list_event(event);
    case Mode::Dialog: return on_dialog_event(event);
    case Mode::Rename: return on_rename_event(event);
    case Mode::KillConfirm: return on_kill_event(event);
  }
  return false;
}

bool Tui::on_list_event(const Event& e) {
  const int index = selected_index();
  const int page = std::max(1, list_height_ - 1);
  if (e == Event::ArrowUp) select_index(index - 1);
  else if (e == Event::ArrowDown) select_index(index + 1);
  else if (e == Event::PageUp) select_index(index - page);
  else if (e == Event::PageDown) select_index(index + page);
  else if (e == Event::Home) select_index(0);
  else if (e == Event::End) select_index(int(order_.size()) - 1);
  else if (e == Event::Return) attach_selected(false);
  else if (e == Event::CtrlN) open_dialog();
  else if (e == Event::CtrlR) start_rename();
  else if (e == Event::CtrlX) kill_selected();
  else if (e == Event::Escape) {
    filter_.clear();
    rebuild();
  } else if (e == Event::Backspace) {
    pop_glyph(filter_);
    rebuild();
  } else if (e.is_character()) {
    if (procs_.empty()) return true;
    filter_ += e.character();
    rebuild();
  } else {
    return false;
  }
  return true;
}

bool Tui::on_dialog_event(const Event& e) {
  std::string& field = fields_[focus_];
  if (e == Event::Escape) mode_ = Mode::List;
  else if (e == Event::Return) submit_dialog();
  else if (e == Event::Tab || e == Event::ArrowDown) focus_ = (focus_ + 1) % 3;
  else if (e == Event::TabReverse || e == Event::ArrowUp) focus_ = (focus_ + 2) % 3;
  else if (e == Event::Backspace) pop_glyph(field);
  else if (e.is_character()) field += e.character();
  return true;
}

bool Tui::on_rename_event(const Event& e) {
  if (e == Event::Escape) {
    mode_ = Mode::List;
    note_.reset();
  } else if (e == Event::Return) {
    submit_rename();
  } else if (e == Event::Backspace) {
    pop_glyph(rename_buf_);
  } else if (e.is_character()) {
    rename_buf_ += e.character();
  }
  return true;
}

bool Tui::on_kill_event(const Event& e) {
  if (e == Event::Character('y') || e == Event::Character('Y')) confirm_kill();
  else if (e == Event::Character('n') || e == Event::Character('N') || e == Event::Escape) mode_ = Mode::List;
  return true;
}

bool Tui::on_mouse(const Event& e) {
  if (mode_ != Mode::List) return true;
  Event copy = e;  // Event::mouse() is non-const
  const ftxui::Mouse m = copy.mouse();
  if (m.motion != ftxui::Mouse::Pressed) return true;
  if (m.button == ftxui::Mouse::WheelUp) {
    select_index(selected_index() - 1);
  } else if (m.button == ftxui::Mouse::WheelDown) {
    select_index(selected_index() + 1);
  } else if (m.button == ftxui::Mouse::Left) {
    const std::uint32_t id = m.y >= 0 && m.y < int(line_ids_.size()) ? line_ids_[std::size_t(m.y)] : 0;
    if (id == 0) return true;
    const auto now = SteadyClock::now();
    const bool dbl = id == last_click_id_ && now - last_click_ <= kDoubleClick;
    selected_ = id;
    last_index_ = std::max(0, selected_index());
    if (dbl) {
      last_click_id_ = 0;
      attach_selected(true);
    } else {
      last_click_id_ = id;
      last_click_ = now;
    }
  }
  return true;
}

std::vector<Span> Tui::keys(const std::vector<std::pair<std::string, std::string>>& pairs,
                            const std::string& sep) const {
  std::vector<Span> out;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (i > 0) out.push_back({sep, secondary()});
    out.push_back({pairs[i].first, text()});
    out.push_back({" " + pairs[i].second, secondary()});
  }
  return out;
}

void Tui::draw_header(Canvas& c) {
  c.put(1, 0, "✻ pmux", accent(true));
  if (procs_.empty()) return;
  int running = 0, idle = 0, exited = 0;
  for (const auto& p : procs_) {
    switch (status_of(p)) {
      case Status::Running:
      case Status::Bell: ++running; break;
      case Status::Idle: ++idle; break;
      case Status::Exited:
      case Status::Signaled: ++exited; break;
    }
  }
  std::string counts;
  auto add = [&](int n, const char* what) {
    if (n == 0) return;
    if (!counts.empty()) counts += " · ";
    counts += std::to_string(n) + " " + what;
  };
  add(running, "running");
  add(idle, "idle");
  add(exited, "exited");
  const int x = c.width() - 1 - text_width(counts);
  if (x > 8) c.put(x, 0, counts, secondary());
}

void Tui::draw_row(Canvas& c, int y, const ProcInfo& p, int name_w) {
  const bool sel = p.id == selected_;
  const int w = c.width();
  Style base;
  if (sel) {
    base.bg = theme_.sel_bg;
    base.fg = theme_.sel_fg;
  }
  c.fill(y, 0, w, base);
  auto st = [&](Style s) {
    s.bg = base.bg;
    if (s.fg == Color::Default) s.fg = base.fg;
    return s;
  };

  const std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
  const Status status = status_of(p);
  const char* glyph = "●";
  Color glyph_color = theme_.running;
  switch (status) {
    case Status::Running: break;
    case Status::Idle: glyph = "◌", glyph_color = theme_.secondary; break;
    case Status::Bell: glyph = "!", glyph_color = theme_.bell; break;
    case Status::Exited:
    case Status::Signaled: glyph = "○", glyph_color = theme_.secondary; break;
  }
  c.put(3, y, glyph, st({glyph_color}));

  // Name column.
  const int nx = 5;
  if (mode_ == Mode::Rename && p.id == rename_id_) {
    Style u = st({});
    u.underline = true;
    const int x = c.put(nx, y, fit_tail(rename_buf_, name_w - 1), u);
    c.put(x, y, "▏", st({theme_.accent}));
  } else {
    Style name_style = st({});
    name_style.bold = sel;
    std::string shown = p.name;
    std::size_t prefix = shown.size();
    if (text_width(shown) > name_w) {
      prefix = fit_prefix(shown, name_w - 1);
      shown = shown.substr(0, prefix) + "…";
    }
    std::size_t mb = std::string::npos, me = 0;
    if (!filter_.empty()) {
      mb = find_ci(p.name, lower(filter_));
      if (mb != std::string::npos) me = mb + filter_.size();
    }
    Style hl = st({theme_.accent});
    hl.bold = sel;
    hl.underline = true;
    int x = nx;
    std::size_t off = 0;
    const auto glyphs = ftxui::Utf8ToGlyphs(shown);
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
      if (glyphs[i].empty()) continue;
      const bool in_match = mb != std::string::npos && off >= mb && off < me && off < prefix;
      x = c.put(x, y, glyphs[i], in_match ? hl : name_style);
      off += glyphs[i].size();
    }
  }

  // Age, right-aligned ending at column w-2.
  const std::string age = format_duration(now - std::int64_t(p.created));
  const int age_x = w - 1 - text_width(age);
  const int dx = nx + name_w + 2;
  if (age_x > dx) c.put(age_x, y, age, st(secondary()));

  // Detail.
  std::vector<Span> detail;
  const std::string fg = p.fg_command.empty() ? join(p.argv) : p.fg_command;
  switch (status) {
    case Status::Running: detail = {{fg, text()}}; break;
    case Status::Idle: {
      const std::int64_t since = p.last_output ? std::int64_t(p.last_output) : std::int64_t(p.created);
      detail = {{fg, text()}, {" · idle " + format_duration(now - since), secondary()}};
      break;
    }
    case Status::Bell: detail = {{fg, text()}, {" · bell", secondary()}}; break;
    case Status::Exited: {
      const int code = WEXITSTATUS(p.wait_status);
      detail = {{"exited (" + std::to_string(code) + ")", code == 0 ? secondary() : error()}};
      break;
    }
    case Status::Signaled:
      detail = {{"killed (" + signal_name(WTERMSIG(p.wait_status)) + ")", error()}};
      break;
  }
  put_spans(c, dx, y, detail, age_x - 2 - dx, base);
}

void Tui::draw_footer(Canvas& c, int y) {
  std::vector<Span> spans{{" ", text()}};
  auto append = [&](std::vector<Span> more) { spans.insert(spans.end(), more.begin(), more.end()); };
  if (note_ && SteadyClock::now() >= note_->until) note_.reset();

  if (note_ && mode_ != Mode::KillConfirm) {
    spans.push_back({note_->text, note_->error ? error() : secondary()});
  } else if (mode_ == Mode::Rename) {
    append(keys({{"enter", "save"}, {"esc", "cancel"}}, " · "));
  } else if (mode_ == Mode::KillConfirm) {
    const ProcInfo* p = find(kill_id_);
    const std::string name = p ? p->name : "?";
    const std::string cmd = p ? join(p->argv) : "";
    spans.push_back({"Kill", error(true)});
    spans.push_back({" " + name + " (" + cmd + ")?  ", text()});
    append(keys({{"y", "yes"}, {"n", "no"}}, " · "));
  } else if (procs_.empty()) {
    append(keys({{"^n", "new"}, {"^q", "quit"}}, "  "));
  } else if (!filter_.empty()) {
    append(keys({{"↑↓", "select"}, {"enter", "attach"}, {"esc", "clear filter"}}, "  "));
  } else {
    append(keys({{"↑↓", "select"}, {"enter", "attach"}, {"^n", "new"}, {"^r", "rename"}, {"^x", "kill"}, {"^q", "quit"}},
                "  "));
    if (procs_.size() > 8) spans.push_back({"  type to filter", secondary()});
  }
  put_spans(c, 0, y, spans, c.width() - 1);
}

void Tui::draw_dialog(Canvas& c) {
  const int W = c.width(), H = c.height();
  const int iw = std::clamp(std::max(44, W / 2), 20, 72);
  const int inner = std::min(iw, W - 2);
  if (inner < 12 || H < 8) return;
  const int left = (W - inner - 2) / 2;
  const int top = std::max(0, (H - 8) / 2);
  const Style bd{theme_.border};
  const Style clear{};

  auto blank_row = [&](int y) {
    c.fill(y, left, left + inner + 2, clear);
    c.put(left, y, "│", bd);
    c.put(left + inner + 1, y, "│", bd);
  };
  // Top border with the title.
  c.fill(top, left, left + inner + 2, clear);
  std::string rule = " ";
  for (int i = 0; i < inner - 14; ++i) rule += "─";
  int x = c.put(left, top, "╭─ ", bd);
  x = c.put(x, top, "New process", accent(true));
  c.put(x, top, rule, bd, left + inner + 1);
  c.put(left + inner + 1, top, "╮", bd);

  blank_row(top + 1);
  const char* labels[3] = {"Name", "Dir", "Command"};
  for (int i = 0; i < 3; ++i) {
    const int y = top + 2 + i;
    blank_row(y);
    c.put(left + 3, y, labels[i], secondary());
    int fx = left + 3 + 9;
    const int room = left + inner + 1 - fx - 1;  // keep one blank before the border
    const bool focused = i == focus_;
    const std::string shown = fit_tail(fields_[i], focused ? room - 1 : room);
    fx = c.put(fx, y, shown, text(), left + inner);
    if (focused) c.put(fx, y, "▏", accent(), left + inner + 1);
  }
  blank_row(top + 5);
  if (!dialog_error_.empty())
    put_spans(c, left + 3, top + 5, {{dialog_error_, error()}}, inner - 4);
  c.fill(top + 6, left, left + inner + 2, clear);
  std::string bottom = "╰";
  for (int i = 0; i < inner; ++i) bottom += "─";
  bottom += "╯";
  c.put(left, top + 6, bottom, bd);
  c.fill(top + 7, left, left + inner + 2, clear);
  c.put(left + 2, top + 7, "enter create · tab next · esc cancel", secondary(), left + inner + 2);
}

ftxui::Element Tui::render() {
  const auto size = ftxui::Terminal::Size();
  const int W = std::max(size.dimx, 1), H = std::max(size.dimy, 1);
  Canvas c(W, H);
  line_ids_.assign(std::size_t(H), 0);

  if (too_small(W, H)) {
    const std::string msg = "Terminal too small", hint = "^q quit";
    const int y = std::max(0, (H - 2) / 2);
    c.put(std::max(0, (W - text_width(msg)) / 2), y, msg, text());
    c.put(std::max(0, (W - text_width(hint)) / 2), y + 1, hint, secondary());
    return std::make_shared<CanvasNode>(std::move(c));
  }

  draw_header(c);
  if (procs_.empty()) {
    c.put(3, 3, "No processes yet.", text());
    int x = c.put(3, 4, "Press ", secondary());
    x = c.put(x, 4, "^n", text());
    c.put(x, 4, " to start one.", secondary());
  } else {
    int top = 2;
    if (!filter_.empty()) {
      int x = c.put(1, 2, "›", accent());
      x = c.put(x, 2, " ", text());
      x = c.put(x, 2, fit_tail(filter_, W - 5), text());
      c.put(x, 2, "▏", accent());
      top = 4;
    }

    int name_w = 0;
    for (const auto& p : procs_) name_w = std::max(name_w, text_width(p.name));
    name_w = std::clamp(name_w, 12, 24);

    std::vector<ListLine> lines;
    int sel_line = -1;
    for (std::size_t g = 0; g < groups_.size(); ++g) {
      if (g > 0) lines.push_back({ListLine::Blank, g, nullptr});
      lines.push_back({ListLine::Header, g, nullptr});
      for (const ProcInfo* p : groups_[g].rows) {
        if (p->id == selected_) sel_line = int(lines.size());
        lines.push_back({ListLine::Row, g, p});
      }
    }

    const int avail = std::max(1, H - 2 - top);
    list_height_ = avail;
    if (sel_line >= 0) {
      int want_top = sel_line;
      if (sel_line > 0 && lines[std::size_t(sel_line - 1)].kind == ListLine::Header) want_top = sel_line - 1;
      if (want_top < scroll_) scroll_ = want_top;
      if (sel_line >= scroll_ + avail) scroll_ = sel_line - avail + 1;
    }
    scroll_ = std::clamp(scroll_, 0, std::max(0, int(lines.size()) - avail));

    for (int i = 0; i < avail && scroll_ + i < int(lines.size()); ++i) {
      const ListLine& l = lines[std::size_t(scroll_ + i)];
      const int y = top + i;
      if (y >= H - 1) break;
      if (l.kind == ListLine::Header) {
        const Group& g = groups_[l.group];
        Style bold;
        bold.bold = true;
        const std::string count = " · " + std::to_string(g.rows.size());
        const int x = put_spans(c, 1, y, {{g.display, bold}}, W - 2 - text_width(count));
        c.put(x, y, count, secondary());
      } else if (l.kind == ListLine::Row) {
        draw_row(c, y, *l.proc, name_w);
        line_ids_[std::size_t(y)] = l.proc->id;
      }
    }
  }
  if (H >= 2) draw_footer(c, H - 1);

  if (mode_ == Mode::Dialog) {
    c.dim_all();
    draw_dialog(c);
  }
  return std::make_shared<CanvasNode>(std::move(c));
}

}  // namespace

int run_tui(const Config& config, const std::string& launch_dir) {
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    std::fprintf(stderr, "pmux: the process list needs a terminal (stdin and stdout must be a tty)\n");
    return 1;
  }
  UniqueFd ctl = connect_or_spawn_daemon();
  if (!ctl) {
    std::fprintf(stderr, "pmux: cannot connect to daemon\n");
    return 1;
  }
  auto initial = fetch_list(ctl.get());
  if (!initial) {
    std::fprintf(stderr, "pmux: no reply from daemon\n");
    return 1;
  }

  const char* colorterm = std::getenv("COLORTERM");
  if (colorterm && (std::strstr(colorterm, "truecolor") || std::strstr(colorterm, "24bit")))
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
  std::vector<Event> typed;
  const Theme theme = make_theme(config, typed);

  // stdout on a tty is line buffered; emit each frame in one write instead of one per line.
  std::setvbuf(stdout, nullptr, _IOFBF, 1 << 18);
  auto screen = ftxui::ScreenInteractive::Fullscreen();
  screen.ForceHandleCtrlC(false);
  screen.ForceHandleCtrlZ(false);

  auto poller = std::make_shared<Poller>();
  poller->screen = &screen;

  Tui tui(config, launch_dir, theme, screen, poller, std::move(ctl));
  tui.set_procs(std::move(*initial));
  tui.set_pending(std::move(typed));

  auto component = ftxui::Renderer([&] { return tui.render(); });
  component |= ftxui::CatchEvent([&](Event e) { return tui.on_event(e); });

  std::thread worker([poller] { poller->run(); });
  {
    ftxui::Loop loop(&screen, component);
    // Deliver the keys typed during startup now; they precede anything FTXUI reads itself.
    screen.PostEvent(Event::Custom);
    loop.Run();
  }
  {
    std::lock_guard lock(poller->m);
    poller->stop = true;
  }
  poller->cv.notify_all();
  worker.join();
  return 0;
}

}  // namespace pmux
