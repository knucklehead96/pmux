#include "daemon/server.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <utility>

#include "common/config.hpp"
#include "common/paths.hpp"

namespace pmux {

namespace {

constexpr std::size_t kOutputHighWater = 1 << 20;
constexpr auto kKillGrace = std::chrono::seconds(3);
constexpr auto kDrainGrace = std::chrono::seconds(1);
constexpr std::size_t kSnapshotChunk = 1 << 20;
// A client whose unsent output (beyond its last snapshot), or whose session's unwritten input,
// exceeds this is dropped.
constexpr std::size_t kMaxQueue = std::size_t(64) << 20;
// Client output held back as the possible start of an alternate screen switch is sent after this.
constexpr auto kHoldTime = std::chrono::milliseconds(20);

enum class Tag : std::uint64_t { Listen = 1, Signal, Client, Master };

std::uint64_t make_tag(Tag tag, std::uint32_t id) {
  return static_cast<std::uint64_t>(tag) << 32 | id;
}

bool valid_name(std::string_view name) {
  if (name.empty() || name.front() == '-') return false;
  return std::none_of(name.begin(), name.end(),
                      [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

// Client-supplied sizes are clamped before they reach libvterm or TIOCSWINSZ.
int clamp_rows(int rows) {
  return std::min(rows, Screen::kMaxRows);
}

int clamp_cols(int cols) {
  return std::min(cols, Screen::kMaxCols);
}

// st_rdev of a terminal device path; 0 if it is not one.
dev_t tty_device(const std::string& path) {
  struct stat st {};
  if (path.empty() || stat(path.c_str(), &st) != 0 || !S_ISCHR(st.st_mode)) return 0;
  return st.st_rdev;
}

// Prints an error on stderr and appends it, timestamped, to ~/.pmux/daemon.log.
void log_error(const std::string& message) {
  std::fprintf(stderr, "pmux: %s\n", message.c_str());
  const std::string home = home_dir();
  if (home.empty()) return;
  const std::string dir = home + "/.pmux";
  mkdir(dir.c_str(), 0700);
  UniqueFd fd(open((dir + "/daemon.log").c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600));
  if (!fd) return;
  char stamp[32] = "";
  const std::time_t now = std::time(nullptr);
  std::tm tm {};
  if (localtime_r(&now, &tm)) std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
  const std::string line = std::string(stamp) + " pmux[" + std::to_string(getpid()) + "]: " + message + "\n";
  (void)!write(fd.get(), line.data(), line.size());
}

Frame ok_frame(std::vector<std::uint8_t> payload = {}) {
  return make_frame(MsgType::Ok, std::move(payload));
}

UniqueFd listen_on(const std::string& path, std::string& error) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) {
    error = path + ": socket path too long";
    return {};
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  UniqueFd fd(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  unlink(path.c_str());
  if (!fd || bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
      listen(fd.get(), SOMAXCONN) != 0) {
    error = path + ": " + std::strerror(errno);
    return {};
  }
  return fd;
}

}  // namespace

Server::Server(UniqueFd listen_fd) : listen_fd_(std::move(listen_fd)) {}

bool Server::setup() {
  signal(SIGPIPE, SIG_IGN);
  // An inherited SIG_IGN would make the kernel reap children before waitpid sees them.
  signal(SIGCHLD, SIG_DFL);
  sigset_t mask;
  sigemptyset(&mask);
  for (int sig : {SIGCHLD, SIGTERM, SIGINT, SIGHUP}) sigaddset(&mask, sig);
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) return false;

  signal_fd_.reset(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  epoll_fd_.reset(epoll_create1(EPOLL_CLOEXEC));
  if (!signal_fd_ || !epoll_fd_) return false;

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.u64 = make_tag(Tag::Listen, 0);
  if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, listen_fd_.get(), &ev) != 0) return false;
  ev.data.u64 = make_tag(Tag::Signal, 0);
  return epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, signal_fd_.get(), &ev) == 0;
}

int Server::run() {
  if (!setup()) {
    log_error(std::string("daemon setup: ") + std::strerror(errno));
    return 1;
  }
  while (running_) {
    epoll_event events[64];
    const int n = epoll_wait(epoll_fd_.get(), events, 64, timeout_ms());
    if (n < 0 && errno != EINTR) {
      log_error(std::string("epoll_wait: ") + std::strerror(errno));
      return 1;
    }
    for (int i = 0; i < n; ++i) {
      const auto tag = static_cast<Tag>(events[i].data.u64 >> 32);
      const auto id = static_cast<std::uint32_t>(events[i].data.u64);
      switch (tag) {
        case Tag::Listen: on_accept(); break;
        case Tag::Signal: on_signal(); break;
        case Tag::Client: on_client_event(id, events[i].events); break;
        case Tag::Master: on_master_event(id, events[i].events); break;
      }
    }
    run_timers();
    remove_finished();
    if (stopping_) finish_stop();
    close_dead_clients();
  }
  return 0;
}

void Server::on_accept() {
  for (;;) {
    UniqueFd fd(accept4(listen_fd_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    if (!fd) return;
    const std::uint32_t id = next_client_id_++;
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = make_tag(Tag::Client, id);
    if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd.get(), &ev) != 0) continue;
    Client& c = clients_[id];
    c.id = id;
    c.fd = std::move(fd);
  }
}

void Server::on_signal() {
  signalfd_siginfo info;
  bool child = false;
  while (read(signal_fd_.get(), &info, sizeof info) == sizeof info) {
    if (info.ssi_signo == SIGCHLD)
      child = true;
    else
      running_ = false;
  }
  if (child) reap_children();
}

void Server::on_client_event(std::uint32_t id, std::uint32_t events) {
  Client* c = find_client(id);
  if (!c || c->dead) return;
  if (events & EPOLLOUT) flush(*c);
  if (!(events & (EPOLLIN | EPOLLHUP | EPOLLERR))) return;

  bool eof = false;
  for (int i = 0; i < 16; ++i) {
    char buf[65536];
    const ssize_t n = read(c->fd.get(), buf, sizeof buf);
    if (n > 0) {
      c->decoder.feed(buf, std::size_t(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    eof = !(n < 0 && errno == EAGAIN);
    break;
  }
  while (!c->dead) {
    auto frame = c->decoder.next();
    if (!frame) break;
    handle_frame(*c, *frame);
  }
  if (eof || c->decoder.bad()) c->dead = true;
}

void Server::handle_frame(Client& c, const Frame& frame) {
  switch (frame.type) {
    case MsgType::List: do_list(c); break;
    case MsgType::New: do_new(c, frame); break;
    case MsgType::Rename: do_rename(c, frame); break;
    case MsgType::Attach: do_attach(c, frame); break;
    case MsgType::Detach: do_detach(c); break;
    case MsgType::View: do_view(c, frame); break;
    case MsgType::Resize: do_resize(c, frame); break;
    case MsgType::Input: do_input(c, frame); break;
    case MsgType::Kill: do_kill(c, frame); break;
    case MsgType::Remove: do_remove(c, frame); break;
    case MsgType::Stop: do_stop(c, frame); break;
    case MsgType::Scroll: do_scroll(c, frame); break;
    default: send(c, error_frame("unsupported request")); break;
  }
}

void Server::do_list(Client& c) {
  std::vector<ProcInfo> list;
  for (const auto& [id, p] : procs_) {
    const Session& s = *p.session;
    ProcInfo info;
    info.id = id;
    info.name = s.name();
    info.dir = s.dir();
    info.argv = s.argv();
    info.pid = s.pid();
    info.created_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(s.created().time_since_epoch()).count());
    info.exited = s.exited();
    info.wait_status = s.exit_status();
    info.created = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(s.created().time_since_epoch()).count());
    info.bell = s.bell();
    info.fg_command = s.fg_command();
    list.push_back(std::move(info));
  }
  send(c, make_frame(MsgType::ListReply, encode_proc_list(list)));
}

void Server::do_new(Client& c, const Frame& frame) {
  if (stopping_) return send(c, error_frame("daemon is stopping"));
  PayloadReader r(frame.payload);
  SessionSpec spec;
  spec.name = r.str();
  spec.dir = r.str();
  spec.argv = r.strs();
  spec.env = r.strs();
  spec.rows = clamp_rows(r.u16());
  spec.cols = clamp_cols(r.u16());
  if (!r.at_end()) spec.umask = int(r.u32() & 0777);
  if (!r.ok()) return send(c, error_frame("malformed request"));
  spec.scrollback_lines = load_config_quiet().scrollback_lines;

  std::error_code ec;
  if (spec.dir.empty() || spec.dir.front() != '/' || !std::filesystem::is_directory(spec.dir, ec))
    return send(c, error_frame("not a directory: " + spec.dir));
  if (spec.name.empty()) {
    spec.name = default_name(spec.dir);
  } else if (!valid_name(spec.name)) {
    return send(c, error_frame("invalid name: " + spec.name));
  } else if (name_taken(spec.name)) {
    return send(c, error_frame("name already in use: " + spec.name));
  }
  if (spec.rows <= 0 || spec.cols <= 0) {
    spec.rows = 24;
    spec.cols = 80;
  }

  const std::uint32_t id = next_session_id_++;
  std::string error;
  auto session = Session::spawn(id, spec, error);
  if (!session) return send(c, error_frame(error));

  Proc& p = procs_[id];
  p.session = std::move(session);
  update_master_events(p);
  send(c, ok_frame(PayloadWriter().u32(id).str(spec.name).take()));
}

void Server::do_rename(Client& c, const Frame& frame) {
  PayloadReader r(frame.payload);
  const std::uint32_t id = r.u32();
  std::string name = r.str();
  if (!r.ok()) return send(c, error_frame("malformed request"));
  Proc* p = find_proc(id);
  if (!p) return send(c, error_frame("no such process"));
  if (name == p->session->name()) return send(c, ok_frame());
  if (!valid_name(name)) return send(c, error_frame("invalid name: " + name));
  if (name_taken(name)) return send(c, error_frame("name already in use: " + name));
  p->session->set_name(std::move(name));
  send(c, ok_frame());
}

void Server::do_attach(Client& c, const Frame& frame) {
  PayloadReader r(frame.payload);
  const std::uint32_t id = r.u32();
  const int rows = clamp_rows(r.u16());
  const int cols = clamp_cols(r.u16());
  const std::string tty = r.str();
  if (!r.ok()) return send(c, error_frame("malformed request"));
  Proc* p = find_proc(id);
  if (!p) return send(c, error_frame("no such process"));
  if (p->session->exited()) return send(c, error_frame(p->session->name() + " has exited"));
  // A client running inside the process itself would feed the process its own output; so would
  // one whose output reaches the process through a chain of attached clients.
  const dev_t rdev = tty_device(tty);
  if ((rdev != 0 && rdev == p->session->tty_rdev()) || (!tty.empty() && tty == p->session->tty_path()))
    return send(c, error_frame("cannot attach " + p->session->name() + " to itself"));
  if (attach_loop(c, *p, rdev))
    return send(c, error_frame("cannot attach " + p->session->name() + ": would create an attach loop"));
  c.tty_rdev = rdev;

  if (Proc* old = find_proc(c.attached); old && old != p) {
    old->attached = 0;
    update_master_events(*old);
  }
  if (Client* prev = find_client(p->attached); prev && prev != &c) {
    prev->attached = 0;
    prev->scroll_top.reset();
    send(*prev, make_frame(MsgType::Detach, PayloadWriter().str(p->session->screen().color_resets()).take()));
  }
  p->attached = c.id;
  c.attached = id;
  c.viewing = 0;
  c.scroll_top.reset();
  p->session->clear_bell();
  send(c, ok_frame());
  // Resize the screen to the client size and snapshot first; then resize the PTY. The app gets
  // exactly one SIGWINCH: the kernel's if the size changed, else ours. (Both would reach it as
  // one or two signals depending on timing.)
  p->session->screen().resize(rows, cols);
  send_snapshot(c, p->session->screen().snapshot(true));
  send_state(c, p->session->screen(), true);
  if (!p->session->resize(rows, cols)) p->session->notify_winch();
  update_master_events(*p);
}

void Server::do_detach(Client& c) {
  std::string resets;
  if (Proc* p = find_proc(c.attached); p && p->attached == c.id) {
    resets = p->session->screen().color_resets();
    p->attached = 0;
    update_master_events(*p);
  }
  c.attached = 0;
  c.scroll_top.reset();
  send(c, make_frame(MsgType::Detach, PayloadWriter().str(resets).take()));
}

void Server::do_view(Client& c, const Frame& frame) {
  PayloadReader r(frame.payload);
  const std::uint32_t id = r.u32();
  int rows = 0, cols = 0;
  if (!r.at_end()) {  // optional for older clients
    rows = clamp_rows(r.u16());
    cols = clamp_cols(r.u16());
  }
  Proc* p = r.ok() ? find_proc(id) : nullptr;
  if (!p) return send(c, error_frame("no such process"));
  Screen& screen = p->session->screen();
  // An exited process's screen takes the viewer's size (a running one keeps its PTY's).
  if (p->session->exited()) screen.resize(rows, cols);
  c.viewing = id;
  c.scroll_top.reset();
  send_snapshot(c, screen.snapshot(false));
  send(c, ok_frame(PayloadWriter().str(screen.color_resets()).take()));
}

void Server::send_snapshot(Client& c, const std::string& bytes) {
  c.snapshot_size = bytes.size();  // bounded by the scrollback size, however large
  for (std::size_t off = 0; off < bytes.size(); off += kSnapshotChunk)
    send(c, bytes_frame(MsgType::Snapshot, std::string_view(bytes).substr(off, kSnapshotChunk)));
}

void Server::send_state(Client& c, const Screen& screen, bool reapply) {
  if (c.scroll_top) return;  // frozen until scroll mode ends
  const ClientState st = screen.client_state();
  if (!reapply && c.state == st) return;
  c.state = st;
  PayloadWriter w;
  w.u8(std::uint8_t(st.flags | (reapply ? kStateReapply : 0))).u16(st.mouse);
  w.u32(0).u32(std::uint32_t(screen.history_size()));
  send(c, make_frame(MsgType::State, w.take()));
}

void Server::send_scroll_state(Client& c, const Screen& screen) {
  const ClientState st = c.state.value_or(screen.client_state());
  const std::uint64_t end = screen.history_end();
  const std::uint64_t offset = c.scroll_top ? end - std::min(*c.scroll_top, end) : 0;
  PayloadWriter w;
  w.u8(std::uint8_t(st.flags | (c.scroll_top ? kStateScrolled : 0))).u16(st.mouse);
  w.u32(std::uint32_t(offset)).u32(std::uint32_t(screen.history_size()));
  send(c, make_frame(MsgType::State, w.take()));
}

void Server::do_scroll(Client& c, const Frame& frame) {
  PayloadReader r(frame.payload);
  const auto op = static_cast<ScrollOp>(r.u8());
  const std::uint16_t lines = r.at_end() ? 1 : r.u16();
  if (!r.ok()) return;
  Proc* p = find_proc(c.attached ? c.attached : c.viewing);
  if (!p || (c.attached && p->attached != c.id)) return;
  Screen& screen = p->session->screen();
  const std::uint64_t base = screen.history_base();
  const std::uint64_t end = screen.history_end();  // the top of the live screen
  const auto page = std::uint64_t(std::max(screen.rows() - 1, 1));
  std::uint64_t top = std::clamp(c.scroll_top.value_or(end), base, end);
  switch (op) {
    case ScrollOp::Up: top -= std::min<std::uint64_t>(lines, top - base); break;
    case ScrollOp::Down: top = std::min<std::uint64_t>(top + lines, end); break;
    case ScrollOp::PageUp: top -= std::min(page, top - base); break;
    case ScrollOp::PageDown: top = std::min(top + page, end); break;
    case ScrollOp::Top: top = base; break;
    case ScrollOp::Bottom:
    case ScrollOp::Exit: top = end; break;
    default: return;
  }
  if (top >= end) {
    if (c.scroll_top) exit_scroll(c, *p);
    return;
  }
  c.scroll_top = top;  // freezes c.state (see send_state)
  send_snapshot(c, screen.scroll_paint(top));
  send_scroll_state(c, screen);
}

// Leaves scroll mode: paints the live screen and resumes the output.
void Server::exit_scroll(Client& c, Proc& p) {
  c.scroll_top.reset();
  Screen& screen = p.session->screen();
  if (!c.attached) {
    send_snapshot(c, screen.snapshot(false));
    send_scroll_state(c, screen);
    return;
  }
  send_snapshot(c, screen.snapshot(true, false));
  send_state(c, screen, true);
}

void Server::do_resize(Client& c, const Frame& frame) {
  PayloadReader r(frame.payload);
  const int rows = clamp_rows(r.u16());
  const int cols = clamp_cols(r.u16());
  Proc* p = find_proc(c.attached);
  if (!r.ok() || !p) return;
  p->session->resize(rows, cols);
  if (c.scroll_top) exit_scroll(c, *p);
}

void Server::do_input(Client& c, const Frame& frame) {
  Proc* p = find_proc(c.attached);
  if (!p) return;
  if (c.scroll_top) exit_scroll(c, *p);
  if (p->session->pending_input() + frame.payload.size() > kMaxQueue) {
    c.dead = true;  // the process is not reading its input
    return;
  }
  p->session->queue_input({reinterpret_cast<const char*>(frame.payload.data()), frame.payload.size()});
  update_master_events(*p);
}

void Server::do_kill(Client& c, const Frame& frame) {
  PayloadReader r(frame.payload);
  const std::uint32_t id = r.u32();
  const bool remove = !r.at_end() && r.u8() != 0;  // optional for older clients
  Proc* p = r.ok() ? find_proc(id) : nullptr;
  if (!p) return send(c, error_frame("no such process"));
  if (p->session->exited()) {
    procs_.erase(id);
    return send(c, ok_frame());
  }
  if (remove) p->remove_when_exited = true;
  p->kill_waiters.push_back(c.id);
  if (!p->kill_deadline && !p->session->reaped()) {
    p->session->hangup();
    p->kill_deadline = Clock::now() + kKillGrace;
  }
}

void Server::do_remove(Client& c, const Frame& frame) {
  PayloadReader r(frame.payload);
  const std::uint32_t id = r.u32();
  Proc* p = r.ok() ? find_proc(id) : nullptr;
  if (!p) return send(c, error_frame("no such process"));
  if (!p->session->exited()) return send(c, error_frame(p->session->name() + " is still running"));
  procs_.erase(id);
  send(c, ok_frame());
}

void Server::do_stop(Client& c, const Frame& frame) {
  PayloadReader r(frame.payload);
  const bool force = r.u8() != 0;
  if (!r.ok()) return send(c, error_frame("malformed request"));
  std::vector<Proc*> running;
  for (auto& [id, p] : procs_)
    if (!p.session->exited()) running.push_back(&p);
  if (!running.empty() && !force && !stopping_) {
    std::string names;
    for (const Proc* p : running) names += (names.empty() ? "" : ", ") + p->session->name();
    return send(c, error_frame(std::to_string(running.size()) + " process(es) still running: " + names +
                               "\nuse pmux --stop --force to kill them and stop the daemon"));
  }
  stopping_ = true;
  stop_waiters_.push_back(c.id);
  for (Proc* p : running) {
    if (p->kill_deadline || p->session->reaped()) continue;
    p->session->hangup();
    p->kill_deadline = Clock::now() + kKillGrace;
  }
}

// Once every process has exited, answers the STOP requests and ends the loop.
void Server::finish_stop() {
  for (const auto& [id, p] : procs_)
    if (!p.session->exited()) return;
  for (std::uint32_t cid : std::exchange(stop_waiters_, {}))
    if (Client* c = find_client(cid)) send(*c, ok_frame());
  running_ = false;
}

void Server::send(Client& c, const Frame& frame) {
  if (c.dead) return;
  const auto bytes = encode_frame(frame);
  c.out.append(bytes.begin(), bytes.end());
  flush(c);
  if (c.pending() > kMaxQueue + c.snapshot_size) c.dead = true;  // the client is not reading
}

void Server::flush(Client& c) {
  while (c.pending() > 0) {
    const ssize_t n = write(c.fd.get(), c.out.data() + c.out_off, c.pending());
    if (n > 0) {
      c.out_off += std::size_t(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      if (!(n < 0 && errno == EAGAIN)) c.dead = true;
      break;
    }
  }
  if (c.pending() == 0) {
    c.out.clear();
    c.out_off = 0;
  } else if (c.out_off > (1u << 16) && c.out_off * 2 > c.out.size()) {
    c.out.erase(0, c.out_off);
    c.out_off = 0;
  }
  update_client_events(c);
  if (Proc* p = find_proc(c.attached)) update_master_events(*p);
}

void Server::update_client_events(Client& c) {
  const bool want = !c.dead && c.pending() > 0;
  if (want == c.want_write) return;
  epoll_event ev{};
  ev.events = EPOLLIN | (want ? EPOLLOUT : 0u);
  ev.data.u64 = make_tag(Tag::Client, c.id);
  epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, c.fd.get(), &ev);
  c.want_write = want;
}

void Server::close_dead_clients() {
  for (auto it = clients_.begin(); it != clients_.end();) {
    Client& c = it->second;
    if (!c.dead) {
      ++it;
      continue;
    }
    if (Proc* p = find_proc(c.attached); p && p->attached == c.id) {
      p->attached = 0;
      update_master_events(*p);
    }
    it = clients_.erase(it);
  }
}

bool Server::output_paused(const Proc& p) {
  const Client* c = find_client(p.attached);
  return c && c->pending() > kOutputHighWater;
}

void Server::update_master_events(Proc& p) {
  const Session& s = *p.session;
  if (s.master_fd() < 0) return;
  std::uint32_t want = 0;
  if (!output_paused(p)) want |= EPOLLIN;
  if (s.has_pending_input()) want |= EPOLLOUT;
  if (want == p.events) return;

  epoll_event ev{};
  ev.events = want;
  ev.data.u64 = make_tag(Tag::Master, s.id());
  const int op = p.events == 0 ? EPOLL_CTL_ADD : want == 0 ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
  epoll_ctl(epoll_fd_.get(), op, s.master_fd(), &ev);
  p.events = want;
}

void Server::on_master_event(std::uint32_t id, std::uint32_t events) {
  Proc* p = find_proc(id);
  if (!p || p->session->master_fd() < 0) return;
  if (events & EPOLLOUT) p->session->flush_input();
  if (events & (EPOLLIN | EPOLLHUP | EPOLLERR))
    read_master(*p);
  else
    update_master_events(*p);
}

void Server::read_master(Proc& p) {
  Session& s = *p.session;
  std::vector<Screen::OutputPiece> pieces;
  for (int i = 0; i < 16 && !output_paused(p); ++i) {
    Client* c = find_client(p.attached);
    const bool scrolled = c && c->scroll_top;
    const auto status = s.read_output(pieces, p.attached != 0, scrolled);
    if (status == Session::ReadStatus::Again) break;
    if (status == Session::ReadStatus::Eof) {
      close_master(p);
      if (s.reaped()) finalize(p);
      return;
    }
    // Output is withheld from a client in scroll mode; it gets a snapshot when that ends.
    if (c && !scrolled) {
      for (const auto& piece : pieces) {
        if (!piece.bytes.empty()) send(*c, bytes_frame(MsgType::Output, piece.bytes));
        send_state(*c, s.screen(), piece.reapply);
      }
    }
  }
  if (!s.screen().holding()) p.held_deadline.reset();
  else if (!p.held_deadline) p.held_deadline = Clock::now() + kHoldTime;
  update_master_events(p);
}

void Server::flush_held(Proc& p) {
  p.held_deadline.reset();
  const std::string held = p.session->screen().take_held();
  Client* c = find_client(p.attached);
  if (c && !c->scroll_top && !held.empty()) send(*c, bytes_frame(MsgType::Output, held));
}

void Server::close_master(Proc& p) {
  Session& s = *p.session;
  if (s.master_fd() < 0) return;
  if (p.events != 0) epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, s.master_fd(), nullptr);
  p.events = 0;
  s.close_master();
}

void Server::reap_children() {
  int status = 0;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (auto& [id, p] : procs_) {
      Session& s = *p.session;
      if (s.pid() != pid || s.reaped()) continue;
      s.set_reaped(status);
      if (s.master_fd() < 0)
        finalize(p);
      else
        p.reap_deadline = Clock::now() + kDrainGrace;
      break;
    }
  }
}

void Server::finalize(Proc& p) {
  Session& s = *p.session;
  close_master(p);
  s.finish();
  p.kill_deadline.reset();
  p.reap_deadline.reset();
  if (Client* c = find_client(p.attached)) {
    c->attached = 0;
    c->scroll_top.reset();
    send(*c, make_frame(MsgType::Exited,
                        PayloadWriter().i32(s.exit_status()).str(s.screen().color_resets()).take()));
  }
  p.attached = 0;
  // With the remove flag, remove_finished drops the entry at the end of this event pass, before
  // any later request (from these waiters or anyone else) is handled.
  for (std::uint32_t cid : std::exchange(p.kill_waiters, {}))
    if (Client* c = find_client(cid)) send(*c, ok_frame());
}

// Drops exited processes killed with the remove flag. Runs after each event pass, outside the
// loops over procs_ that finalize them.
void Server::remove_finished() {
  std::erase_if(procs_, [](const auto& entry) {
    return entry.second.remove_when_exited && entry.second.session->exited();
  });
}

void Server::run_timers() {
  const auto now = Clock::now();
  for (auto& [id, p] : procs_) {
    if (p.held_deadline && now >= *p.held_deadline) flush_held(p);
    if (p.kill_deadline && now >= *p.kill_deadline) {
      p.kill_deadline.reset();
      p.session->force_kill();
    }
    if (p.reap_deadline && now >= *p.reap_deadline) {
      // The child is gone but something else still holds the PTY open.
      read_master(p);
      if (!p.session->exited()) finalize(p);
    }
  }
}

int Server::timeout_ms() const {
  std::optional<Clock::time_point> next;
  for (const auto& [id, p] : procs_)
    for (const auto& d : {p.kill_deadline, p.reap_deadline, p.held_deadline})
      if (d && (!next || *d < *next)) next = d;
  if (!next) return -1;
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(*next - Clock::now()).count();
  return static_cast<int>(std::max<decltype(ms)>(ms, 0));
}

Server::Client* Server::find_client(std::uint32_t id) {
  auto it = clients_.find(id);
  return it == clients_.end() ? nullptr : &it->second;
}

Server::Proc* Server::find_proc(std::uint32_t id) {
  auto it = procs_.find(id);
  return it == procs_.end() ? nullptr : &it->second;
}

Server::Proc* Server::find_proc_by_tty(dev_t rdev) {
  if (rdev == 0) return nullptr;
  for (auto& [id, p] : procs_)
    if (p.session->tty_rdev() == rdev) return &p;
  return nullptr;
}

// Would attaching client `c` (on terminal `tty`) to `target` feed target's output back into
// it? Follows: the process whose PTY is c's terminal -> the client attached to that process ->
// the process whose PTY is that client's terminal -> ...
bool Server::attach_loop(const Client& c, const Proc& target, dev_t tty) {
  std::set<std::uint32_t> visited;
  for (Proc* cur = find_proc_by_tty(tty); cur && visited.insert(cur->session->id()).second;) {
    if (cur == &target) return true;
    const Client* holder = find_client(cur->attached);
    if (!holder || holder == &c) return false;
    cur = find_proc_by_tty(holder->tty_rdev);
  }
  return false;
}

bool Server::name_taken(std::string_view name) const {
  return std::any_of(procs_.begin(), procs_.end(),
                     [&](const auto& entry) { return entry.second.session->name() == name; });
}

std::string Server::default_name(const std::string& dir) const {
  std::string base = std::filesystem::path(dir).filename().string();
  if (!valid_name(base)) base = "root";
  std::string name = base;
  for (int n = 2; name_taken(name); ++n) name = base + "-" + std::to_string(n);
  return name;
}

int run_daemon() {
  prctl(PR_SET_NAME, "pmux", 0, 0, 0);  // spawned via /proc/self/exe, it would show as "exe"
  std::string error;
  if (!ensure_socket_dir(error)) {
    log_error(error);
    return 1;
  }
  const std::string pid_file = pid_path();
  const std::string sock = socket_path();

  // The pidfile doubles as the single-instance lock.
  UniqueFd pid_fd(open(pid_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600));
  if (!pid_fd) {
    log_error(pid_file + ": " + std::strerror(errno));
    return 1;
  }
  if (flock(pid_fd.get(), LOCK_EX | LOCK_NB) != 0) {
    log_error("daemon already running");
    return 1;
  }
  const std::string pid_text = std::to_string(getpid()) + "\n";
  if (ftruncate(pid_fd.get(), 0) != 0 ||
      pwrite(pid_fd.get(), pid_text.data(), pid_text.size(), 0) != ssize_t(pid_text.size())) {
    log_error(pid_file + ": " + std::strerror(errno));
    return 1;
  }

  UniqueFd listen_fd = listen_on(sock, error);
  if (!listen_fd) {
    log_error(error);
    unlink(pid_file.c_str());
    return 1;
  }
  (void)!chdir("/");

  const int rc = Server(std::move(listen_fd)).run();
  unlink(sock.c_str());
  unlink(pid_file.c_str());
  return rc;
}

}  // namespace pmux
