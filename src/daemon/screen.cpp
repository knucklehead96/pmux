#include "daemon/screen.hpp"

#include <vterm.h>

namespace pmux {

Screen::Screen(int /*rows*/, int /*cols*/, std::size_t scrollback_lines)
    : scrollback_lines_(scrollback_lines) {}

Screen::~Screen() {
  if (vt_) vterm_free(vt_);
}

void Screen::feed(const char* /*data*/, std::size_t /*len*/) {}

void Screen::resize(int /*rows*/, int /*cols*/) {}

std::string Screen::snapshot() const {
  return {};
}

}  // namespace pmux
