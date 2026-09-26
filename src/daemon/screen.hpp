#pragma once

#include <cstddef>
#include <string>

struct VTerm;

namespace pmux {

class Screen {
 public:
  Screen(int rows, int cols, std::size_t scrollback_lines = 10000);
  ~Screen();
  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;

  void feed(const char* data, std::size_t len);
  void resize(int rows, int cols);
  std::string snapshot() const;

 private:
  VTerm* vt_ = nullptr;
  std::size_t scrollback_lines_;
};

}  // namespace pmux
