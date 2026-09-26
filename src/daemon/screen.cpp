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
// Turns off every mode a snapshot may turn on that DECSTR leaves alone (DECSTR resets the cursor
// keys, keypad, cursor visibility, autowrap, origin mode, insert mode and margins).
constexpr std::string_view kModeOffs =
    "\x1b[?9l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1005l\x1b[?1006l\x1b[?1015l\x1b[?1016l"
    "\x1b[?1004l\x1b[?2004l\x1b[>4m\x1b[0 q\x1b[?1l\x1b>";

enum PenFlag : std::uint16_t {
  kBold = 1 << 0,
  kItalic = 1 << 1,
  kBlink = 1 << 2,
  kReverse = 1 << 3,
  kConceal = 1 << 4,
  kStrike = 1 << 5,
  kFaint = 1 << 6,  // libvterm font 1, see SgrRewriter
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
                            (a.reverse ? kReverse : 0) | (a.conceal ? kConceal : 0) | (a.strike ? kStrike : 0) |
                            (a.font == 1 ? kFaint : 0));
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

// Full SGR for a pen, starting from a reset. For libvterm (`vterm`), faint is font 1 (SGR 11).
void append_sgr(std::string& out, const Screen::Pen& pen, bool vterm = false) {
  SgrBuf b;
  b.put("\x1b[0");
  if (pen.flags & kBold) b.put(";1");
  if (pen.flags & kFaint) b.put(vterm ? ";11" : ";2");
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
void render_line(std::string& out, const Screen::Line& line, bool vterm = false) {
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
        append_sgr(out, pen, vterm);
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

// The line cut to `cols` cells (a wide character whose right half is cut becomes a blank).
Screen::Line truncated(const Screen::Line& line, int cols) {
  const auto n = std::size_t(cols);
  if (line.chars.size() <= n) return line;
  Screen::Line out;
  out.chars.assign(line.chars.begin(), line.chars.begin() + std::ptrdiff_t(n));
  if (n > 0 && line.chars[n] == kWideCont) out.chars[n - 1] = 0;
  for (const auto& run : line.runs)
    if (run.first < n) out.runs.push_back(run);
  for (const auto& comb : line.combining)
    if (comb.first < n && out.chars[comb.first] != 0) out.combining.push_back(comb);
  return out;
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
  mouse_1005_ = mouse_1006_ = mouse_1015_ = mouse_1016_ = false;
  cursor_shape_ = -1;
  modify_other_keys_ = 0;
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
          if (cursor_restore_) return i + 1;
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
          if (alt_requested_ || alt_exit_ || cursor_restore_) return i + 1;
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
    case 'c':
      ris_color_resets_ += color_resets();
      reset();
      break;
    case '8': cursor_restore_ = true; break;
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
      else alt_exit_ = std::max(alt_exit_, mode == 1049 ? 2 : 1);
      if (!on && mode == 1049) cursor_restore_ = true;
      alt_ = on;
      break;
    case 1048:
      if (!on) cursor_restore_ = true;
      break;
    case 9:
    case 1000:
    case 1002:
    case 1003: mouse_ = on ? mode : 0; break;
    case 1004: focus_ = on; break;
    case 1005: mouse_1005_ = on; break;
    case 1006: mouse_1006_ = on; break;
    case 1015: mouse_1015_ = on; break;
    case 1016: mouse_1016_ = on; break;
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
  } else if ((final == 'm' || final == 'n') && prefix_ == '>' && intermediates_.empty()) {
    // xterm modifyOtherKeys: CSI > 4 ; n m sets it, CSI > 4 m resets it, CSI > 4 n disables it.
    if (param(p, 0, -1) == 4) modify_other_keys_ = final == 'm' ? int(std::clamp(param(p, 1, 0), 0L, 3L)) : 0;
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
  if (mouse_1016_) out += "\x1b[?1016h";
  if (focus_) out += "\x1b[?1004h";
  if (paste_) out += "\x1b[?2004h";
  if (modify_other_keys_) {
    out += "\x1b[>4;";
    append_int(out, modify_other_keys_);
    out += 'm';
  }
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

std::string ModeTracker::set_kitty(bool alt) const {
  std::string out = "\x1b[=";
  append_int(out, long((alt ? kitty_alt_ : kitty_main_).back()));
  out += ";1u";
  return out;
}

ClientState ModeTracker::client_state() const {
  ClientState st;
  st.mouse = std::uint16_t(mouse_);
  if (mouse_1006_) st.flags |= kStateMouseSgr;
  if (mouse_1005_) st.flags |= kStateMouseUtf8;
  if (mouse_1015_) st.flags |= kStateMouseUrxvt;
  if (mouse_1016_) st.flags |= kStateMousePixels;
  if (cursor_keys_) st.flags |= kStateCursorKeys;
  return st;
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
// SgrRewriter

namespace {

constexpr std::size_t kMaxCsi = 4096;
// CSI_ARGS_MAX in libvterm: 0.3.3 writes past its argument array (and crashes) on more.
constexpr int kVtermMaxArgs = 16;

bool c0(unsigned char u) {
  return u < 0x20;
}

// Value of an SGR parameter element ("" = 0); -1 if not a plain number.
long sgr_value(std::string_view s) {
  long v = 0;
  for (const char ch : s) {
    if (ch < '0' || ch > '9') return -1;
    v = std::min(v * 10 + (ch - '0'), 1000000L);
  }
  return v;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  for (;;) {
    const std::size_t at = s.find(sep);
    out.push_back(s.substr(0, at));
    if (at == std::string_view::npos) return out;
    s.remove_prefix(at + 1);
  }
}

// Rewritten SGR parameters as self-contained items (a color with its arguments is one item);
// empty if every parameter was dropped.
std::vector<std::string> rewrite_sgr(std::string_view params) {
  const auto groups = split(params, ';');
  std::vector<std::string> out;
  for (std::size_t i = 0; i < groups.size(); ++i) {
    const auto elems = split(groups[i], ':');
    const long v = sgr_value(elems[0]);
    if (elems.size() > 1) {
      // Colon form: one self-contained group.
      if (v == 58) continue;
      if ((v == 38 || v == 48) && elems.size() == 6 && sgr_value(elems[1]) == 2) {
        // 38:2:<color space>:r:g:b -> 38:2:r:g:b (libvterm reads the color space as red).
        out.push_back(std::string(elems[0]) + ":2:" + std::string(elems[3]) + ':' + std::string(elems[4]) +
                      ':' + std::string(elems[5]));
        continue;
      }
    } else if (v == 38 || v == 48 || v == 58) {
      // Semicolon form: the following parameters belong to the color (as libvterm counts them).
      std::size_t n = 0;
      if (i + 1 < groups.size()) {
        const long mode = sgr_value(split(groups[i + 1], ':')[0]);
        n = 1 + (mode == 5 ? 1 : mode == 2 ? 3 : 0);
      }
      n = std::min(n, groups.size() - 1 - i);
      if (v != 58) {
        std::string item(groups[i]);
        for (std::size_t k = 1; k <= n; ++k) (item += ';') += groups[i + k];
        out.push_back(std::move(item));
      }
      i += n;
      continue;
    }
    if (v == 2) out.emplace_back("11");
    else if (v == 22) out.emplace_back("22;10");
    else out.emplace_back(groups[i]);
  }
  return out;
}

}  // namespace

bool SgrRewriter::keep_param_byte(char ch) {
  if (ch == ';' || ch == ':') ++separators_;
  return separators_ < kVtermMaxArgs;
}

void SgrRewriter::finish_csi(char final, std::string& out) {
  if (final != 'm' || !rewritable_) {
    out += raw_;  // at most kVtermMaxArgs arguments
    return;
  }
  out += c0_;
  // SGRs longer than libvterm's argument limit are split into several at item boundaries.
  int args = 0;
  for (const std::string& item : rewrite_sgr(params_)) {
    const int n = 1 + int(std::count_if(item.begin(), item.end(), [](char c) { return c == ';' || c == ':'; }));
    if (n > kVtermMaxArgs) continue;
    if (args > 0 && args + n > kVtermMaxArgs) {
      out += 'm';
      args = 0;
    }
    out += args == 0 ? "\x1b[" : ";";
    out += item;
    args += n;
  }
  if (args > 0) out += 'm';
}

void SgrRewriter::feed(const char* data, std::size_t len, std::string& out) {
  out.reserve(out.size() + len + 16);
  for (std::size_t i = 0; i < len; ++i) {
    const char ch = data[i];
    const auto u = static_cast<unsigned char>(ch);
    switch (state_) {
      case State::Ground:
        if (ch == '\x1b') state_ = State::Esc;  // held until the next byte
        else out += ch;
        break;
      case State::Esc:
        if (ch == '[') {
          state_ = State::Csi;
          raw_.assign("\x1b[");
          params_.clear();
          separators_ = 0;
          c0_.clear();
          rewritable_ = true;
        } else if ((c0(u) && ch != '\x1b' && ch != '\x18' && ch != '\x1a') || u == 0x7F) {
          out += ch;  // C0 executes (DEL is ignored) without leaving the escape sequence
        } else if (ch == 'c') {
          out += "\x1b[?1049l\x1b" "c";  // RIS; libvterm's would stay in the alternate screen
          state_ = State::Ground;
        } else {
          out += '\x1b';
          if (ch == '\x1b') break;  // restarts; the new ESC is held
          out += ch;
          if (ch == ']' || ch == 'P' || ch == 'X' || ch == '^' || ch == '_') state_ = State::String;
          else if (u >= 0x20 && u <= 0x2F) state_ = State::EscIntermediate;
          else state_ = State::Ground;
        }
        break;
      case State::EscIntermediate:
        if (ch == '\x1b') {
          state_ = State::Esc;
        } else {
          out += ch;
          if ((u >= 0x30 && u <= 0x7E) || ch == '\x18' || ch == '\x1a') state_ = State::Ground;
        }
        break;
      case State::Csi:
        if (ch == '\x1b') {
          out += raw_;
          state_ = State::Esc;
          break;
        }
        if (u >= 0x30 && u <= 0x3F) {
          params_ += ch;
          if (!(ch >= '0' && ch <= '9') && ch != ';' && ch != ':') rewritable_ = false;
          if (keep_param_byte(ch)) raw_ += ch;
        } else if (ch == '\x18' || ch == '\x1a') {
          out += raw_;
          out += ch;
          state_ = State::Ground;
        } else if (c0(u)) {
          raw_ += ch;
          c0_ += ch;
        } else if (u >= 0x40 && u <= 0x7E) {
          raw_ += ch;
          state_ = State::Ground;
          finish_csi(ch, out);
        } else {
          raw_ += ch;
          rewritable_ = false;  // intermediates, DEL, non-ASCII
        }
        if (state_ == State::Csi && params_.size() + raw_.size() > kMaxCsi) {
          // Runaway sequence: discarded (the C0 controls inside it still execute).
          out += c0_;
          raw_.clear();
          params_.clear();
          c0_.clear();
          state_ = State::CsiDiscard;
        }
        break;
      case State::CsiDiscard:
        if (ch == '\x1b') {
          state_ = State::Esc;
        } else if (ch == '\x18' || ch == '\x1a') {
          out += ch;
          state_ = State::Ground;
        } else if (c0(u)) {
          out += ch;
        } else if (u >= 0x40 && u <= 0x7E) {
          state_ = State::Ground;
        }
        break;
      case State::String:
        if (ch == '\x1b') {
          state_ = State::StringEsc;
        } else {
          out += ch;
          if (ch == '\a' || ch == '\x18' || ch == '\x1a') state_ = State::Ground;
        }
        break;
      case State::StringEsc:
        if (ch == '\\') {
          out += "\x1b\\";
          state_ = State::Ground;
        } else {
          state_ = State::Esc;  // the ESC is still held
          --i;
        }
        break;
    }
    if (state_ == State::Ground) {
      in_sequence_ = false;
      if (boundary_wanted_) {
        boundary_ = out.size();
        boundary_wanted_ = false;
      }
    } else if (state_ == State::String || state_ == State::EscIntermediate) {
      in_sequence_ = true;
    }
  }
}

// ---------------------------------------------------------------------------
// OutputFilter

namespace {

bool alt_screen_mode(long mode) {
  return mode == 47 || mode == 1047 || mode == 1049;
}

bool mouse_mode(long mode) {
  switch (mode) {
    case 9:
    case 1000:
    case 1001:
    case 1002:
    case 1003:
    case 1005:
    case 1006:
    case 1015:
    case 1016: return true;
    default: return false;
  }
}

}  // namespace

void OutputFilter::release(std::string& out) {
  out += held_;
  held_.clear();
}

std::string OutputFilter::take_held() {
  if (!held_.empty()) released_ = true;
  return std::exchange(held_, {});
}

void OutputFilter::resync() {
  if (at_ground()) return;
  skip_ = true;
  if (!held_.empty()) released_ = true;  // part of what the snapshot already shows
  held_.clear();
}

OutputFilter::Event OutputFilter::finish_private(char final, std::string& out) {
  if ((final != 'h' && final != 'l') || !plain_) {
    release(out);
    return Event::None;
  }
  std::string kept;
  bool alt = false, mouse = false;
  for (const std::string_view p : split(params_, ';')) {
    const long mode = sgr_value(p);
    if (alt_screen_mode(mode)) {
      alt = true;
    } else if (!p.empty()) {
      mouse = mouse || mouse_mode(mode);
      (kept += kept.empty() ? "" : ";") += p;
    }
  }
  if (!alt) {
    release(out);
    return mouse ? Event::Mouse : Event::Mode;
  }
  if (released_) {
    // Already sent unchanged: the terminal switched screens. Back to its alternate screen.
    out += "\x1b[?1049h";
  } else {
    out += c0_;
    held_.clear();
    if (!kept.empty()) (((out += "\x1b[?") += kept) += final);
  }
  return Event::AltScreen;
}

OutputFilter::Event OutputFilter::finish_plain(char final, std::string& out) {
  if (final == 'p' && decstr_ && params_.empty()) {
    release(out);
    return Event::Mouse;  // DECSTR: some terminals (VTE) reset the mouse modes too
  }
  if (final == 'J' && plain_ && !released_ && sgr_value(split(params_, ';')[0]) == 3) {
    out += c0_;  // CSI 3 J would erase the user's scrollback, not the session's
    held_.clear();
    return Event::None;
  }
  release(out);
  return Event::None;
}

std::size_t OutputFilter::feed(const char* data, std::size_t len, std::string& out, Event& event) {
  if (!skip_) return feed_bytes(data, len, out, event);
  // Resyncing: drop everything up to the end of the sequence / character in progress.
  const std::size_t mark = out.size();
  std::size_t i = 0;
  event = Event::None;
  while (i < len && skip_ && event == Event::None) {
    i += feed_bytes(data + i, 1, out, event);
    if (at_ground()) skip_ = false;
  }
  out.resize(mark);
  if (event != Event::None || i == len) return i;
  return i + feed_bytes(data + i, len - i, out, event);
}

std::size_t OutputFilter::feed_bytes(const char* data, std::size_t len, std::string& out, Event& event) {
  event = Event::None;
  // Bytes of a sequence that may still be rewritten are held, unless take_held() gave it up.
  auto hold = [&](char ch) {
    if (released_) out += ch;
    else held_ += ch;
  };
  auto start_escape = [&] {
    released_ = false;
    utf8_left_ = 0;
    held_.assign(1, '\x1b');
    state_ = State::Esc;
  };
  auto add_param = [&](char ch) {
    if (params_.size() < kMaxParams) params_ += ch;
  };
  for (std::size_t i = 0; i < len; ++i) {
    const char ch = data[i];
    const auto u = static_cast<unsigned char>(ch);
    const bool cancel = ch == '\x18' || ch == '\x1a';
    switch (state_) {
      case State::Ground:
        if (ch == '\x1b') {
          start_escape();
          break;
        }
        out += ch;
        if (u >= 0x80 && u < 0xC0 && utf8_left_ > 0) --utf8_left_;
        else utf8_left_ = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : 0;
        break;
      case State::Esc:
        if (ch == '\x1b') {
          release(out);
          start_escape();
        } else if (ch == '[') {
          hold(ch);
          state_ = State::CsiStart;
          params_.clear();
          c0_.clear();
          plain_ = true;
          decstr_ = false;
        } else if (cancel) {
          release(out);
          out += ch;
          state_ = State::Ground;
        } else if (c0(u) || u == 0x7F) {
          hold(ch);  // executes (DEL is ignored) without leaving the sequence
        } else if (ch == 'c') {
          // RIS: never sent (it would leave the alternate screen and, on some terminals, erase
          // the user's scrollback); the caller sends a soft reset instead. Only the C0 controls
          // executed inside the sequence remain.
          if (!held_.empty()) out.append(held_, 1);
          held_.clear();
          state_ = State::Ground;
          event = Event::Reset;
          return i + 1;
        } else {
          release(out);
          out += ch;
          state_ = State::Ground;
          if (ch == ']' || ch == 'P' || ch == 'X' || ch == '^' || ch == '_') state_ = State::String;
          else if (u >= 0x20 && u <= 0x2F) state_ = State::EscIntermediate;
        }
        break;
      case State::EscIntermediate:
        if (ch == '\x1b') {
          start_escape();
        } else {
          out += ch;
          if ((u >= 0x30 && u <= 0x7E) || cancel) state_ = State::Ground;
        }
        break;
      case State::CsiStart:
        if (ch == '?') {
          hold(ch);
          state_ = State::PrivateCsi;
          break;
        }
        if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '!') {
          state_ = State::PlainCsi;  // may be CSI 3 J or DECSTR
          --i;
          break;
        }
        release(out);
        state_ = State::Csi;
        [[fallthrough]];
      case State::Csi:
        if (ch == '\x1b') {
          start_escape();
        } else {
          out += ch;
          if ((u >= 0x40 && u <= 0x7E) || cancel) state_ = State::Ground;
        }
        break;
      case State::PlainCsi:
      case State::PrivateCsi:
        if (ch == '\x1b') {
          release(out);
          start_escape();
          break;
        }
        hold(ch);
        if (cancel) {
          release(out);
          state_ = State::Ground;
        } else if (u >= 0x40 && u <= 0x7E) {
          const Event e = state_ == State::PlainCsi ? finish_plain(ch, out) : finish_private(ch, out);
          state_ = State::Ground;
          released_ = false;
          if (e != Event::None) {
            event = e;
            return i + 1;
          }
        } else {
          if (u >= 0x30 && u <= 0x3F) {
            add_param(ch);
            decstr_ = false;
          } else if (c0(u)) {
            if (!released_) c0_ += ch;
          } else {
            decstr_ = ch == '!' && plain_ && params_.empty() && !decstr_;
            plain_ = false;  // intermediates, DEL, non-ASCII
          }
          if (held_.size() > kMaxHeld) {
            release(out);
            released_ = true;
          }
        }
        break;
      case State::String:
        if (ch == '\x1b') {
          start_escape();
          state_ = State::StringEsc;
        } else {
          out += ch;
          if (ch == '\a' || cancel) state_ = State::Ground;
        }
        break;
      case State::StringEsc:
        if (ch == '\\') {
          hold(ch);
          release(out);
          released_ = false;
          state_ = State::Ground;
        } else {
          state_ = State::Esc;  // the ESC ended the string and starts a sequence
          --i;
        }
        break;
    }
  }
  return len;
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
      cell.attrs.font = (pen.flags & kFaint) ? 1 : 0;
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
    : rows_(std::clamp(rows, kMinRows, kMaxRows)),
      cols_(std::clamp(cols, kMinCols, kMaxCols)),
      capacity_(scrollback_lines) {
  vt_ = vterm_new(rows_, cols_);
  vterm_set_utf8(vt_, 1);
  vterm_output_set_callback(vt_, cb_output, this);
  screen_ = vterm_obtain_screen(vt_);
  state_ = vterm_obtain_state(vt_);
  vterm_screen_set_callbacks(screen_, &kCallbacks, this);
  vterm_screen_enable_altscreen(screen_, 1);
  // No reflow: libvterm 0.3.3's reflow aborts or corrupts memory when the cursor sits on a
  // wrapped line longer than the screen.
  vterm_screen_enable_reflow(screen_, false);
  vterm_screen_reset(screen_, 1);
  save_default_cursor();
}

// libvterm's saved cursor starts zeroed (an RGB black pen): a restore without a save (DECRC,
// CSI ? 1049 l) must give the default pen and the home position, as terminals do.
void Screen::save_default_cursor() {
  vterm_input_write(vt_, "\x1b" "7", 2);
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

void Screen::feed(const char* data, std::size_t len, std::vector<OutputPiece>& out) {
  out.emplace_back();
  ClientState state = client_state();
  while (len > 0) {
    OutputFilter::Event event;
    const bool was_alt = alt_;
    const std::size_t n = filter_.feed(data, len, out.back().bytes, event);
    feed_model(data, n);
    data += n;
    len -= n;
    switch (event) {
      case OutputFilter::Event::None: continue;
      case OutputFilter::Event::Mode: break;
      case OutputFilter::Event::Mouse: out.back().reapply = true; break;
      case OutputFilter::Event::AltScreen:
        // The client's terminal stays on its alternate screen: show the app's new screen.
        if (alt_ != was_alt && modes_.kitty_differs()) out.back().bytes += modes_.set_kitty(alt_);
        out.back().bytes += repaint();
        out.back().reapply = true;  // the same sequence may have named mouse modes
        break;
      case OutputFilter::Event::Reset:
        save_default_cursor();
        // Instead of RIS: a soft reset of everything the app may have changed, then the (blank)
        // screen from the model.
        out.back().bytes += "\x1b[!p";
        out.back().bytes += kModeOffs;
        out.back().bytes += "\x1b[<99u";
        out.back().bytes += modes_.take_ris_color_resets();
        out.back().bytes += repaint();
        out.back().reapply = true;
        break;
    }
    // A new piece only where the client's STATE changes (most mode changes leave it alone).
    const ClientState now = client_state();
    if (now == state && !out.back().reapply) continue;
    state = now;
    out.emplace_back();
  }
}

void Screen::feed_model(const char* raw, std::size_t raw_len) {
  rewritten_.clear();
  sgr_.feed(raw, raw_len, rewritten_);
  if (const auto cut = sgr_.take_boundary()) {
    // A resize happened while libvterm was inside a sequence: reset the region once it is out.
    process(rewritten_.data(), *cut);
    reset_scroll_region();
    process(rewritten_.data() + *cut, rewritten_.size() - *cut);
  } else {
    process(rewritten_.data(), rewritten_.size());
  }
}

void Screen::process(const char* data, std::size_t len) {
  while (len > 0) {
    const std::size_t n = modes_.feed(data, len);
    const bool was_alt = alt_;
    if (modes_.take_alt_request()) {
      // data[n - 1] is the final byte of a sequence that switches to the alternate screen.
      // Save the primary screen first: libvterm cannot read it while the alternate is active.
      vterm_input_write(vt_, data, n - 1);
      if (!alt_) {
        saved_primary_.clear();
        for (int r = 0; r < rows_; ++r) saved_primary_.push_back(read_row(r));
        vterm_state_get_cursorpos(state_, &saved_cursor_);
        alt_resized_ = false;
      }
      vterm_input_write(vt_, data + n - 1, 1);
    } else {
      vterm_input_write(vt_, data, n);
    }
    // data[n - 1] ended a sequence that left the alternate screen: after a resize, libvterm's
    // primary buffer is stale; replace it with the rows the snapshot showed.
    if (const int exit = modes_.take_alt_exit(); exit && was_alt && !alt_) {
      if (alt_resized_) repaint_primary(exit == 2);
      alt_resized_ = false;
    }
    // libvterm does not clamp the saved cursor on resize: a restore can put its cursor off the
    // screen, and the next character is written out of bounds. CSI ? 0 h (an unknown mode, a
    // no-op) makes libvterm clamp the cursor like after every CSI.
    if (modes_.take_cursor_restore()) {
      VTermPos pos{};
      vterm_state_get_cursorpos(state_, &pos);
      if (pos.row < 0 || pos.row >= rows_ || pos.col < 0 || pos.col >= cols_)
        vterm_input_write(vt_, "\x1b[?0h", 6);
    }
    data += n;
    len -= n;
  }
}

void Screen::resize(int rows, int cols) {
  if (rows <= 0 || cols <= 0) return;
  rows = std::clamp(rows, kMinRows, kMaxRows);
  cols = std::clamp(cols, kMinCols, kMaxCols);
  if (rows == rows_ && cols == cols_) return;
  if (alt_) {
    alt_resizing_ = true;
    vterm_set_size(vt_, rows, cols);
    alt_resizing_ = false;
    resize_saved_primary(rows);
    alt_resized_ = true;
  } else {
    vterm_set_size(vt_, rows, cols);
  }
  rows_ = rows;
  cols_ = cols;
  // libvterm does not clamp the scroll region's top on resize: a region starting below the new
  // last row makes the next scroll write out of bounds. Reset it, as xterm and tmux do on resize;
  // if libvterm is inside a string or escape sequence, as soon as it is out of it.
  if (sgr_.at_boundary()) reset_scroll_region();
  else sgr_.request_boundary();
}

void Screen::reset_scroll_region() {
  VTermPos pos{};
  vterm_state_get_cursorpos(state_, &pos);
  std::string out = "\x1b[r";  // DECSTBM homes the cursor: put it back
  append_cup(out, std::clamp(pos.row, 0, rows_ - 1), std::clamp(pos.col, 0, cols_ - 1));
  vterm_input_write(vt_, out.data(), out.size());
}

void Screen::resize_saved_primary(int rows) {
  auto& saved = saved_primary_;
  const int old_rows = int(saved.size());
  if (rows > old_rows) {
    // Growing: pull lines back from history above the rows, then add blank rows below.
    int pulled = 0;
    while (old_rows + pulled < rows && !scrollback_.empty()) {
      saved.insert(saved.begin(), std::move(scrollback_.back()));
      scrollback_.pop_back();
      ++pulled;
    }
    saved.resize(std::size_t(rows));
    saved_cursor_.row += pulled;
  } else if (rows < old_rows) {
    // Shrinking: drop blank rows below the cursor, then push top rows into history.
    while (int(saved.size()) > rows && int(saved.size()) - 1 > saved_cursor_.row && saved.back().chars.empty())
      saved.pop_back();
    const int excess = int(saved.size()) - rows;
    for (int i = 0; i < excess; ++i) push_line(std::move(saved[std::size_t(i)]));
    saved.erase(saved.begin(), saved.begin() + excess);
    saved_cursor_.row = std::max(saved_cursor_.row - excess, 0);
  }
}

void Screen::repaint_primary(bool restore_cursor) {
  VTermPos cursor{};
  vterm_state_get_cursorpos(state_, &cursor);
  if (restore_cursor) cursor = saved_cursor_;
  const Pen pen = current_pen();
  std::string out = "\x1b[0m\x1b[H\x1b[2J";
  for (int r = 0; r < rows_ && r < int(saved_primary_.size()); ++r) {
    append_cup(out, r, 0);
    render_line(out, truncated(saved_primary_[std::size_t(r)], cols_), true);
  }
  append_cup(out, std::clamp(cursor.row, 0, rows_ - 1), std::clamp(cursor.col, 0, cols_ - 1));
  append_sgr(out, pen, true);
  vterm_input_write(vt_, out.data(), out.size());
}

void Screen::push_line(Line line) {
  if (capacity_ == 0 || alt_resizing_) return;
  while (scrollback_.size() >= capacity_) {
    scrollback_.pop_front();
    ++dropped_;
  }
  scrollback_.push_back(std::move(line));
}

bool Screen::pop_line(Line& line) {
  if (scrollback_.empty() || alt_resizing_) return false;
  line = std::move(scrollback_.back());
  scrollback_.pop_back();
  return true;
}

Screen::Line Screen::read_row(int row) const {
  std::vector<VTermScreenCell> cells(static_cast<std::size_t>(cols_));
  for (int c = 0; c < cols_; ++c) vterm_screen_get_cell(screen_, VTermPos{row, c}, &cells[std::size_t(c)]);
  return line_from_cells(cells.data(), cols_);
}

std::string Screen::snapshot(bool fresh) {
  std::string out;
  out.reserve(kSnapshotReserve);
  if (!fresh) out += kModeOffs;
  // Soft reset (never RIS), hide the cursor while painting, clear the screen.
  out += "\x1b[!p\x1b[0m\x1b[H\x1b[2J\x1b[?7h\x1b[?25l";
  const std::uint64_t top = history_end();
  paint_lines(out, top, rows_, cols_, false);
  append_tail(out, top, rows_, cols_, true, fresh);
  return out;
}

std::string Screen::view_snapshot(int rows, int cols) const {
  std::string out;
  out.reserve(kSnapshotReserve);
  out += "\x1b[!p\x1b[0m\x1b[H\x1b[2J\x1b[?7h\x1b[?25l";
  const std::uint64_t top = view_top(rows);
  paint_lines(out, top, rows, cols, false);
  append_tail(out, top, rows, cols, false, true);
  return out;
}

// Cursor, modes, kitty flags, title and colors, pen: after the lines from `top` on.
void Screen::append_tail(std::string& out, std::uint64_t top, int rows, int cols, bool interactive,
                         bool fresh) const {
  VTermPos cursor{};
  vterm_state_get_cursorpos(state_, &cursor);
  const auto row = std::int64_t(history_end()) + cursor.row - std::int64_t(top);
  append_cup(out, int(std::clamp<std::int64_t>(row, 0, rows - 1)), std::clamp(cursor.col, 0, cols - 1));
  out += modes_.restore_modes(interactive);
  if (interactive) out += fresh ? modes_.restore_kitty(alt_) : modes_.set_kitty(alt_);
  out += modes_.restore_title_and_colors();
  // The app's current pen.
  const Pen pen = current_pen();
  if (!plain(pen)) append_sgr(out, pen);
}

std::string Screen::repaint() const {
  std::string out = "\x1b[0m";
  for (int r = 0; r < rows_; ++r) {
    append_cup(out, r, 0);
    out += "\x1b[2K";
    render_line(out, read_row(r));
  }
  VTermPos cursor{};
  vterm_state_get_cursorpos(state_, &cursor);
  append_cup(out, std::clamp(cursor.row, 0, rows_ - 1), std::clamp(cursor.col, 0, cols_ - 1));
  const Pen pen = current_pen();
  if (!plain(pen)) append_sgr(out, pen);
  return out;
}

std::uint64_t Screen::view_top(int rows) const {
  // The last row with content (or the cursor).
  VTermPos cursor{};
  vterm_state_get_cursorpos(state_, &cursor);
  int last = std::clamp(cursor.row, 0, rows_ - 1);
  for (int r = rows_ - 1; r > last; --r) {
    if (!read_row(r).chars.empty()) {
      last = r;
      break;
    }
  }
  const std::int64_t bottom = rows >= rows_ ? rows_ : std::max(last + 1, rows);
  const std::int64_t top = std::int64_t(history_end()) + bottom - rows;
  return std::uint64_t(std::max(top, std::int64_t(dropped_)));
}

void Screen::paint_lines(std::string& out, std::uint64_t top, int rows, int cols, bool primary) const {
  const std::uint64_t end = history_end();
  for (int r = 0; r < rows; ++r) {
    const std::uint64_t n = top + std::uint64_t(r);
    Line line;
    if (n < dropped_) continue;
    if (n < end) {
      line = scrollback_[std::size_t(n - dropped_)];
    } else {
      const std::uint64_t row = n - end;
      if (row >= std::uint64_t(rows_)) break;
      if (!alt_ || !primary) line = read_row(int(row));
      else if (row < saved_primary_.size()) line = saved_primary_[std::size_t(row)];
    }
    if (line.chars.empty()) continue;
    append_cup(out, r, 0);
    render_line(out, truncated(line, cols));
  }
}

std::string Screen::scroll_paint(std::uint64_t top, int rows, int cols) const {
  // No DECSTR: it would reset the terminal's input modes (bracketed paste, and on some
  // terminals pmux's mouse modes). Only what painting needs: no origin / insert mode, full
  // margins, ASCII in G0, the cursor hidden.
  std::string out = "\x1b[?6l\x1b[4l\x1b[r\x1b(B\x0f\x1b[0m\x1b[?25l\x1b[H\x1b[2J";
  paint_lines(out, top, rows, cols, true);
  return out;
}

ClientState Screen::client_state() const {
  ClientState st = modes_.client_state();
  if (alt_) st.flags |= kStateAltScreen;
  return st;
}

Screen::Pen Screen::current_pen() const {
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
  vterm_state_get_penattr(state_, VTERM_ATTR_FONT, &v);
  pen_cell.attrs.font = unsigned(v.number) & 15u;
  vterm_state_get_penattr(state_, VTERM_ATTR_UNDERLINE, &v);
  pen_cell.attrs.underline = unsigned(v.number) & 3u;
  vterm_state_get_penattr(state_, VTERM_ATTR_FOREGROUND, &v);
  pen_cell.fg = v.color;
  vterm_state_get_penattr(state_, VTERM_ATTR_BACKGROUND, &v);
  pen_cell.bg = v.color;
  return pen_of(pen_cell);
}

}  // namespace pmux
