#include "client/attach.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <optional>
#include <utility>

#include "common/fd.hpp"
#include "common/protocol.hpp"

namespace pmux {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kDetachSeqs[] = {"\x1b[92;5u", "\x1b[92;5:1u"};
constexpr std::string_view kReleaseSeq = "\x1b[92;5:3u";
constexpr auto kHoldTime = std::chrono::milliseconds(20);

constexpr std::string_view kModeResets =
    "\x1b[?1049l"
    "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1015l"
    "\x1b[?2004l"
    "\x1b[?1004l"
    "\x1b[?25h"
    "\x1b[0m"
    "\x1b[<99u";

bool is_split_prefix(std::string_view rest) {
  if (rest.size() < 3) return false;
  auto prefix_of = [&](std::string_view seq) { return rest.size() < seq.size() && seq.starts_with(rest); };
  return prefix_of(kDetachSeqs[0]) || prefix_of(kDetachSeqs[1]) || prefix_of(kReleaseSeq);
}

bool starts_with_detach(std::string_view rest) {
  return rest.starts_with(kDetachSeqs[0]) || rest.starts_with(kDetachSeqs[1]);
}

struct Winsize {
  std::uint16_t rows = 0;
  std::uint16_t cols = 0;
};

Winsize terminal_size(int fd) {
  winsize ws{};
  if (ioctl(fd, TIOCGWINSZ, &ws) != 0) return {};
  return {ws.ws_row, ws.ws_col};
}

enum class Outcome { Detached, AttachedElsewhere, Exited, Lost };

class Passthrough {
 public:
  Passthrough(int sock, FrameDecoder& decoder, int winch_fd)
      : sock_(sock), decoder_(decoder), winch_fd_(winch_fd) {}

  Outcome run();
  int exit_status() const { return exit_status_; }

 private:
  std::optional<Outcome> handle(const Frame& frame);
  bool read_socket();
  std::optional<Outcome> read_stdin();
  void send_input(std::string_view bytes);
  void send_resize();

  int sock_;
  FrameDecoder& decoder_;
  int winch_fd_;
  std::string held_;
  Clock::time_point held_since_;
  int exit_status_ = 0;
  bool lost_ = false;
};

Outcome Passthrough::run() {
  for (;;) {
    while (auto frame = decoder_.next())
      if (auto outcome = handle(*frame)) return *outcome;
    if (decoder_.bad() || lost_) return Outcome::Lost;

    int timeout = -1;
    if (!held_.empty()) {
      const auto left = kHoldTime - (Clock::now() - held_since_);
      if (left <= Clock::duration::zero()) {
        send_input(std::exchange(held_, {}));
        continue;
      }
      timeout = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(left).count());
    }

    pollfd fds[3] = {{sock_, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}, {winch_fd_, POLLIN, 0}};
    if (poll(fds, 3, timeout) < 0) {
      if (errno == EINTR) continue;
      return Outcome::Lost;
    }
    if (fds[0].revents) {
      if (!read_socket()) return Outcome::Lost;
      continue;  // handle daemon frames before more input
    }
    if (fds[2].revents & POLLIN) {
      signalfd_siginfo info;
      while (read(winch_fd_, &info, sizeof info) == sizeof info) {}
      send_resize();
    }
    if (fds[1].revents)
      if (auto outcome = read_stdin()) return *outcome;
  }
}

std::optional<Outcome> Passthrough::handle(const Frame& frame) {
  switch (frame.type) {
    case MsgType::Output:
      if (!write_all(STDOUT_FILENO, frame.payload.data(), frame.payload.size())) return Outcome::Lost;
      return std::nullopt;
    case MsgType::Exited:
      exit_status_ = PayloadReader(frame.payload).i32();
      return Outcome::Exited;
    case MsgType::Detach:
      return Outcome::AttachedElsewhere;
    case MsgType::Error:
      return Outcome::Lost;
    default:
      return std::nullopt;
  }
}

bool Passthrough::read_socket() {
  char buf[65536];
  const ssize_t n = read(sock_, buf, sizeof buf);
  if (n < 0 && (errno == EINTR || errno == EAGAIN)) return true;
  if (n <= 0) return false;
  decoder_.feed(buf, std::size_t(n));
  return true;
}

std::optional<Outcome> Passthrough::read_stdin() {
  char buf[4096];
  const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
  if (n < 0 && (errno == EINTR || errno == EAGAIN)) return std::nullopt;
  if (n <= 0) return Outcome::Detached;

  std::string input = std::exchange(held_, {});
  input.append(buf, std::size_t(n));
  InputScan scan = scan_input(input);
  send_input(scan.forward);
  if (scan.detach) return Outcome::Detached;
  if (!scan.held.empty()) {
    held_ = std::move(scan.held);
    held_since_ = Clock::now();
  }
  return lost_ ? std::optional(Outcome::Lost) : std::nullopt;
}

void Passthrough::send_input(std::string_view bytes) {
  if (!bytes.empty() && !send_frame(sock_, bytes_frame(MsgType::Input, bytes))) lost_ = true;
}

void Passthrough::send_resize() {
  const Winsize ws = terminal_size(STDIN_FILENO);
  if (ws.rows == 0 || ws.cols == 0) return;
  if (!send_frame(sock_, make_frame(MsgType::Resize, PayloadWriter().u16(ws.rows).u16(ws.cols).take())))
    lost_ = true;
}

}  // namespace

InputScan scan_input(std::string_view input) {
  InputScan scan;
  std::size_t start = 0;  // first byte not yet copied to `forward`
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\x1c') {
      scan.forward.append(input.substr(start, i - start));
      scan.detach = true;
      return scan;
    }
    if (input[i] != '\x1b') continue;
    const std::string_view rest = input.substr(i);
    if (starts_with_detach(rest)) {
      scan.forward.append(input.substr(start, i - start));
      scan.detach = true;
      return scan;
    }
    if (rest.starts_with(kReleaseSeq)) {
      scan.forward.append(input.substr(start, i - start));
      i += kReleaseSeq.size() - 1;
      start = i + 1;
      continue;
    }
    if (is_split_prefix(rest)) {
      scan.forward.append(input.substr(start, i - start));
      scan.held = rest;
      return scan;
    }
  }
  scan.forward.append(input.substr(start));
  return scan;
}

bool find_detach_key(const char* buf, std::size_t len, std::size_t& pos) {
  const std::string_view input(buf, len);
  for (pos = 0; pos < len; ++pos)
    if (input[pos] == '\x1c' || starts_with_detach(input.substr(pos))) return true;
  pos = 0;
  return false;
}

AttachResult attach_session(int daemon_fd, std::uint32_t session_id, std::string_view prelude) {
  AttachResult result;
  if (!isatty(STDIN_FILENO)) {
    result.error = "stdin is not a terminal";
    return result;
  }

  sigset_t mask, saved_mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGWINCH);
  sigprocmask(SIG_BLOCK, &mask, &saved_mask);
  UniqueFd winch_fd(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  auto restore_mask = [&] {
    winch_fd.reset();
    sigprocmask(SIG_SETMASK, &saved_mask, nullptr);
  };

  Winsize ws = terminal_size(STDIN_FILENO);
  if (ws.rows == 0 || ws.cols == 0) ws = {24, 80};
  FrameDecoder decoder;
  const auto request = PayloadWriter().u32(session_id).u16(ws.rows).u16(ws.cols).take();
  std::optional<Frame> reply;
  if (send_frame(daemon_fd, make_frame(MsgType::Attach, request)))
    reply = recv_frame(daemon_fd, decoder, 5000);
  if (!reply || reply->type != MsgType::Ok) {
    restore_mask();
    if (!reply) result.error = "no reply from daemon";
    else result.error = PayloadReader(reply->payload).str();
    if (result.error.empty()) result.error = "attach failed";
    return result;
  }

  termios saved{};
  tcgetattr(STDIN_FILENO, &saved);
  termios raw = saved;
  cfmakeraw(&raw);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  if (!prelude.empty()) write_all(STDOUT_FILENO, prelude.data(), prelude.size());

  Passthrough passthrough(daemon_fd, decoder, winch_fd.get());
  const Outcome outcome = passthrough.run();

  tcsetattr(STDIN_FILENO, TCSADRAIN, &saved);
  write_all(STDOUT_FILENO, kModeResets.data(), kModeResets.size());
  restore_mask();

  result.wait_status = passthrough.exit_status();
  switch (outcome) {
    case Outcome::Detached: result.outcome = AttachOutcome::Detached; break;
    case Outcome::AttachedElsewhere: result.outcome = AttachOutcome::AttachedElsewhere; break;
    case Outcome::Exited: result.outcome = AttachOutcome::Exited; break;
    case Outcome::Lost: result.outcome = AttachOutcome::Lost; break;
  }
  return result;
}

int attach(int daemon_fd, std::uint32_t session_id, const std::string& name) {
  const AttachResult r = attach_session(daemon_fd, session_id);
  const int status = r.wait_status;
  switch (r.outcome) {
    case AttachOutcome::Detached:
      std::fprintf(stderr, "[detached from %s]\n", name.c_str());
      return 0;
    case AttachOutcome::AttachedElsewhere:
      std::fprintf(stderr, "[detached from %s: attached elsewhere]\n", name.c_str());
      return 0;
    case AttachOutcome::Exited:
      if (WIFSIGNALED(status))
        std::fprintf(stderr, "[%s killed by signal %d]\n", name.c_str(), WTERMSIG(status));
      else
        std::fprintf(stderr, "[%s exited: %d]\n", name.c_str(), WEXITSTATUS(status));
      return 0;
    case AttachOutcome::Failed:
      std::fprintf(stderr, "pmux: %s\n", r.error.c_str());
      return 1;
    case AttachOutcome::Lost:
      break;
  }
  std::fprintf(stderr, "pmux: lost connection to daemon\n");
  return 1;
}

}  // namespace pmux
