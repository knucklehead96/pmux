#pragma once

#include <cstddef>

namespace pmux {

// Detects BEL (0x07) in PTY output, ignoring BELs that terminate or occur inside
// OSC / DCS / APC / PM / SOS strings. The escape state survives chunk boundaries.
// (Isolated so M3 can replace it with libvterm's bell callback.)
class BellScanner {
 public:
  // Returns true if the chunk contains a bell.
  bool feed(const char* data, std::size_t len);

 private:
  enum class State { Ground, Esc, String, StringEsc };
  State state_ = State::Ground;
};

}  // namespace pmux
