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
#include "daemon/bell.hpp"

namespace pmux {

struct SessionSpec {
  std::string name;
  std::string dir;
  std::vector<std::string> argv;
  std::vector<std::string> env;
  int rows = 24;
  int cols = 80;
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
  SystemClock::time_point created() const { return created_; }
  SteadyClock::time_point last_activity() const { return last_activity_; }
  std::optional<SystemClock::time_point> last_output() const { return last_output_; }
  bool bell() const { return bell_; }
  void clear_bell() { bell_ = false; }
  void set_name(std::string name) { name_ = std::move(name); }
  // argv of the PTY's foreground process group leader, joined by spaces; empty if unknown.
  std::string fg_command() const;

  // Reads one chunk of PTY output into `out`.
  ReadStatus read_output(std::string& out);
  void queue_input(std::string_view data);
  void flush_input();
  bool has_pending_input() const { return !input_.empty(); }

  void resize(int rows, int cols);
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
  std::vector<std::string> argv_;
  SystemClock::time_point created_;
  SteadyClock::time_point last_activity_;
  std::optional<SystemClock::time_point> last_output_;
  BellScanner bell_scanner_;
  bool bell_ = false;
  std::string input_;
  std::optional<int> reaped_;
  std::optional<int> exit_status_;
};

}  // namespace pmux
