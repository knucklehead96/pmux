#pragma once

#include <string>

#include "common/config.hpp"

namespace pmux {

// The list screen. `launch_dir` is the directory pmux was started from.
int run_tui(const Config& config, const std::string& launch_dir);

}  // namespace pmux
