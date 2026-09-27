#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>

#include "common/fd.hpp"

namespace pmux {

// Returns a connected socket to our own user's daemon, or an empty fd and sets `error`.
// Refuses a socket directory other users can access and a daemon owned by another uid.
// The connection has passed HELLO: a daemon of another protocol version (or one older than
// HELLO) is refused with a message telling the user to restart it.
// `not_running` (if given) is set when the failure is only that no daemon is listening.
UniqueFd connect_daemon(std::string* error = nullptr, bool* not_running = nullptr);

// Connects, spawning the daemon (double fork + setsid) if it is not running.
UniqueFd connect_or_spawn_daemon(std::string* error = nullptr);

// What HELLO learned about the daemon.
enum class Handshake { Match, Mismatch, Old, NoReply };
struct DaemonInfo {
  std::uint32_t protocol = 0;
  std::string version;
  pid_t pid = 0;  // the daemon's pid (SO_PEERCRED; the one HELLO reported if it did)
};

// Like connect_daemon, without HELLO (pmux --stop, which also handles other daemons).
// `peer_pid` is the pid of the process listening on the socket.
UniqueFd connect_daemon_unchecked(std::string& error, bool& not_running, pid_t& peer_pid);

// Sends HELLO and reads the reply. Old: the daemon answered with ERROR (it predates HELLO).
Handshake handshake(int fd, DaemonInfo& info);

// "the running daemon (pid N, ...) doesn't match this pmux (...). Restart it with: pmux --stop"
std::string mismatch_message(Handshake result, const DaemonInfo& info);

}  // namespace pmux
