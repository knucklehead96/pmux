#include "client/connect.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "common/paths.hpp"
#include "daemon/server.hpp"

namespace pmux {

namespace {

void spawn_daemon() {
  const pid_t pid = fork();
  if (pid < 0) return;
  if (pid > 0) {
    waitpid(pid, nullptr, 0);
    return;
  }
  setsid();
  if (fork() != 0) _exit(0);

  const int null_fd = open("/dev/null", O_RDWR);
  if (null_fd >= 0) {
    for (int fd = 0; fd < 3; ++fd) dup2(null_fd, fd);
    if (null_fd > 2) close(null_fd);
  }
  execl("/proc/self/exe", "pmux", "--daemon", static_cast<char*>(nullptr));
  _exit(run_daemon());
}

}  // namespace

UniqueFd connect_daemon() {
  const std::string path = socket_path();
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) return {};
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  UniqueFd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd || connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) return {};
  return fd;
}

UniqueFd connect_or_spawn_daemon() {
  if (auto fd = connect_daemon()) return fd;
  spawn_daemon();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (auto fd = connect_daemon()) return fd;
  }
  return {};
}

}  // namespace pmux
