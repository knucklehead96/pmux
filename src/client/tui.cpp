#include "client/tui.hpp"

#include <cstdio>

#include <ftxui/component/screen_interactive.hpp>

namespace pmux {

int run_tui(int /*daemon_fd*/) {
  std::fprintf(stderr, "pmux: TUI not implemented yet\n");
  return 1;
}

}  // namespace pmux
