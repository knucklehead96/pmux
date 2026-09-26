#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/fd.hpp"
#include "common/protocol.hpp"
#include "daemon/session.hpp"

namespace pmux {

class Server {
 public:
  explicit Server(UniqueFd listen_fd);

  int run();

 private:
  using Clock = std::chrono::steady_clock;

  struct Client {
    std::uint32_t id = 0;
    UniqueFd fd;
    FrameDecoder decoder;
    std::string out;
    std::size_t out_off = 0;
    std::uint32_t attached = 0;  // session id
    std::uint32_t viewing = 0;   // session id shown by VIEW
    int view_rows = 0, view_cols = 0;  // the viewer's size
    std::optional<std::uint64_t> scroll_top;  // scroll mode: the line at the view's top
    std::string scroll_color_resets;  // scroll mode: the color resets at its start
    std::optional<ClientState> state;  // the last STATE sent (frozen while scrolled)
    dev_t tty_rdev = 0;          // device of the tty the client attached from (0 = unknown)
    std::size_t snapshot_size = 0;  // the last snapshot sent; allowed on top of the queue limit
    bool want_write = false;
    bool dead = false;
    std::size_t pending() const { return out.size() - out_off; }
  };

  struct Proc {
    std::unique_ptr<Session> session;
    std::uint32_t attached = 0;  // client id
    std::vector<std::uint32_t> kill_waiters;
    std::optional<Clock::time_point> kill_deadline;
    std::optional<Clock::time_point> reap_deadline;
    std::optional<Clock::time_point> held_deadline;  // flush the client output held back
    bool remove_when_exited = false;  // KILL with the remove flag
    std::uint32_t events = 0;  // current epoll interest on the master
  };

  bool setup();
  void on_accept();
  void on_signal();
  void on_client_event(std::uint32_t id, std::uint32_t events);
  void on_master_event(std::uint32_t id, std::uint32_t events);

  void handle_frame(Client& c, const Frame& frame);
  void do_list(Client& c);
  void do_new(Client& c, const Frame& frame);
  void do_rename(Client& c, const Frame& frame);
  void do_attach(Client& c, const Frame& frame);
  void do_detach(Client& c);
  void do_view(Client& c, const Frame& frame);
  void send_snapshot(Client& c, const std::string& bytes);
  void send_state(Client& c, const Screen& screen, bool reapply);
  void send_scroll_state(Client& c, const Screen& screen);
  void do_scroll(Client& c, const Frame& frame);
  void exit_scroll(Client& c, Proc& p);
  // Color resets for a client leaving a session: the colors the app changed, plus those it
  // changed and reset while the client was scrolled (the terminal still has them).
  static std::string client_color_resets(const Client& c, const Screen& screen);
  void flush_held(Proc& p);
  // After a snapshot to the attached client: its output continues at a sequence boundary.
  void resume_output(Proc& p);
  void do_resize(Client& c, const Frame& frame);
  void do_input(Client& c, const Frame& frame);
  void do_kill(Client& c, const Frame& frame);
  void do_remove(Client& c, const Frame& frame);
  void do_stop(Client& c, const Frame& frame);
  void finish_stop();

  void send(Client& c, const Frame& frame);
  void flush(Client& c);
  void update_client_events(Client& c);
  void close_dead_clients();

  bool output_paused(const Proc& p);
  void update_master_events(Proc& p);
  void read_master(Proc& p);
  void close_master(Proc& p);
  void reap_children();
  void finalize(Proc& p);
  void run_timers();
  void remove_finished();
  int timeout_ms() const;

  Client* find_client(std::uint32_t id);
  Proc* find_proc(std::uint32_t id);
  Proc* find_proc_by_tty(dev_t rdev);
  bool attach_loop(const Client& c, const Proc& target, dev_t tty);
  bool name_taken(std::string_view name) const;
  std::string default_name(const std::string& dir) const;

  UniqueFd listen_fd_;
  UniqueFd epoll_fd_;
  UniqueFd signal_fd_;
  bool running_ = true;
  bool stopping_ = false;  // STOP accepted: exit once every process has exited
  std::vector<std::uint32_t> stop_waiters_;
  std::uint32_t next_client_id_ = 1;
  std::uint32_t next_session_id_ = 1;
  std::map<std::uint32_t, Client> clients_;
  std::map<std::uint32_t, Proc> procs_;
};

// Creates the socket dir, takes the pidfile lock, listens and runs the loop.
int run_daemon();

}  // namespace pmux
