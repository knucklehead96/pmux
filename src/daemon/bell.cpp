#include "daemon/bell.hpp"

namespace pmux {

bool BellScanner::feed(const char* data, std::size_t len) {
  bool bell = false;
  for (std::size_t i = 0; i < len; ++i) {
    const char ch = data[i];
    switch (state_) {
      case State::Ground:
        if (ch == '\a') bell = true;
        else if (ch == '\x1b') state_ = State::Esc;
        break;
      case State::Esc:
        if (ch == '\a') bell = true;  // C0 controls execute inside escape sequences
        else if (ch == ']' || ch == 'P' || ch == '_' || ch == '^' || ch == 'X') state_ = State::String;
        else if (ch != '\x1b') state_ = State::Ground;
        break;
      case State::String:
        if (ch == '\a' || ch == '\x18' || ch == '\x1a') state_ = State::Ground;  // BEL ends it; CAN/SUB abort
        else if (ch == '\x1b') state_ = State::StringEsc;
        break;
      case State::StringEsc:
        if (ch == '\\' || ch == '\a') {
          state_ = State::Ground;
        } else {
          // ESC ended the string and starts a new sequence.
          state_ = State::Esc;
          --i;
        }
        break;
    }
  }
  return bell;
}

}  // namespace pmux
