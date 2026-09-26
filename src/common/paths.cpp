#include "common/paths.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

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

bool check_socket_dir(std::string& error) {
  const std::string dir = socket_dir();
  struct stat st {};
  if (lstat(dir.c_str(), &st) != 0) {
    if (errno == ENOENT) return true;
    error = dir + ": " + std::strerror(errno);
    return false;
  }
  if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
    error = dir + ": not a directory owned by the current user; refusing to use it";
    return false;
  }
  if ((st.st_mode & 077) != 0) {
    char mode[8];
    std::snprintf(mode, sizeof mode, "%04o", st.st_mode & 07777);
    error = dir + ": accessible by other users (mode " + mode + "); refusing to use it";
    return false;
  }
  return true;
}

mode_t current_umask() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (!line.starts_with("Umask:")) continue;
    char* end = nullptr;
    const unsigned long mask = std::strtoul(line.c_str() + 6, &end, 8);
    if (end != line.c_str() + 6 && mask <= 0777) return static_cast<mode_t>(mask);
    break;
  }
  const mode_t mask = umask(0);
  umask(mask);
  return mask;
}

}  // namespace pmux
