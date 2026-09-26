#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vterm.h>

#include "common/protocol.hpp"

namespace pmux {

// Terminal state libvterm does not expose, tracked over the raw output stream.
// The escape-sequence parser state survives chunk boundaries.
class ModeTracker {
 public:
  // Returns the bytes consumed: stops right after a sequence that enters or leaves the
  // alternate screen (see take_alt_request / take_alt_exit) or restores the saved cursor
  // (take_cursor_restore), else consumes everything.
  std::size_t feed(const char* data, std::size_t len);
  bool take_alt_request() { return std::exchange(alt_requested_, false); }
  // 0: no alternate screen exit; 1: left it (47 / 1047); 2: left it restoring the cursor (1049).
  int take_alt_exit() { return std::exchange(alt_exit_, 0); }
  // DECRC (ESC 8), CSI ? 1048 l or CSI ? 1049 l.
  bool take_cursor_restore() { return std::exchange(cursor_restore_, false); }
  void reset();

  // Mode restore sequences (keys, mouse, paste, cursor shape, cursor visibility last).
  // Without `input`, only the cursor shape and visibility.
  std::string restore_modes(bool input) const;
  // Kitty keyboard flags push for the main / alternate screen stack ("" if none).
  std::string restore_kitty(bool alt) const;
  // Title (OSC 2) and recorded OSC 4/10/11/12 color sets.
  std::string restore_title_and_colors() const;
  // Sets the current kitty keyboard flags to the main / alternate stack's (CSI = flags ; 1 u).
  std::string set_kitty(bool alt) const;
  // OSC 104;<idx> / 110 / 111 / 112 for colors the app set and has not reset.
  std::string color_resets() const;
  // The app's mouse modes and cursor-key mode as STATE flags (without the alternate screen).
  ClientState client_state() const;
  bool kitty_differs() const { return kitty_main_.back() != kitty_alt_.back(); }

 private:
  enum class State { Ground, Esc, EscIntermediate, Csi, Osc, OscEsc, String, StringEsc };

  void dispatch_csi(char final);
  void dispatch_osc();
  void dispatch_esc(char final);
  void set_private_mode(int mode, bool on);
  std::string restore_input_modes() const;
  std::vector<std::uint32_t>& kitty_stack() { return alt_ ? kitty_alt_ : kitty_main_; }

  State state_ = State::Ground;
  std::string params_;
  char prefix_ = 0;
  std::string intermediates_;
  std::string osc_;
  bool osc_overflow_ = false;
  bool alt_requested_ = false;
  int alt_exit_ = 0;
  bool cursor_restore_ = false;

  // Tracked state.
  bool alt_ = false;
  bool cursor_keys_ = false;  // DECCKM (?1)
  bool autowrap_ = true;      // DECAWM (?7)
  bool cursor_visible_ = true;
  bool keypad_ = false;  // ESC = / ESC >
  bool focus_ = false;   // ?1004
  bool paste_ = false;   // ?2004
  int mouse_ = 0;  // 9 / 1000 / 1002 / 1003 (mutually exclusive), 0 = off
  bool mouse_1005_ = false, mouse_1006_ = false, mouse_1015_ = false, mouse_1016_ = false;
  int cursor_shape_ = -1;  // DECSCUSR Ps; -1 = never set
  int modify_other_keys_ = 0;  // xterm modifyOtherKeys (CSI > 4 ; n m), 0 = off
  std::vector<std::uint32_t> kitty_main_{0}, kitty_alt_{0};  // back() = current flags
  std::optional<std::string> title_;
  std::map<int, std::string> palette_;  // OSC 4 index -> spec
  std::optional<std::string> dyn_colors_[3];  // OSC 10 / 11 / 12
};

// Rewrites SGR sequences in the stream fed to libvterm (never the bytes sent to a client).
// libvterm 0.3.3 has no faint attribute, so faint is carried in the alternate font slot:
// SGR 2 -> 11 (font 1), SGR 22 -> 22;10 (normal intensity also clears faint); resets clear
// the font already. Also drops underline colors (58, which libvterm misreads as further SGR
// parameters) and the color-space id of `38:2:<cs>:r:g:b` / `48:...`, and keeps every CSI
// within libvterm's 16-argument limit (more crash libvterm 0.3.3; long SGRs are split, other
// sequences truncated; a CSI longer than 4 KiB is discarded). Every RIS (ESC c) is preceded by
// CSI ? 1049 l: libvterm's RIS does not leave the alternate screen. Other bytes pass through
// unchanged; parser state survives chunk boundaries (a partial CSI is held back).
class SgrRewriter {
 public:
  void feed(const char* data, std::size_t len, std::string& out);
  // False while libvterm, fed everything output so far, is inside a string or escape sequence.
  bool at_boundary() const { return !in_sequence_; }
  // The next feed() records the offset in `out` where at_boundary() holds again.
  void request_boundary() { boundary_wanted_ = true; }
  std::optional<std::size_t> take_boundary() { return std::exchange(boundary_, std::nullopt); }

 private:
  enum class State { Ground, Esc, EscIntermediate, Csi, CsiDiscard, String, StringEsc };
  void finish_csi(char final, std::string& out);
  // Counts argument separators; false for parameter bytes past libvterm's argument limit.
  bool keep_param_byte(char ch);

  State state_ = State::Ground;
  std::string raw_;     // the pending CSI, verbatim (from ESC [)
  std::string params_;  // its parameter bytes
  std::string c0_;      // C0 controls executed inside it
  int separators_ = 0;  // ';' and ':' seen in it
  bool rewritable_ = true;
  bool in_sequence_ = false;  // libvterm has seen part of a string or escape sequence
  bool boundary_wanted_ = false;
  std::optional<std::size_t> boundary_;
};

// Filters PTY output on its way to an attached client (never what libvterm is fed): removes
// 47 / 1047 / 1049 from DEC private mode set / reset sequences (CSI ? ... h / l; other
// parameters stay, the sequence is dropped if none is left), and stops after each of them and
// after RIS so the caller can act on the new state in stream order. Other bytes pass through
// unchanged. A possible start of such a sequence (ESC, ESC [, ESC [ ? ..., at most kMaxHeld
// bytes) is held back until it is complete or take_held() gives it up.
class OutputFilter {
 public:
  enum class Event {
    None,       // consumed everything
    Mode,       // a DEC private mode set / reset
    Mouse,      // one that names a mouse tracking or encoding mode
    AltScreen,  // one that switched the screen (47 / 1047 / 1049 were removed)
    Reset,      // RIS
  };
  static constexpr std::size_t kMaxHeld = 32;

  // Appends the output for the client to `out`; returns the bytes consumed (up to and
  // including the sequence behind `event`).
  std::size_t feed(const char* data, std::size_t len, std::string& out, Event& event);
  bool holding() const { return !held_.empty(); }
  // The held bytes, unchanged; the rest of their sequence then passes through unchanged too.
  std::string take_held();

 private:
  enum class State { Ground, Esc, EscIntermediate, CsiStart, Csi, PrivateCsi, String, StringEsc };
  // Ends the sequence being held: its bytes go out unchanged.
  void release(std::string& out);
  Event finish_private(char final, std::string& out);

  State state_ = State::Ground;
  std::string held_;    // raw bytes of the sequence being held
  std::string params_;  // its parameter bytes
  std::string c0_;      // C0 controls executed inside it
  bool plain_ = true;   // no intermediates or other odd bytes: may be rewritten
  bool released_ = false;  // take_held() gave the current sequence up
};

// Per-session virtual terminal: libvterm screen, scrollback ring, mode tracker.
class Screen {
 public:
  // Output for an attached client, cut where its STATE changed.
  struct OutputPiece {
    std::string bytes;
    bool reapply = false;  // RIS or a mouse mode: the client re-applies its mouse modes
  };

  // libvterm's size is clamped to [kMinRows..kMaxRows] x [kMinCols..kMaxCols]: libvterm 0.3.3
  // crashes on a double-width character in a 1-column screen.
  static constexpr int kMinRows = 2, kMinCols = 2;
  static constexpr int kMaxRows = 500, kMaxCols = 1000;

  Screen(int rows, int cols, std::size_t scrollback_lines = 10000);
  ~Screen();
  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;

  // Feeds PTY output and appends the client's output to `out` (see OutputFilter): an alternate
  // screen switch is followed by a repaint of the new screen, RIS by CSI ? 1049 h and a repaint.
  // Terminal query replies generated by libvterm accumulate in replies().
  void feed(const char* data, std::size_t len, std::vector<OutputPiece>& out);
  // Held-back client output (see OutputFilter).
  bool holding() const { return filter_.holding(); }
  std::string take_held() { return filter_.take_held(); }
  void resize(int rows, int cols);
  // Bytes that redraw the visible screen and modes on a terminal (DECSTR, clear, rows, cursor,
  // modes, kitty flags, title and colors, pen). Non-interactive (read-only view): input-related
  // modes and kitty flags are not restored. `fresh`: the terminal's kitty flags stack is empty
  // (pushed then), else the current flags are set.
  std::string snapshot(bool interactive, bool fresh = true);
  std::string color_resets() const { return modes_.color_resets(); }
  ClientState client_state() const;

  // Scroll mode. Lines are numbered for as long as the session lives: history holds
  // [history_base(), history_end()), the screen's rows follow from history_end().
  std::uint64_t history_base() const { return dropped_; }
  std::uint64_t history_end() const { return dropped_ + scrollback_.size(); }
  std::size_t history_size() const { return scrollback_.size(); }
  int rows() const { return rows_; }
  // Paints rows() lines from line `top` on (history, then the primary screen's rows).
  std::string scroll_paint(std::uint64_t top) const;

  std::string take_replies() { return std::exchange(replies_, {}); }
  bool take_bell() { return std::exchange(bell_, false); }

  // Compact cell storage (public for the libvterm callbacks in screen.cpp).
  struct Pen {
    std::uint16_t flags = 0;  // bit set, see screen.cpp
    std::uint8_t underline = 0;
    std::uint32_t fg = 0;  // packed VTermColor
    std::uint32_t bg = 0;
    bool operator==(const Pen&) const = default;
  };
  struct Line {
    std::vector<std::uint32_t> chars;  // one per cell: 0 empty, 0xFFFFFFFF wide-char continuation
    std::vector<std::pair<std::uint16_t, Pen>> runs;  // (start column, pen), ascending
    std::vector<std::pair<std::uint16_t, std::u32string>> combining;  // extra codepoints per column
  };

  // libvterm's scrollback callbacks; ignored while resizing with the alternate screen active.
  void push_line(Line line);
  bool pop_line(Line& line);
  void clear_scrollback() {
    dropped_ += scrollback_.size();
    scrollback_.clear();
  }
  void set_alt(bool alt) { alt_ = alt; }
  void ring_bell() { bell_ = true; }
  void add_reply(const char* data, std::size_t len) { replies_.append(data, len); }
  std::uint32_t default_fg() const;  // packed VTermColor
  std::uint32_t default_bg() const;

 private:
  Line read_row(int row) const;
  Pen current_pen() const;
  // Feeds libvterm and the mode tracker.
  void feed_model(const char* data, std::size_t len);
  void process(const char* data, std::size_t len);
  // Redraws the visible screen from the model: rows, cursor, pen (no modes).
  std::string repaint() const;
  void reset_scroll_region();
  void resize_saved_primary(int rows);
  void repaint_primary(bool restore_cursor);

  VTerm* vt_ = nullptr;
  VTermScreen* screen_ = nullptr;
  VTermState* state_ = nullptr;
  int rows_;
  int cols_;
  std::size_t capacity_;
  std::deque<Line> scrollback_;
  std::uint64_t dropped_ = 0;  // lines dropped from the front of scrollback_
  // The primary screen while the alternate screen is active: its rows (always rows_ of them)
  // and the cursor saved on entry. Resizes adjust them like a terminal would (history pulled in
  // when growing; blank rows below the cursor, then top rows pushed to history when shrinking),
  // instead of libvterm, whose primary buffer is repainted from them on exit if resized. Scroll
  // mode shows them below the history (libvterm cannot read the primary buffer meanwhile).
  std::vector<Line> saved_primary_;
  VTermPos saved_cursor_{};
  bool alt_resizing_ = false;  // inside vterm_set_size with the alternate screen active
  bool alt_resized_ = false;   // resized since the alternate screen was entered
  ModeTracker modes_;
  SgrRewriter sgr_;
  OutputFilter filter_;
  std::string rewritten_;  // scratch for feed()
  std::string replies_;
  bool bell_ = false;
  bool alt_ = false;
};

}  // namespace pmux
