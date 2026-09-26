#include "daemon/session.hpp"

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace pmux {

namespace {

std::vector<char*> c_strings(const std::vector<std::string>& v) {
  std::vector<char*> out;
  out.reserve(v.size() + 1);
  for (const auto& s : v) out.push_back(const_cast<char*>(s.c_str()));
  out.push_back(nullptr);
  return out;
}

// Runs in the forked child: undo daemon signal state, then exec. Reports errno on err_fd.
[[noreturn]] void exec_child(const char* dir, char* const* argv, char** envp, int err_fd) {
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
  for (int sig : {SIGPIPE, SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGQUIT}) signal(sig, SIG_DFL);

  if (chdir(dir) == 0) {
    environ = envp;
    execvp(argv[0], argv);
  }
  const int err = errno;
  (void)!write(err_fd, &err, sizeof err);
  _exit(127);
}

}  // namespace

std::unique_ptr<Session> Session::spawn(std::uint32_t id, const SessionSpec& spec, std::string& error) {
  if (spec.argv.empty()) {
    error = "empty command";
    return nullptr;
  }
  auto argv = c_strings(spec.argv);
  auto envp = c_strings(spec.env);

  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
    error = std::string("pipe: ") + std::strerror(errno);
    return nullptr;
  }
  UniqueFd err_read(pipe_fds[0]);
  UniqueFd err_write(pipe_fds[1]);

  winsize ws{};
  ws.ws_row = static_cast<unsigned short>(spec.rows);
  ws.ws_col = static_cast<unsigned short>(spec.cols);
  int master = -1;
  const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) {
    error = std::string("forkpty: ") + std::strerror(errno);
    return nullptr;
  }
  if (pid == 0) exec_child(spec.dir.c_str(), argv.data(), envp.data(), err_write.get());

  UniqueFd master_fd(master);
  err_write.reset();
  int child_errno = 0;
  ssize_t n;
  do n = read(err_read.get(), &child_errno, sizeof child_errno);
  while (n < 0 && errno == EINTR);
  if (n == sizeof child_errno) {
    waitpid(pid, nullptr, 0);
    error = "cannot run " + spec.argv[0] + " in " + spec.dir + ": " + std::strerror(child_errno);
    return nullptr;
  }

  fcntl(master, F_SETFD, FD_CLOEXEC);
  fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

  std::unique_ptr<Session> s(new Session());
  s->id_ = id;
  s->master_ = std::move(master_fd);
  s->pid_ = pid;
  s->name_ = spec.name;
  s->dir_ = spec.dir;
  s->argv_ = spec.argv;
  s->created_ = SystemClock::now();
  s->last_activity_ = SteadyClock::now();
  return s;
}

Session::ReadStatus Session::read_output(std::string& out) {
  char buf[65536];
  const ssize_t n = read(master_.get(), buf, sizeof buf);
  if (n > 0) {
    out.assign(buf, std::size_t(n));
    last_activity_ = SteadyClock::now();
    return ReadStatus::Data;
  }
  if (n < 0 && (errno == EAGAIN || errno == EINTR)) return ReadStatus::Again;
  return ReadStatus::Eof;
}

void Session::queue_input(std::string_view data) {
  if (!master_) return;
  input_.append(data);
  flush_input();
}

void Session::flush_input() {
  while (!input_.empty()) {
    const ssize_t n = write(master_.get(), input_.data(), input_.size());
    if (n > 0) {
      input_.erase(0, std::size_t(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      if (!(n < 0 && errno == EAGAIN)) input_.clear();
      return;
    }
  }
}

void Session::resize(int rows, int cols) {
  if (!master_ || rows <= 0 || cols <= 0) return;
  winsize ws{};
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  ioctl(master_.get(), TIOCSWINSZ, &ws);
}

void Session::notify_winch() {
  if (!master_) return;
  const pid_t pgrp = tcgetpgrp(master_.get());
  if (pgrp > 0) kill(-pgrp, SIGWINCH);
}

void Session::signal_groups(int sig) {
  if (reaped_) return;  // pid may already be recycled
  if (kill(-pid_, sig) != 0) kill(pid_, sig);
  if (master_) {
    const pid_t fg = tcgetpgrp(master_.get());
    if (fg > 0 && fg != pid_) kill(-fg, sig);
  }
}

void Session::hangup() {
  signal_groups(SIGHUP);
  signal_groups(SIGCONT);
}

void Session::force_kill() {
  signal_groups(SIGKILL);
}

}  // namespace pmux
