#pragma once

#include <sys/types.h>

#include <string>

namespace pmux {

// $XDG_RUNTIME_DIR/pmux, fallback /tmp/pmux-$UID
std::string socket_dir();
std::string socket_path();
std::string pid_path();

// Creates socket_dir() with mode 0700 if needed and checks its ownership.
bool ensure_socket_dir(std::string& error);

// Client side: false (with `error`) if socket_dir() exists but is not a directory owned by the
// current user without group / other permissions. A missing directory is fine.
bool check_socket_dir(std::string& error);

// The process's umask, read from /proc/self/status without changing it (falls back to
// umask(0) + umask(mask) if that is unavailable).
mode_t current_umask();

}  // namespace pmux
