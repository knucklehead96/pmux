#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pmux {

enum class ThemeMode { Auto, Dark, Light, Ansi };
enum class Accent { Clay, Blue, Purple, Teal };

struct Config {
  std::string default_dir;  // expanded; empty if unset
  std::string default_cmd;  // expanded; empty if unset
  ThemeMode theme = ThemeMode::Auto;
  Accent accent = Accent::Clay;
};

// Reads ~/.pmux/config (missing file = defaults); warnings go to stderr.
Config load_config();

// Parses config text; `warnings` receives complete warning lines (without "pmux: " or newline).
Config parse_config(std::string_view text, std::vector<std::string>& warnings);

// Expands a leading `~` and $VAR / ${VAR} references.
std::string expand_value(std::string_view value);

// Splits a command line on whitespace, honoring simple '...' and "..." quoting.
std::vector<std::string> split_command(std::string_view command);

// $HOME, or empty.
std::string home_dir();

}  // namespace pmux
