#pragma once

#include "common/fd.hpp"

namespace pmux {

// Returns a connected socket, or an empty fd.
UniqueFd connect_daemon();

// Connects, spawning the daemon (double fork + setsid) if it is not running.
UniqueFd connect_or_spawn_daemon();

}  // namespace pmux
