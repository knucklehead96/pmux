#include "daemon/session.hpp"

#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
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

constexpr std::size_t kMaxCmdline = 4096;
constexpr auto kFgCacheTime = std::chrono::milliseconds(500);

// Runs in the forked child: reset every signal disposition and the signal mask (the daemon may
// have inherited ignored signals), apply the client's umask, then exec. Reports errno on err_fd.
[[noreturn]] void exec_child(const char* dir, char* const* argv, char** envp, int mask, int err_fd) {
  for (int sig = 1; sig < NSIG; ++sig) signal(sig, SIG_DFL);
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
  if (mask >= 0) umask(mode_t(mask) & 0777);

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
  ws.ws_row = static_cast<unsigned short>(std::min(spec.rows, Screen::kMaxRows));
  ws.ws_col = static_cast<unsigned short>(std::min(spec.cols, Screen::kMaxCols));
  int master = -1;
  const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) {
    error = std::string("forkpty: ") + std::strerror(errno);
    return nullptr;
  }
  if (pid == 0) exec_child(spec.dir.c_str(), argv.data(), envp.data(), spec.umask, err_write.get());

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
  if (const char* tty = ptsname(master)) {
    s->tty_path_ = tty;
    struct stat st {};
    if (stat(tty, &st) == 0 && S_ISCHR(st.st_mode)) s->tty_rdev_ = st.st_rdev;
  }
  s->argv_ = spec.argv;
  s->created_ = SystemClock::now();
  s->last_activity_ = SteadyClock::now();
  s->screen_ = std::make_unique<Screen>(spec.rows, spec.cols, spec.scrollback_lines);
  return s;
}

Session::ReadStatus Session::read_output(std::string& out, bool attached) {
  char buf[65536];
  const ssize_t n = read(master_.get(), buf, sizeof buf);
  if (n > 0) {
    out.assign(buf, std::size_t(n));
    last_activity_ = SteadyClock::now();
    last_output_ = SystemClock::now();
    screen_->feed(buf, std::size_t(n));
    if (screen_->take_bell() && !attached) bell_ = true;
    const std::string replies = screen_->take_replies();
    if (!attached && !replies.empty()) queue_input(replies);
    return ReadStatus::Data;
  }
  if (n < 0 && (errno == EAGAIN || errno == EINTR)) return ReadStatus::Again;
  return ReadStatus::Eof;
}

std::string Session::fg_command() const {
  if (!master_ || exited()) return {};
  const pid_t pgrp = tcgetpgrp(master_.get());
  if (pgrp <= 0) return {};
  const auto now = SteadyClock::now();
  if (pgrp == fg_pgrp_ && now - fg_time_ < kFgCacheTime) return fg_command_;

  std::string raw;
  const std::string path = "/proc/" + std::to_string(pgrp) + "/cmdline";
  if (UniqueFd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC)); fd) {
    raw.resize(kMaxCmdline);
    std::size_t len = 0;
    while (len < raw.size()) {
      const ssize_t n = read(fd.get(), raw.data() + len, raw.size() - len);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) break;
      len += std::size_t(n);
    }
    raw.resize(len);
  }
  while (!raw.empty() && raw.back() == '\0') raw.pop_back();
  for (char& ch : raw)
    if (ch == '\0') ch = ' ';
  fg_pgrp_ = pgrp;
  fg_time_ = now;
  fg_command_ = raw;
  return raw;
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

bool Session::resize(int rows, int cols) {
  if (!master_ || rows <= 0 || cols <= 0) return false;
  rows = std::min(rows, Screen::kMaxRows);
  cols = std::min(cols, Screen::kMaxCols);
  winsize ws{};
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  winsize old{};
  const bool changed = ioctl(master_.get(), TIOCGWINSZ, &old) != 0 || old.ws_row != ws.ws_row ||
                       old.ws_col != ws.ws_col || old.ws_xpixel != 0 || old.ws_ypixel != 0;
  const bool set = ioctl(master_.get(), TIOCSWINSZ, &ws) == 0;
  screen_->resize(rows, cols);
  return changed && set;
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
