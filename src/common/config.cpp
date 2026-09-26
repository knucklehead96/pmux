#include "common/config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>

namespace pmux {

namespace {

constexpr std::string_view kDisplayPath = "~/.pmux/config";

bool is_space(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r';
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

// Drops a `#` comment that starts the line or follows whitespace.
std::string_view strip_comment(std::string_view line) {
  for (std::size_t i = 0; i < line.size(); ++i)
    if (line[i] == '#' && (i == 0 || is_space(line[i - 1]))) return line.substr(0, i);
  return line;
}

bool is_var_char(char ch, bool first) {
  return ch == '_' || std::isalpha(static_cast<unsigned char>(ch)) ||
         (!first && std::isdigit(static_cast<unsigned char>(ch)));
}

std::optional<ThemeMode> parse_theme(std::string_view v) {
  if (v == "auto") return ThemeMode::Auto;
  if (v == "dark") return ThemeMode::Dark;
  if (v == "light") return ThemeMode::Light;
  if (v == "ansi") return ThemeMode::Ansi;
  return std::nullopt;
}

std::optional<Accent> parse_accent(std::string_view v) {
  if (v == "clay") return Accent::Clay;
  if (v == "blue") return Accent::Blue;
  if (v == "purple") return Accent::Purple;
  if (v == "teal") return Accent::Teal;
  return std::nullopt;
}

// A decimal integer in [0, max].
std::optional<std::size_t> parse_count(std::string_view v, std::size_t max) {
  if (v.empty() || v.size() > 9) return std::nullopt;
  std::size_t n = 0;
  for (const char ch : v) {
    if (ch < '0' || ch > '9') return std::nullopt;
    n = n * 10 + std::size_t(ch - '0');
  }
  if (n > max) return std::nullopt;
  return n;
}

std::string read_config_text() {
  const std::string home = home_dir();
  if (home.empty()) return {};
  std::ifstream in(home + "/.pmux/config", std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

std::string home_dir() {
  const char* home = std::getenv("HOME");
  return home ? home : "";
}

std::string expand_value(std::string_view value) {
  std::string out;
  std::size_t i = 0;
  if (!value.empty() && value[0] == '~' && (value.size() == 1 || value[1] == '/')) {
    out = home_dir();
    i = 1;
  }
  while (i < value.size()) {
    const char ch = value[i];
    if (ch != '$' || i + 1 >= value.size()) {
      out += ch;
      ++i;
      continue;
    }
    std::string_view name;
    std::size_t next = i;
    if (value[i + 1] == '{') {
      const std::size_t close = value.find('}', i + 2);
      if (close != std::string_view::npos) {
        name = value.substr(i + 2, close - i - 2);
        next = close + 1;
      }
    } else if (is_var_char(value[i + 1], true)) {
      std::size_t j = i + 1;
      while (j < value.size() && is_var_char(value[j], j == i + 1)) ++j;
      name = value.substr(i + 1, j - i - 1);
      next = j;
    }
    if (next == i) {  // not a variable reference
      out += ch;
      ++i;
      continue;
    }
    if (const char* v = std::getenv(std::string(name).c_str())) out += v;
    i = next;
  }
  return out;
}

std::vector<std::string> split_command(std::string_view command) {
  std::vector<std::string> out;
  std::string cur;
  bool in_word = false;
  char quote = 0;
  for (const char ch : command) {
    if (quote) {
      if (ch == quote) quote = 0;
      else cur += ch;
    } else if (ch == '\'' || ch == '"') {
      quote = ch;
      in_word = true;
    } else if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
      if (in_word) out.push_back(std::move(cur));
      cur.clear();
      in_word = false;
    } else {
      cur += ch;
      in_word = true;
    }
  }
  if (in_word) out.push_back(std::move(cur));
  return out;
}

Config parse_config(std::string_view text, std::vector<std::string>& warnings) {
  Config config;
  int line_no = 0;
  while (!text.empty()) {
    const std::size_t nl = text.find('\n');
    std::string_view line = text.substr(0, nl);
    text = nl == std::string_view::npos ? std::string_view() : text.substr(nl + 1);
    ++line_no;

    line = trim(strip_comment(line));
    if (line.empty()) continue;
    const std::string where = std::string(kDisplayPath) + ":" + std::to_string(line_no) + ": ";
    const std::size_t eq = line.find('=');
    const std::string key(trim(line.substr(0, eq)));
    if (eq == std::string_view::npos) {
      warnings.push_back(where + "unknown key '" + key + "'");
      continue;
    }
    const std::string_view raw = trim(line.substr(eq + 1));
    const std::string value = expand_value(raw);
    auto invalid = [&] { warnings.push_back(where + "invalid value for " + key + ": '" + std::string(raw) + "'"); };

    if (key == "default_dir") {
      if (!value.empty() && value.front() != '/') invalid();
      else config.default_dir = value;
    } else if (key == "default_cmd") {
      if (!value.empty() && split_command(value).empty()) invalid();
      else config.default_cmd = value;
    } else if (key == "theme") {
      if (auto theme = parse_theme(value)) config.theme = *theme;
      else invalid();
    } else if (key == "accent") {
      if (auto accent = parse_accent(value)) config.accent = *accent;
      else invalid();
    } else if (key == "scrollback_lines") {
      if (auto n = parse_count(value, 100000)) config.scrollback_lines = *n;
      else invalid();
    } else {
      warnings.push_back(where + "unknown key '" + key + "'");
    }
  }
  return config;
}

Config load_config() {
  std::vector<std::string> warnings;
  Config config = parse_config(read_config_text(), warnings);
  for (const auto& w : warnings) std::fprintf(stderr, "pmux: %s\n", w.c_str());
  return config;
}

Config load_config_quiet() {
  std::vector<std::string> warnings;
  return parse_config(read_config_text(), warnings);
}

}  // namespace pmux
