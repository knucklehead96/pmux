#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/config.hpp"

namespace pmux {

enum class AttachOutcome {
  Detached,           // the detach key or stdin EOF
  AttachedElsewhere,  // another client attached
  Exited,             // the process exited; see wait_status
  Lost,               // connection to the daemon lost
  Failed,             // the attach request failed; see error
  Signaled,           // the client got SIGTERM / SIGHUP / SIGINT / SIGQUIT; see signal
};

struct AttachResult {
  AttachOutcome outcome = AttachOutcome::Failed;
  int wait_status = 0;
  int signal = 0;     // for Signaled
  std::string error;  // for Failed
};

// Attaches the controlling terminal to a session and runs the passthrough loop until the
// detach key (config.detach_key) or exit. The session is shown on the terminal's alternate
// screen, left again at the end: the main screen and its scrollback are as before. Prints
// nothing else; `prelude` is written to the terminal once the daemon accepted.
// While the app tracks no mouse, pmux has it: the wheel scrolls through the history (scroll
// mode) or, on the app's alternate screen, sends cursor keys; other mouse reports are dropped.
// SIGWINCH, SIGTERM, SIGHUP, SIGINT and SIGQUIT are read through a signalfd meanwhile; other
// threads must block them (block_attach_signals).
AttachResult attach_session(int daemon_fd, std::uint32_t session_id, const Config& config,
                            std::string_view prelude = {});

struct ViewResult {
  bool ok = false;
  std::string error;
};

// Shows a session's screen read-only (the final screen of an exited process, sized to the
// terminal) on the alternate screen in raw mode: the wheel and navigation keys scroll through
// its history, any other key ends the view. Then resets terminal modes as on detach.
ViewResult view_session(int daemon_fd, std::uint32_t session_id, const Config& config);

// CLI attach: attach_session plus the [detached ...] / [... exited ...] messages on stderr (on
// the main screen). Returns the exit code for pmux.
int attach(int daemon_fd, std::uint32_t session_id, const std::string& name, const Config& config);

// Result of scanning one input chunk for the detach key (and mouse reports).
struct InputScan {
  std::string forward;  // bytes to send to the process
  std::string held;     // possible start of a split detach key / mouse report; resolve within 20 ms
  bool detach = false;
  int mouse = -1;            // with `mouse`: the button code of the SGR mouse report ending the scan
  std::size_t consumed = 0;  // for a mouse report: the input bytes up to its end
};

// Finds the detach key and swallows kitty repeats / releases of it. Kitty modifiers count only
// Ctrl (and Shift for ctrl+shift+left) besides Caps Lock / Num Lock.
//   ctrl+left:       CSI 1;5D, kitty CSI 1;<m>[:<ev>]D, rxvt ESC O d
//   ctrl+shift+left: CSI 1;6D, kitty CSI 1;<m>[:<ev>]D
//   ctrl+backslash:  0x1c, kitty CSI 92;<m>[:<ev>]u, xterm modifyOtherKeys CSI 27;5;92~
// With `mouse`, also stops after the first SGR mouse report (ESC [ < b ; x ; y M|m).
InputScan scan_input(std::string_view input, DetachKey key, bool mouse = false);

// Blocks the signals attach_session reads in the calling thread; for helper threads.
void block_attach_signals();

}  // namespace pmux
