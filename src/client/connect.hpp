#pragma once

#include <string>

#include "common/fd.hpp"

namespace pmux {

// Returns a connected socket to our own user's daemon, or an empty fd and sets `error`.
// Refuses a socket directory other users can access and a daemon owned by another uid.
// `not_running` (if given) is set when the failure is only that no daemon is listening.
UniqueFd connect_daemon(std::string* error = nullptr, bool* not_running = nullptr);

// Connects, spawning the daemon (double fork + setsid) if it is not running.
UniqueFd connect_or_spawn_daemon(std::string* error = nullptr);

}  // namespace pmux
