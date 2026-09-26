#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pmux {

// Attaches the controlling terminal to a session; returns the process exit code for pmux.
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
