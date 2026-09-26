#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/fd.hpp"
#include "daemon/screen.hpp"

namespace pmux {

struct SessionSpec {
  std::string name;
  std::string dir;
  std::vector<std::string> argv;
  std::vector<std::string> env;
  int rows = 24;
  int cols = 80;
  std::size_t scrollback_lines = 10000;
  int umask = -1;  // the creating client's umask; -1 = the daemon's
};

// One managed process: its PTY master and metadata.
class Session {
 public:
  using SteadyClock = std::chrono::steady_clock;
  using SystemClock = std::chrono::system_clock;
  enum class ReadStatus { Data, Again, Eof };

  static std::unique_ptr<Session> spawn(std::uint32_t id, const SessionSpec& spec, std::string& error);

  std::uint32_t id() const { return id_; }
  const std::string& name() const { return name_; }
  const std::string& dir() const { return dir_; }
  const std::vector<std::string>& argv() const { return argv_; }
  pid_t pid() const { return pid_; }
  int master_fd() const { return master_.get(); }
  // Path of the PTY slave (the process's controlling terminal), e.g. /dev/pts/3.
  const std::string& tty_path() const { return tty_path_; }
  // Device number (st_rdev) of the PTY slave; 0 if unknown or the PTY is closed.
  dev_t tty_rdev() const { return master_ ? tty_rdev_ : 0; }
  SystemClock::time_point created() const { return created_; }
  SteadyClock::time_point last_activity() const { return last_activity_; }
  std::optional<SystemClock::time_point> last_output() const { return last_output_; }
  bool bell() const { return bell_; }
  void clear_bell() { bell_ = false; }
  void set_name(std::string name) { name_ = std::move(name); }
  // argv of the PTY's foreground process group leader, joined by spaces (at most 4 KiB of
  // /proc/<pgid>/cmdline); empty if unknown. Cached for up to 500 ms.
  std::string fg_command() const;

  // Reads one chunk of PTY output into `out` and feeds it to the screen. While detached,
  // terminal query replies go back to the PTY and BEL sets the bell flag.
  ReadStatus read_output(std::string& out, bool attached);
  Screen& screen() { return *screen_; }
  void queue_input(std::string_view data);
  void flush_input();
  bool has_pending_input() const { return !input_.empty(); }
  std::size_t pending_input() const { return input_.size(); }

  // Sets the PTY size (the kernel sends SIGWINCH if it changed) and the screen's. True if the
  // PTY size changed.
  bool resize(int rows, int cols);
  void notify_winch();
  void hangup();
  void force_kill();
  void close_master() { master_.reset(); }

  // Wait status from waitpid; the session counts as exited only after finish().
  void set_reaped(int wait_status) { reaped_ = wait_status; }
  bool reaped() const { return reaped_.has_value(); }
  void finish() { exit_status_ = reaped_.value_or(0); }
  bool exited() const { return exit_status_.has_value(); }
  int exit_status() const { return exit_status_.value_or(0); }

 private:
  Session() = default;
  void signal_groups(int sig);

  std::uint32_t id_ = 0;
  UniqueFd master_;
  pid_t pid_ = -1;
  std::string name_;
  std::string dir_;
  std::string tty_path_;
  dev_t tty_rdev_ = 0;
  std::vector<std::string> argv_;
  SystemClock::time_point created_;
  SteadyClock::time_point last_activity_;
  std::optional<SystemClock::time_point> last_output_;
  std::unique_ptr<Screen> screen_;
  bool bell_ = false;
  std::string input_;
  mutable pid_t fg_pgrp_ = 0;
  mutable SteadyClock::time_point fg_time_;
  mutable std::string fg_command_;
  std::optional<int> reaped_;
  std::optional<int> exit_status_;
};

}  // namespace pmux
