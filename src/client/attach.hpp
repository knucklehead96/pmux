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
// detach key (`key`) or exit. Prints nothing; `prelude` is written to the terminal once the
// daemon accepted.
// SIGWINCH, SIGTERM, SIGHUP, SIGINT and SIGQUIT are read through a signalfd meanwhile; other
// threads must block them (block_attach_signals).
AttachResult attach_session(int daemon_fd, std::uint32_t session_id, DetachKey key,
                            std::string_view prelude = {});

struct ViewResult {
  bool ok = false;
  std::string error;
};

// Shows a session's screen read-only (the final screen of an exited process) in raw mode
// until any key is pressed, then resets terminal modes as on detach.
ViewResult view_session(int daemon_fd, std::uint32_t session_id);

// CLI attach: attach_session plus the [detached ...] / [... exited ...] messages on stderr.
// Returns the exit code for pmux.
int attach(int daemon_fd, std::uint32_t session_id, const std::string& name, DetachKey key);

// Result of scanning one input chunk for the detach key.
struct InputScan {
  std::string forward;  // bytes to send to the process
  std::string held;     // possible start of a split detach key; resolve within 20 ms
  bool detach = false;
};

// Finds the detach key and swallows kitty repeats / releases of it. Kitty modifiers count only
// Ctrl (and Shift for ctrl+shift+left) besides Caps Lock / Num Lock.
//   ctrl+left:       CSI 1;5D, kitty CSI 1;<m>[:<ev>]D, rxvt ESC O d
//   ctrl+shift+left: CSI 1;6D, kitty CSI 1;<m>[:<ev>]D
//   ctrl+backslash:  0x1c, kitty CSI 92;<m>[:<ev>]u, xterm modifyOtherKeys CSI 27;5;92~
InputScan scan_input(std::string_view input, DetachKey key);

// Blocks the signals attach_session reads in the calling thread; for helper threads.
void block_attach_signals();

}  // namespace pmux
