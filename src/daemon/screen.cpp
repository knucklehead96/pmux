#include "daemon/screen.hpp"

#include <vterm.h>

#include <algorithm>
#include <charconv>
#include <string_view>

namespace pmux {

namespace {

// ---------------------------------------------------------------------------
// Helpers

constexpr std::uint32_t kWideCont = 0xFFFFFFFF;  // libvterm's marker for the right half of a wide char
constexpr std::size_t kMaxParams = 256;
constexpr std::size_t kMaxOsc = 1 << 16;
constexpr std::size_t kMaxKittyStack = 64;
constexpr std::size_t kSnapshotReserve = 1 << 16;

enum PenFlag : std::uint16_t {
  kBold = 1 << 0,
  kItalic = 1 << 1,
  kBlink = 1 << 2,
  kReverse = 1 << 3,
  kConceal = 1 << 4,
  kStrike = 1 << 5,
};

void append_int(std::string& out, long v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr);
}

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x110000) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += "\xEF\xBF\xBD";
  }
}

void append_cup(std::string& out, int row, int col) {
  out += "\x1b[";
  append_int(out, row + 1);
  out += ';';
  append_int(out, col + 1);
  out += 'H';
}

std::uint32_t pack_color(const VTermColor& c) {
  if (VTERM_COLOR_IS_INDEXED(&c)) return std::uint32_t(c.type) | std::uint32_t(c.indexed.idx) << 8;
  return std::uint32_t(c.type) | std::uint32_t(c.rgb.red) << 8 | std::uint32_t(c.rgb.green) << 16 |
         std::uint32_t(c.rgb.blue) << 24;
}

VTermColor unpack_color(std::uint32_t v) {
  VTermColor c{};
  c.type = static_cast<std::uint8_t>(v);
  if (VTERM_COLOR_IS_INDEXED(&c)) {
    c.indexed.idx = static_cast<std::uint8_t>(v >> 8);
  } else {
    c.rgb.red = static_cast<std::uint8_t>(v >> 8);
    c.rgb.green = static_cast<std::uint8_t>(v >> 16);
    c.rgb.blue = static_cast<std::uint8_t>(v >> 24);
  }
  return c;
}

bool is_default_fg(std::uint32_t packed) {
  return packed & VTERM_COLOR_DEFAULT_FG;
}

bool is_default_bg(std::uint32_t packed) {
  return packed & VTERM_COLOR_DEFAULT_BG;
}

bool plain(const Screen::Pen& pen) {
  return pen.flags == 0 && pen.underline == 0 && is_default_fg(pen.fg) && is_default_bg(pen.bg);
}

Screen::Pen pen_of(const VTermScreenCell& cell) {
  Screen::Pen pen;
  const auto& a = cell.attrs;
  pen.flags = std::uint16_t((a.bold ? kBold : 0) | (a.italic ? kItalic : 0) | (a.blink ? kBlink : 0) |
                            (a.reverse ? kReverse : 0) | (a.conceal ? kConceal : 0) | (a.strike ? kStrike : 0));
  pen.underline = static_cast<std::uint8_t>(a.underline);
  pen.fg = pack_color(cell.fg);
  pen.bg = pack_color(cell.bg);
  return pen;
}

// Small fixed buffer for building one SGR sequence without repeated string appends.
struct SgrBuf {
  char data[96];
  std::size_t n = 0;
  void put(std::string_view s) {
    for (char ch : s) data[n++] = ch;
  }
  void num(int v) {
    const auto r = std::to_chars(data + n, data + sizeof data, v);
    n = std::size_t(r.ptr - data);
  }
};

void put_color(SgrBuf& b, std::uint32_t packed, bool fg) {
  if (fg ? is_default_fg(packed) : is_default_bg(packed)) return;
  const VTermColor c = unpack_color(packed);
  if (VTERM_COLOR_IS_INDEXED(&c)) {
    const int idx = c.indexed.idx;
    if (idx < 8) {
      b.put(fg ? ";3" : ";4");
      b.num(idx);
    } else if (idx < 16) {
      b.put(fg ? ";9" : ";10");
      b.num(idx - 8);
    } else {
      b.put(fg ? ";38;5;" : ";48;5;");
      b.num(idx);
    }
  } else {
    b.put(fg ? ";38;2;" : ";48;2;");
    b.num(c.rgb.red);
    b.put(";");
    b.num(c.rgb.green);
    b.put(";");
    b.num(c.rgb.blue);
  }
}

// Full SGR for a pen, starting from a reset.
void append_sgr(std::string& out, const Screen::Pen& pen) {
  SgrBuf b;
  b.put("\x1b[0");
  if (pen.flags & kBold) b.put(";1");
  if (pen.flags & kItalic) b.put(";3");
  if (pen.underline == 1) {
    b.put(";4");
  } else if (pen.underline > 1) {
    b.put(";4:");
    b.num(pen.underline);
  }
  if (pen.flags & kBlink) b.put(";5");
  if (pen.flags & kReverse) b.put(";7");
  if (pen.flags & kConceal) b.put(";8");
  if (pen.flags & kStrike) b.put(";9");
  put_color(b, pen.fg, true);
  put_color(b, pen.bg, false);
  b.put("m");
  out.append(b.data, b.n);
}

// A cell that looks the same as an erased one.
bool blank(std::uint32_t ch, const Screen::Pen& pen) {
  return (ch == 0 || ch == ' ') && is_default_bg(pen.bg) && !(pen.flags & (kReverse | kStrike)) &&
         pen.underline == 0;
}

Screen::Line line_from_cells(const VTermScreenCell* cells, int cols) {
  Screen::Line line;
  line.chars.resize(std::size_t(cols));
  for (int c = 0; c < cols; ++c) {
    const VTermScreenCell& cell = cells[c];
    line.chars[std::size_t(c)] = cell.chars[0];
    if (cell.chars[0] != 0 && cell.chars[0] != kWideCont && cell.chars[1] != 0) {
      std::u32string extra;
      for (int i = 1; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; ++i) extra += char32_t(cell.chars[i]);
      line.combining.emplace_back(std::uint16_t(c), std::move(extra));
    }
    const Screen::Pen pen = pen_of(cell);
    if (line.runs.empty() || !(line.runs.back().second == pen)) line.runs.emplace_back(std::uint16_t(c), pen);
  }
  // Trim trailing blank cells.
  std::size_t end = line.chars.size();
  std::size_t run = line.runs.size();
  while (end > 0) {
    while (run > 0 && line.runs[run - 1].first >= end) --run;
    const std::size_t col = end - 1;
    if (!blank(line.chars[col], line.runs[run - 1].second)) break;
    if (!line.combining.empty() && line.combining.back().first == col) break;
    end = col;
  }
  line.chars.resize(end);
  while (!line.runs.empty() && line.runs.back().first >= end) line.runs.pop_back();
  line.chars.shrink_to_fit();
  line.runs.shrink_to_fit();
  return line;
}

// Appends the line's text with SGR, ending with SGR 0.
void render_line(std::string& out, const Screen::Line& line) {
  std::size_t run = 0;
  std::size_t comb = 0;
  const Screen::Pen* cur = nullptr;
  bool reset = true;  // the terminal pen is SGR 0
  for (std::size_t col = 0; col < line.chars.size(); ++col) {
    const std::uint32_t ch = line.chars[col];
    if (ch == kWideCont) continue;
    while (run + 1 < line.runs.size() && line.runs[run + 1].first <= col) ++run;
    const Screen::Pen& pen = line.runs[run].second;
    if (!cur || !(*cur == pen)) {
      if (reset && plain(pen)) {
        // already in effect
      } else {
        append_sgr(out, pen);
        reset = plain(pen);
      }
      cur = &pen;
    }
    append_utf8(out, ch ? ch : ' ');
    while (comb < line.combining.size() && line.combining[comb].first < col) ++comb;
    if (comb < line.combining.size() && line.combining[comb].first == col)
      for (char32_t extra : line.combining[comb].second) append_utf8(out, extra);
  }
  out += "\x1b[0m";
}

// ---------------------------------------------------------------------------
// libvterm callbacks

int cb_settermprop(VTermProp prop, VTermValue* val, void* user) {
  if (prop == VTERM_PROP_ALTSCREEN) static_cast<Screen*>(user)->set_alt(val->boolean);
  return 1;
}

int cb_bell(void* user) {
  static_cast<Screen*>(user)->ring_bell();
  return 1;
}

int cb_sb_pushline(int cols, const VTermScreenCell* cells, void* user) {
  static_cast<Screen*>(user)->push_line(line_from_cells(cells, cols));
  return 1;
}

int cb_sb_clear(void* user) {
  static_cast<Screen*>(user)->clear_scrollback();
  return 1;
}

void cb_output(const char* s, std::size_t len, void* user) {
  static_cast<Screen*>(user)->add_reply(s, len);
}

}  // namespace

// Defined below; it needs Screen's scrollback and default colors.
static int cb_sb_popline(int cols, VTermScreenCell* cells, void* user);

namespace {

const VTermScreenCallbacks kCallbacks = {
    nullptr,           // damage
    nullptr,           // moverect
    nullptr,           // movecursor
    cb_settermprop,    // settermprop
    cb_bell,           // bell
    nullptr,           // resize
    cb_sb_pushline,    // sb_pushline
    cb_sb_popline,     // sb_popline
    cb_sb_clear,       // sb_clear
};

}  // namespace

// ---------------------------------------------------------------------------
// ModeTracker

void ModeTracker::reset() {
  alt_ = false;
  cursor_keys_ = false;
  autowrap_ = true;
  cursor_visible_ = true;
  keypad_ = false;
  focus_ = false;
  paste_ = false;
  mouse_ = 0;
  mouse_1005_ = mouse_1006_ = mouse_1015_ = false;
  cursor_shape_ = -1;
  kitty_main_.assign(1, 0);
  kitty_alt_.assign(1, 0);
  title_.reset();
  palette_.clear();
  for (auto& c : dyn_colors_) c.reset();
}

std::size_t ModeTracker::feed(const char* data, std::size_t len) {
  for (std::size_t i = 0; i < len; ++i) {
    const char ch = data[i];
    const auto u = static_cast<unsigned char>(ch);
    switch (state_) {
      case State::Ground:
        if (ch == '\x1b') state_ = State::Esc;
        break;
      case State::Esc:
        if (ch == '[') {
          state_ = State::Csi;
          params_.clear();
          intermediates_.clear();
          prefix_ = 0;
        } else if (ch == ']') {
          state_ = State::Osc;
          osc_.clear();
          osc_overflow_ = false;
        } else if (ch == 'P' || ch == 'X' || ch == '^' || ch == '_') {
          state_ = State::String;
        } else if (u >= 0x20 && u <= 0x2F) {
          state_ = State::EscIntermediate;
        } else if (u >= 0x30 && u <= 0x7E) {
          state_ = State::Ground;
          dispatch_esc(ch);
        } else if (ch == '\x18' || ch == '\x1a') {
          state_ = State::Ground;
        }
        // ESC restarts; other C0 controls execute without leaving the sequence.
        break;
      case State::EscIntermediate:
        if (ch == '\x1b') state_ = State::Esc;
        else if (u >= 0x30 && u <= 0x7E) state_ = State::Ground;
        else if (ch == '\x18' || ch == '\x1a') state_ = State::Ground;
        break;
      case State::Csi:
        if (ch == '\x1b') {
          state_ = State::Esc;
        } else if (ch == '\x18' || ch == '\x1a') {
          state_ = State::Ground;
        } else if (u >= 0x30 && u <= 0x3F) {
          if (params_.empty() && intermediates_.empty() && prefix_ == 0 && (ch == '<' || ch == '=' || ch == '>' || ch == '?'))
            prefix_ = ch;
          else if (params_.size() < kMaxParams)
            params_ += ch;
        } else if (u >= 0x20 && u <= 0x2F) {
          if (intermediates_.size() < 4) intermediates_ += ch;
        } else if (u >= 0x40 && u <= 0x7E) {
          state_ = State::Ground;
          dispatch_csi(ch);
          if (alt_requested_) return i + 1;
        }
        break;
      case State::Osc:
        if (ch == '\a') {
          state_ = State::Ground;
          dispatch_osc();
        } else if (ch == '\x1b') {
          state_ = State::OscEsc;
        } else if (ch == '\x18' || ch == '\x1a') {
          state_ = State::Ground;
        } else if (osc_.size() < kMaxOsc) {
          osc_ += ch;
        } else {
          osc_overflow_ = true;
        }
        break;
      case State::OscEsc:
        state_ = State::Ground;
        dispatch_osc();
        if (ch != '\\') {
          state_ = State::Esc;
          --i;
        }
        break;
      case State::String:
        if (ch == '\x1b') state_ = State::StringEsc;
        else if (ch == '\a' || ch == '\x18' || ch == '\x1a') state_ = State::Ground;
        break;
      case State::StringEsc:
        if (ch == '\\') {
          state_ = State::Ground;
        } else {
          state_ = State::Esc;
          --i;
        }
        break;
    }
  }
  return len;
}

void ModeTracker::dispatch_esc(char final) {
  switch (final) {
    case 'c': reset(); break;
    case '=': keypad_ = true; break;
    case '>': keypad_ = false; break;
    default: break;
  }
}

namespace {

// Parses "a;b;c" (sub-parameters after ':' are ignored); empty params are -1.
std::vector<long> parse_params(const std::string& s) {
  std::vector<long> out;
  long cur = -1;
  bool sub = false;
  for (const char ch : s) {
    if (ch == ';') {
      out.push_back(cur);
      cur = -1;
      sub = false;
    } else if (ch == ':') {
      sub = true;
    } else if (!sub && ch >= '0' && ch <= '9') {
      cur = (cur < 0 ? 0 : cur) * 10 + (ch - '0');
      if (cur > 1000000) cur = 1000000;
    }
  }
  out.push_back(cur);
  return out;
}

long param(const std::vector<long>& p, std::size_t i, long def) {
  return i < p.size() && p[i] >= 0 ? p[i] : def;
}

}  // namespace

void ModeTracker::set_private_mode(int mode, bool on) {
  switch (mode) {
    case 1: cursor_keys_ = on; break;
    case 7: autowrap_ = on; break;
    case 25: cursor_visible_ = on; break;
    case 47:
    case 1047:
    case 1049:
      if (on) alt_requested_ = true;
      alt_ = on;
      break;
    case 1000:
    case 1002:
    case 1003: mouse_ = on ? mode : 0; break;
    case 1004: focus_ = on; break;
    case 1005: mouse_1005_ = on; break;
    case 1006: mouse_1006_ = on; break;
    case 1015: mouse_1015_ = on; break;
    case 2004: paste_ = on; break;
    default: break;
  }
}

void ModeTracker::dispatch_csi(char final) {
  const auto p = parse_params(params_);
  if ((final == 'h' || final == 'l') && prefix_ == '?' && intermediates_.empty()) {
    for (long m : p)
      if (m >= 0) set_private_mode(int(m), final == 'h');
  } else if (final == 'q' && prefix_ == 0 && intermediates_ == " ") {
    cursor_shape_ = int(param(p, 0, 0));
  } else if (final == 'p' && prefix_ == 0 && intermediates_ == "!") {
    // DECSTR
    cursor_keys_ = false;
    keypad_ = false;
    cursor_visible_ = true;
    autowrap_ = true;
  } else if (final == 'u' && intermediates_.empty()) {
    auto& stack = kitty_stack();
    if (prefix_ == '>') {
      if (stack.size() >= kMaxKittyStack) stack.erase(stack.begin());
      stack.push_back(std::uint32_t(param(p, 0, 0)));
    } else if (prefix_ == '<') {
      const long n = param(p, 0, 1);
      for (long k = 0; k < n && !stack.empty(); ++k) stack.pop_back();
      if (stack.empty()) stack.push_back(0);
    } else if (prefix_ == '=') {
      const auto flags = std::uint32_t(param(p, 0, 0));
      switch (param(p, 1, 1)) {
        case 1: stack.back() = flags; break;
        case 2: stack.back() |= flags; break;
        case 3: stack.back() &= ~flags; break;
        default: break;
      }
    }
  }
}

void ModeTracker::dispatch_osc() {
  if (osc_overflow_) return;
  const std::size_t semi = osc_.find(';');
  const std::string num = osc_.substr(0, semi);
  if (num.empty() || num.size() > 4 || num.find_first_not_of("0123456789") != std::string::npos) return;
  const int ps = std::stoi(num);
  const std::string pt = semi == std::string::npos ? std::string() : osc_.substr(semi + 1);

  std::vector<std::string> fields;
  if (semi != std::string::npos) {
    std::size_t start = 0;
    for (;;) {
      const std::size_t next = pt.find(';', start);
      fields.push_back(pt.substr(start, next - start));
      if (next == std::string::npos) break;
      start = next + 1;
    }
  }

  switch (ps) {
    case 0:
    case 2: title_ = pt; break;
    case 4:
      for (std::size_t k = 0; k + 1 < fields.size(); k += 2) {
        const auto idx = parse_params(fields[k]);
        if (idx.size() != 1 || idx[0] < 0 || idx[0] > 255 || fields[k + 1] == "?") continue;
        palette_[int(idx[0])] = fields[k + 1];
      }
      break;
    case 10:
    case 11:
    case 12:
      for (std::size_t k = 0; k < fields.size() && ps - 10 + int(k) < 3; ++k)
        if (fields[k] != "?") dyn_colors_[std::size_t(ps - 10) + k] = fields[k];
      break;
    case 104:
      if (fields.empty() || (fields.size() == 1 && fields[0].empty())) {
        palette_.clear();
      } else {
        for (const auto& f : fields) {
          const auto idx = parse_params(f);
          if (idx.size() == 1 && idx[0] >= 0) palette_.erase(int(idx[0]));
        }
      }
      break;
    case 110:
    case 111:
    case 112: dyn_colors_[std::size_t(ps - 110)].reset(); break;
    default: break;
  }
}

std::string ModeTracker::restore_modes(bool input) const {
  std::string out;
  if (input) out += restore_input_modes();
  if (!autowrap_) out += "\x1b[?7l";
  if (cursor_shape_ >= 0) {
    out += "\x1b[";
    append_int(out, cursor_shape_);
    out += " q";
  }
  out += cursor_visible_ ? "\x1b[?25h" : "\x1b[?25l";
  return out;
}

std::string ModeTracker::restore_input_modes() const {
  std::string out;
  if (cursor_keys_) out += "\x1b[?1h";
  if (keypad_) out += "\x1b=";
  if (mouse_) {
    out += "\x1b[?";
    append_int(out, mouse_);
    out += 'h';
  }
  if (mouse_1005_) out += "\x1b[?1005h";
  if (mouse_1006_) out += "\x1b[?1006h";
  if (mouse_1015_) out += "\x1b[?1015h";
  if (focus_) out += "\x1b[?1004h";
  if (paste_) out += "\x1b[?2004h";
  return out;
}

std::string ModeTracker::restore_kitty(bool alt) const {
  const auto& stack = alt ? kitty_alt_ : kitty_main_;
  std::string out;
  if (stack.back() != 0) {
    out += "\x1b[>";
    append_int(out, long(stack.back()));
    out += 'u';
  }
  return out;
}

std::string ModeTracker::restore_title_and_colors() const {
  std::string out;
  if (title_) out += "\x1b]2;" + *title_ + "\x1b\\";
  for (const auto& [idx, spec] : palette_) {
    out += "\x1b]4;";
    append_int(out, idx);
    out += ';' + spec + "\x1b\\";
  }
  for (int k = 0; k < 3; ++k) {
    if (!dyn_colors_[k]) continue;
    out += "\x1b]";
    append_int(out, 10 + k);
    out += ';' + *dyn_colors_[k] + "\x1b\\";
  }
  return out;
}

std::string ModeTracker::color_resets() const {
  std::string out;
  for (const auto& entry : palette_) {
    out += "\x1b]104;";
    append_int(out, entry.first);
    out += "\x1b\\";
  }
  for (int k = 0; k < 3; ++k) {
    if (!dyn_colors_[k]) continue;
    out += "\x1b]";
    append_int(out, 110 + k);
    out += "\x1b\\";
  }
  return out;
}

// ---------------------------------------------------------------------------
// Screen

static int cb_sb_popline(int cols, VTermScreenCell* cells, void* user) {
  auto* screen = static_cast<Screen*>(user);
  Screen::Line line;
  if (!screen->pop_line(line)) return 0;
  const VTermColor dfg = unpack_color(screen->default_fg());
  const VTermColor dbg = unpack_color(screen->default_bg());
  std::size_t run = 0;
  std::size_t comb = 0;
  for (int c = 0; c < cols; ++c) {
    VTermScreenCell& cell = cells[c];
    cell = VTermScreenCell{};
    const auto col = std::size_t(c);
    if (col < line.chars.size()) {
      std::uint32_t ch = line.chars[col];
      if (ch == kWideCont && (col == 0 || line.chars[col - 1] == kWideCont)) ch = 0;
      cell.chars[0] = ch;
      while (comb < line.combining.size() && line.combining[comb].first < col) ++comb;
      if (comb < line.combining.size() && line.combining[comb].first == col) {
        const auto& extra = line.combining[comb].second;
        for (std::size_t i = 0; i < extra.size() && i + 1 < VTERM_MAX_CHARS_PER_CELL; ++i)
          cell.chars[i + 1] = extra[i];
      }
      cell.width = (col + 1 < line.chars.size() && line.chars[col + 1] == kWideCont) ? 2 : 1;
      while (run + 1 < line.runs.size() && line.runs[run + 1].first <= col) ++run;
      const Screen::Pen& pen = line.runs[run].second;
      cell.attrs.bold = (pen.flags & kBold) != 0;
      cell.attrs.italic = (pen.flags & kItalic) != 0;
      cell.attrs.blink = (pen.flags & kBlink) != 0;
      cell.attrs.reverse = (pen.flags & kReverse) != 0;
      cell.attrs.conceal = (pen.flags & kConceal) != 0;
      cell.attrs.strike = (pen.flags & kStrike) != 0;
      cell.attrs.underline = pen.underline & 3u;
      cell.fg = unpack_color(pen.fg);
      cell.bg = unpack_color(pen.bg);
    } else {
      cell.width = 1;
      cell.fg = dfg;
      cell.bg = dbg;
    }
  }
  return 1;
}

Screen::Screen(int rows, int cols, std::size_t scrollback_lines)
    : rows_(std::max(rows, 1)), cols_(std::max(cols, 1)), capacity_(scrollback_lines) {
  vt_ = vterm_new(rows_, cols_);
  vterm_set_utf8(vt_, 1);
  vterm_output_set_callback(vt_, cb_output, this);
  screen_ = vterm_obtain_screen(vt_);
  state_ = vterm_obtain_state(vt_);
  vterm_screen_set_callbacks(screen_, &kCallbacks, this);
  vterm_screen_enable_altscreen(screen_, 1);
  vterm_screen_enable_reflow(screen_, true);
  vterm_screen_reset(screen_, 1);
}

Screen::~Screen() {
  if (vt_) vterm_free(vt_);
}

std::uint32_t Screen::default_fg() const {
  VTermColor fg, bg;
  vterm_state_get_default_colors(state_, &fg, &bg);
  return pack_color(fg);
}

std::uint32_t Screen::default_bg() const {
  VTermColor fg, bg;
  vterm_state_get_default_colors(state_, &fg, &bg);
  return pack_color(bg);
}

void Screen::feed(const char* data, std::size_t len) {
  while (len > 0) {
    const std::size_t n = modes_.feed(data, len);
    if (modes_.take_alt_request()) {
      // data[n - 1] is the final byte of a sequence that switches to the alternate screen.
      // Save the primary screen first: libvterm cannot read it while the alternate is active.
      vterm_input_write(vt_, data, n - 1);
      if (!alt_) {
        saved_primary_.clear();
        for (int r = 0; r < rows_; ++r) saved_primary_.push_back(read_row(r));
        vterm_state_get_cursorpos(state_, &saved_cursor_);
      }
      vterm_input_write(vt_, data + n - 1, 1);
    } else {
      vterm_input_write(vt_, data, n);
    }
    data += n;
    len -= n;
  }
}

void Screen::resize(int rows, int cols) {
  if (rows <= 0 || cols <= 0 || (rows == rows_ && cols == cols_)) return;
  rows_ = rows;
  cols_ = cols;
  vterm_set_size(vt_, rows, cols);
}

void Screen::push_line(Line line) {
  if (capacity_ == 0) return;
  while (scrollback_.size() >= capacity_) scrollback_.pop_front();
  scrollback_.push_back(std::move(line));
}

bool Screen::pop_line(Line& line) {
  if (scrollback_.empty()) return false;
  line = std::move(scrollback_.back());
  scrollback_.pop_back();
  return true;
}

Screen::Line Screen::read_row(int row) const {
  std::vector<VTermScreenCell> cells(static_cast<std::size_t>(cols_));
  for (int c = 0; c < cols_; ++c) vterm_screen_get_cell(screen_, VTermPos{row, c}, &cells[std::size_t(c)]);
  return line_from_cells(cells.data(), cols_);
}

std::string Screen::snapshot(bool interactive) {
  std::string out;
  out.reserve(kSnapshotReserve);
  // Soft reset (never RIS), hide the cursor while painting, clear screen and scrollback.
  out += "\x1b[!p\x1b[0m\x1b[H\x1b[2J\x1b[3J\x1b[?7h\x1b[?25l";

  // History, oldest first, as a flowing stream so it ends up in the native scrollback.
  bool first = true;
  auto next_line = [&] {
    if (!first) out += "\r\n";
    first = false;
  };
  for (const Line& line : scrollback_) {
    next_line();
    render_line(out, line);
  }

  std::vector<Line> rows;
  rows.reserve(std::size_t(rows_));
  for (int r = 0; r < rows_; ++r) rows.push_back(read_row(r));

  if (!alt_) {
    for (const Line& line : rows) {
      next_line();
      render_line(out, line);
    }
  } else {
    // The primary screen is not readable while the alternate screen is active: use the copy
    // saved when the app switched, so the terminal's DECSC and primary screen match.
    for (const Line& line : saved_primary_) {
      next_line();
      render_line(out, line);
    }
    if (!saved_primary_.empty())
      append_cup(out, std::clamp(saved_cursor_.row, 0, rows_ - 1), std::clamp(saved_cursor_.col, 0, cols_ - 1));
    if (interactive) out += modes_.restore_kitty(false);  // the main screen has its own kitty flags stack
    out += "\x1b[?1049h\x1b[H\x1b[2J";
  }
  // Repaint the visible rows at exact positions.
  for (int r = 0; r < rows_; ++r) {
    append_cup(out, r, 0);
    out += "\x1b[2K";
    render_line(out, rows[std::size_t(r)]);
  }

  VTermPos cursor{};
  vterm_state_get_cursorpos(state_, &cursor);
  append_cup(out, std::clamp(cursor.row, 0, rows_ - 1), std::clamp(cursor.col, 0, cols_ - 1));
  out += modes_.restore_modes(interactive);
  if (interactive) out += modes_.restore_kitty(alt_);
  out += modes_.restore_title_and_colors();

  // The app's current pen.
  VTermScreenCell pen_cell{};
  VTermValue v;
  vterm_state_get_penattr(state_, VTERM_ATTR_BOLD, &v);
  pen_cell.attrs.bold = v.boolean;
  vterm_state_get_penattr(state_, VTERM_ATTR_ITALIC, &v);
  pen_cell.attrs.italic = v.boolean;
  vterm_state_get_penattr(state_, VTERM_ATTR_BLINK, &v);
  pen_cell.attrs.blink = v.boolean;
  vterm_state_get_penattr(state_, VTERM_ATTR_REVERSE, &v);
  pen_cell.attrs.reverse = v.boolean;
  vterm_state_get_penattr(state_, VTERM_ATTR_CONCEAL, &v);
  pen_cell.attrs.conceal = v.boolean;
  vterm_state_get_penattr(state_, VTERM_ATTR_STRIKE, &v);
  pen_cell.attrs.strike = v.boolean;
  vterm_state_get_penattr(state_, VTERM_ATTR_UNDERLINE, &v);
  pen_cell.attrs.underline = unsigned(v.number) & 3u;
  vterm_state_get_penattr(state_, VTERM_ATTR_FOREGROUND, &v);
  pen_cell.fg = v.color;
  vterm_state_get_penattr(state_, VTERM_ATTR_BACKGROUND, &v);
  pen_cell.bg = v.color;
  const Pen pen = pen_of(pen_cell);
  if (!plain(pen)) append_sgr(out, pen);
  return out;
}

}  // namespace pmux
