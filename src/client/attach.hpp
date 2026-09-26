#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pmux {

enum class AttachOutcome {
  Detached,           // Ctrl+\ or stdin EOF
  AttachedElsewhere,  // another client attached
  Exited,             // the process exited; see wait_status
  Lost,               // connection to the daemon lost
  Failed,             // the attach request failed; see error
};

struct AttachResult {
  AttachOutcome outcome = AttachOutcome::Failed;
  int wait_status = 0;
  std::string error;  // for Failed
};

// Attaches the controlling terminal to a session and runs the passthrough loop until detach
// or exit. Prints nothing; `prelude` is written to the terminal once the daemon accepted.
AttachResult attach_session(int daemon_fd, std::uint32_t session_id, std::string_view prelude = {});

struct ViewResult {
  bool ok = false;
  std::string error;
};

// Shows a session's screen read-only (the final screen of an exited process) in raw mode
// until any key is pressed, then resets terminal modes as on detach.
ViewResult view_session(int daemon_fd, std::uint32_t session_id);

// CLI attach: attach_session plus the [detached ...] / [... exited ...] messages on stderr.
// Returns the exit code for pmux.
int attach(int daemon_fd, std::uint32_t session_id, const std::string& name);

// Result of scanning one input chunk for the detach key.
struct InputScan {
  std::string forward;  // bytes to send to the process
  std::string held;     // possible start of a split detach sequence; resolve within 20 ms
  bool detach = false;
};

// Finds Ctrl+\ (0x1c, CSI 92;5u, CSI 92;5:1u) and swallows its kitty release (CSI 92;5:3u).
InputScan scan_input(std::string_view input);

// True if buf contains a detach key; sets `pos` to its offset.
bool find_detach_key(const char* buf, std::size_t len, std::size_t& pos);

}  // namespace pmux
