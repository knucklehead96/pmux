#pragma once

#include <unistd.h>

#include <utility>

namespace pmux {

// Owning file descriptor.
class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int get() const { return fd_; }
  explicit operator bool() const { return fd_ >= 0; }

  int release() { return std::exchange(fd_, -1); }
  void reset(int fd = -1) {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace pmux
