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

// Closes every fd from 3 up; the daemon must not keep the client's other fds open.
void close_inherited_fds() {
  if (close_range(3, ~0U, 0) == 0) return;
  const long max = sysconf(_SC_OPEN_MAX);
  for (long fd = 3; fd < (max > 0 ? max : 1024); ++fd) close(static_cast<int>(fd));
}

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
  close_inherited_fds();
  execl("/proc/self/exe", "pmux", "--daemon", static_cast<char*>(nullptr));
  _exit(run_daemon());
}

enum class Failure { None, NotRunning, Refused };

// Connects to the daemon's socket. Refused: using it would be unsafe or cannot work (the
// socket directory is not private, the path is too long, the daemon is another user's).
UniqueFd try_connect(std::string& error, Failure& failure) {
  failure = Failure::Refused;
  if (!check_socket_dir(error)) return {};
  const std::string path = socket_path();
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) {
    error = "socket path too long (" + std::to_string(path.size()) + " bytes, max " +
            std::to_string(sizeof addr.sun_path - 1) + "): " + path;
    return {};
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  UniqueFd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (!fd || connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    failure = Failure::NotRunning;
    error = "cannot connect to daemon";
    return {};
  }
  ucred cred{};
  socklen_t len = sizeof cred;
  if (getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
    error = std::string("cannot check the daemon's owner: ") + std::strerror(errno);
    return {};
  }
  if (cred.uid != getuid()) {
    error = "refusing to talk to daemon owned by uid " + std::to_string(cred.uid);
    return {};
  }
  failure = Failure::None;
  return fd;
}

}  // namespace

UniqueFd connect_daemon(std::string* error) {
  std::string message;
  Failure failure;
  UniqueFd fd = try_connect(message, failure);
  if (!fd && error) *error = std::move(message);
  return fd;
}

UniqueFd connect_or_spawn_daemon(std::string* error) {
  std::string message;
  Failure failure;
  if (auto fd = try_connect(message, failure)) return fd;
  if (failure == Failure::NotRunning) {
    spawn_daemon();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (auto fd = try_connect(message, failure)) return fd;
      if (failure == Failure::Refused) break;
    }
    if (failure == Failure::NotRunning) message = "cannot connect to daemon (see ~/.pmux/daemon.log)";
  }
  if (error) *error = std::move(message);
  return {};
}

}  // namespace pmux
