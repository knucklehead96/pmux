#include "common/paths.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace pmux {

std::string socket_dir() {
  if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) return std::string(xdg) + "/pmux";
  return "/tmp/pmux-" + std::to_string(getuid());
}

std::string socket_path() {
  return socket_dir() + "/pmux.sock";
}

std::string pid_path() {
  return socket_dir() + "/pmux.pid";
}

bool ensure_socket_dir(std::string& error) {
  const std::string dir = socket_dir();
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    error = dir + ": " + std::strerror(errno);
    return false;
  }
  struct stat st {};
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
    error = dir + ": not a directory owned by the current user";
    return false;
  }
  if ((st.st_mode & 07777) != 0700 && chmod(dir.c_str(), 0700) != 0) {
    error = dir + ": " + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace pmux
