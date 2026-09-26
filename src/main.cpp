#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "client/attach.hpp"
#include "client/connect.hpp"
#include "client/tui.hpp"
#include "common/config.hpp"
#include "common/protocol.hpp"
#include "daemon/server.hpp"

namespace pmux {
namespace {

constexpr int kRequestTimeoutMs = 5000;
constexpr int kKillTimeoutMs = 10000;

constexpr const char* kUsage =
    "usage: pmux                               open the process list\n"
    "       pmux -n [name] [-d] [-- cmd...]    create a process in the current directory\n"
    "                                          and attach (-d: stay detached)\n"
    "       pmux -l                            list processes\n"
    "       pmux -a <name>                     attach to a process\n"
    "       pmux -k <name>                     kill a process (remove it if exited)\n"
    "       pmux --daemon                      run the daemon in the foreground\n"
    "       pmux -h | --help                   show this help\n";

enum class Mode { Tui, New, List, Attach, Kill, Daemon, Help };

struct Options {
  Mode mode = Mode::Tui;
  std::string name;
  bool detached = false;
  std::vector<std::string> cmd;
};

std::optional<Options> parse_args(int argc, char** argv) {
  Options o;
  bool mode_set = false;
  auto set_mode = [&](Mode m) {
    if (mode_set) return false;
    o.mode = m;
    return mode_set = true;
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--") {
      if (o.mode != Mode::New) return std::nullopt;
      o.cmd.assign(argv + i + 1, argv + argc);
      break;
    }
    if (arg == "-n") {
      if (!set_mode(Mode::New)) return std::nullopt;
      if (i + 1 < argc && argv[i + 1][0] != '-') o.name = argv[++i];
    } else if (arg == "-a" || arg == "-k") {
      if (!set_mode(arg == "-a" ? Mode::Attach : Mode::Kill) || i + 1 >= argc) return std::nullopt;
      o.name = argv[++i];
    } else if (arg == "-d") {
      o.detached = true;
    } else if (arg == "-l") {
      if (!set_mode(Mode::List)) return std::nullopt;
    } else if (arg == "--daemon") {
      if (!set_mode(Mode::Daemon)) return std::nullopt;
    } else if (arg == "-h" || arg == "--help") {
      if (!set_mode(Mode::Help)) return std::nullopt;
    } else {
      return std::nullopt;
    }
  }
  if (o.detached && o.mode != Mode::New) return std::nullopt;
  return o;
}

// $PWD if it names the current directory (keeps symlinked paths), else getcwd().
std::string current_dir() {
  struct stat pwd_st {}, dot_st {};
  const char* pwd = std::getenv("PWD");
  if (pwd && pwd[0] == '/' && stat(pwd, &pwd_st) == 0 && stat(".", &dot_st) == 0 &&
      pwd_st.st_dev == dot_st.st_dev && pwd_st.st_ino == dot_st.st_ino)
    return pwd;
  std::error_code ec;
  return std::filesystem::current_path(ec).string();
}

std::vector<std::string> current_env() {
  std::vector<std::string> env;
  for (char** e = environ; *e; ++e) env.emplace_back(*e);
  return env;
}

std::pair<std::uint16_t, std::uint16_t> creator_size() {
  for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
    winsize ws{};
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) return {ws.ws_row, ws.ws_col};
  }
  return {24, 80};
}

std::string state_string(const ProcInfo& p) {
  if (p.exited) {
    if (WIFSIGNALED(p.wait_status)) return "signaled:" + std::to_string(WTERMSIG(p.wait_status));
    return "exited:" + std::to_string(WEXITSTATUS(p.wait_status));
  }
  return p.idle_ms < 5000 ? "running" : "idle";
}

std::string join(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& s : v) {
    if (!out.empty()) out += ' ';
    out += s;
  }
  return out;
}

UniqueFd connect_or_report() {
  UniqueFd fd = connect_or_spawn_daemon();
  if (!fd) std::fprintf(stderr, "pmux: cannot connect to daemon\n");
  return fd;
}

std::optional<Frame> request(int fd, const Frame& frame, int timeout_ms = kRequestTimeoutMs) {
  FrameDecoder decoder;
  if (!send_frame(fd, frame)) return std::nullopt;
  return recv_frame(fd, decoder, timeout_ms);
}

// Prints an ERROR reply (or a generic failure) and returns exit code 1.
int report_failure(const std::optional<Frame>& reply) {
  std::string message = "no reply from daemon";
  if (reply && reply->type == MsgType::Error) message = PayloadReader(reply->payload).str();
  else if (reply) message = "unexpected reply from daemon";
  std::fprintf(stderr, "pmux: %s\n", message.c_str());
  return 1;
}

std::optional<std::vector<ProcInfo>> fetch_list(int fd) {
  auto reply = request(fd, make_frame(MsgType::List));
  if (reply && reply->type == MsgType::ListReply)
    if (auto list = decode_proc_list(reply->payload)) return list;
  report_failure(reply);
  return std::nullopt;
}

std::optional<ProcInfo> lookup(int fd, const std::string& name) {
  auto list = fetch_list(fd);
  if (!list) return std::nullopt;
  for (auto& p : *list)
    if (p.name == name) return std::move(p);
  std::fprintf(stderr, "pmux: no such process: %s\n", name.c_str());
  return std::nullopt;
}

int cmd_new(const Options& o, const Config& config) {
  if (!o.detached && !isatty(STDIN_FILENO)) {
    std::fprintf(stderr, "pmux: stdin is not a terminal (use -d to create detached)\n");
    return 1;
  }
  std::vector<std::string> argv = o.cmd;
  if (argv.empty()) argv = split_command(config.default_cmd);
  if (argv.empty()) {
    const char* shell = std::getenv("SHELL");
    argv.emplace_back(shell && *shell ? shell : "/bin/sh");
  }
  const auto [rows, cols] = creator_size();

  UniqueFd fd = connect_or_report();
  if (!fd) return 1;
  PayloadWriter w;
  w.str(o.name).str(current_dir()).strs(argv).strs(current_env()).u16(rows).u16(cols);
  auto reply = request(fd.get(), make_frame(MsgType::New, w.take()));
  if (!reply || reply->type != MsgType::Ok) return report_failure(reply);

  PayloadReader r(reply->payload);
  const std::uint32_t id = r.u32();
  const std::string name = r.str();
  if (o.detached) return 0;
  return attach(fd.get(), id, name);
}

int cmd_list() {
  UniqueFd fd = connect_or_report();
  if (!fd) return 1;
  auto list = fetch_list(fd.get());
  if (!list) return 1;
  for (const auto& p : *list)
    std::printf("%s\t%s\t%d\t%s\t%s\n", p.name.c_str(), state_string(p).c_str(), p.pid, p.dir.c_str(),
                join(p.argv).c_str());
  return 0;
}

int cmd_attach(const Options& o) {
  UniqueFd fd = connect_or_report();
  if (!fd) return 1;
  auto proc = lookup(fd.get(), o.name);
  if (!proc) return 1;
  if (proc->exited) {
    std::fprintf(stderr, "pmux: %s has exited\n", proc->name.c_str());
    return 1;
  }
  return attach(fd.get(), proc->id, proc->name);
}

int cmd_kill(const Options& o) {
  UniqueFd fd = connect_or_report();
  if (!fd) return 1;
  auto proc = lookup(fd.get(), o.name);
  if (!proc) return 1;
  auto reply = request(fd.get(), make_frame(MsgType::Kill, PayloadWriter().u32(proc->id).take()),
                       kKillTimeoutMs);
  if (!reply || reply->type != MsgType::Ok) return report_failure(reply);
  return 0;
}

}  // namespace
}  // namespace pmux

int main(int argc, char** argv) {
  using namespace pmux;
  const auto options = parse_args(argc, argv);
  if (!options) {
    std::fputs(kUsage, stderr);
    return 2;
  }
  Config config;
  if (options->mode != Mode::Daemon && options->mode != Mode::Help) config = load_config();
  switch (options->mode) {
    case Mode::Tui: return run_tui(config, current_dir());
    case Mode::New: return cmd_new(*options, config);
    case Mode::List: return cmd_list();
    case Mode::Attach: return cmd_attach(*options);
    case Mode::Kill: return cmd_kill(*options);
    case Mode::Daemon: return run_daemon();
    case Mode::Help: std::fputs(kUsage, stdout); return 0;
  }
  return 2;
}
