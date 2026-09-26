#pragma once

#include <string>

namespace pmux {

// $XDG_RUNTIME_DIR/pmux, fallback /tmp/pmux-$UID
std::string socket_dir();
std::string socket_path();
std::string pid_path();

// Creates socket_dir() with mode 0700 if needed and checks its ownership.
bool ensure_socket_dir(std::string& error);

}  // namespace pmux
