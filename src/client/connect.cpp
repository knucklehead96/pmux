#include "client/connect.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "common/paths.hpp"
#include "common/protocol.hpp"
#include "daemon/server.hpp"

namespace pmux {

namespace {

constexpr int kHandshakeTimeoutMs = 5000;

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

enum class Failure { None, NotRunning, Refused, Timeout };

// Connects to the daemon's socket. Refused: using it would be unsafe or cannot work (the
// socket directory is not private, the path is too long, the daemon is another user's).
UniqueFd try_connect(std::string& error, Failure& failure, pid_t& peer_pid) {
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
  peer_pid = cred.pid;
  failure = Failure::None;
  return fd;
}

// try_connect, then HELLO: a daemon that does not match this pmux is Refused.
UniqueFd connect_checked(std::string& error, Failure& failure) {
  DaemonInfo info;
  UniqueFd fd = try_connect(error, failure, info.pid);
  if (!fd) return fd;
  const Handshake result = handshake(fd.get(), info);
  if (result == Handshake::Match) return fd;
  failure = result == Handshake::Timeout ? Failure::Timeout : Failure::Refused;
  error = mismatch_message(result, info);
  return {};
}

}  // namespace

UniqueFd connect_daemon(std::string* error, bool* not_running, bool* timed_out) {
  std::string message;
  Failure failure;
  UniqueFd fd = connect_checked(message, failure);
  if (!fd && error) *error = std::move(message);
  if (not_running) *not_running = failure == Failure::NotRunning;
  if (timed_out) *timed_out = failure == Failure::Timeout;
  return fd;
}

UniqueFd connect_or_spawn_daemon(std::string* error) {
  std::string message;
  Failure failure;
  if (auto fd = connect_checked(message, failure)) return fd;
  if (failure == Failure::NotRunning) {
    spawn_daemon();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (auto fd = connect_checked(message, failure)) return fd;
      if (failure == Failure::Refused) break;
    }
    if (failure == Failure::NotRunning) message = "cannot connect to daemon (see ~/.pmux/daemon.log)";
  }
  if (error) *error = std::move(message);
  return {};
}

UniqueFd connect_daemon_unchecked(std::string& error, bool& not_running, pid_t& peer_pid) {
  Failure failure;
  UniqueFd fd = try_connect(error, failure, peer_pid);
  not_running = failure == Failure::NotRunning;
  return fd;
}

Handshake handshake(int fd, DaemonInfo& info) {
  FrameDecoder decoder;
  const Frame hello =
      make_frame(MsgType::Hello, PayloadWriter().u32(kProtocolVersion).str(PMUX_VERSION).take());
  if (!send_frame(fd, hello)) return Handshake::NoReply;
  const auto reply = recv_frame(fd, decoder, kHandshakeTimeoutMs);
  if (!reply) {
    pollfd pfd{fd, POLLIN, 0};  // readable now: EOF or an error, not a slow daemon
    return !decoder.bad() && poll(&pfd, 1, 0) == 0 ? Handshake::Timeout : Handshake::NoReply;
  }
  if (reply->type != MsgType::Hello) return Handshake::Old;
  PayloadReader r(reply->payload);
  const std::uint32_t protocol = r.u32();
  std::string version = r.str();
  const std::uint32_t pid = r.u32();
  if (!r.ok()) return Handshake::Old;
  info.protocol = protocol;
  info.version = std::move(version);
  if (pid > 0) info.pid = static_cast<pid_t>(pid);
  return protocol == kProtocolVersion ? Handshake::Match : Handshake::Mismatch;
}

std::string mismatch_message(Handshake result, const DaemonInfo& info) {
  if (result == Handshake::NoReply || result == Handshake::Timeout) return "no reply from daemon";
  std::string daemon = info.pid > 0 ? "pid " + std::to_string(info.pid) + ", " : "";
  std::string ours = PMUX_VERSION;
  if (result == Handshake::Old) {
    daemon += "an older version";
  } else {
    daemon += "pmux " + info.version + ", protocol " + std::to_string(info.protocol);
    ours += ", protocol " + std::to_string(kProtocolVersion);
  }
  return "the running daemon (" + daemon + ") doesn't match this pmux (" + ours +
         "). Restart it with: pmux --stop";
}

}  // namespace pmux
