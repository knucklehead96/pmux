#include "common/protocol.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>

namespace pmux {

namespace {

std::uint32_t load_u32(const std::uint8_t* p) {
  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
         std::uint32_t(p[3]) << 24;
}

}  // namespace

std::vector<std::uint8_t> encode_frame(const Frame& frame) {
  std::vector<std::uint8_t> out;
  out.reserve(kFrameHeaderSize + frame.payload.size());
  const auto len = static_cast<std::uint32_t>(frame.payload.size() + 1);
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(len >> (8 * i)));
  out.push_back(static_cast<std::uint8_t>(frame.type));
  out.insert(out.end(), frame.payload.begin(), frame.payload.end());
  return out;
}

bool frame_header_valid(const std::uint8_t* buf, std::size_t len) {
  if (len < 4) return true;
  const std::uint32_t n = load_u32(buf);
  return n >= 1 && n <= kMaxFrameLen;
}

std::optional<Frame> decode_frame(const std::uint8_t* buf, std::size_t len, std::size_t& consumed) {
  consumed = 0;
  if (len < kFrameHeaderSize || !frame_header_valid(buf, len)) return std::nullopt;
  const std::size_t total = 4 + std::size_t(load_u32(buf));
  if (len < total) return std::nullopt;
  Frame frame{static_cast<MsgType>(buf[4]), {buf + kFrameHeaderSize, buf + total}};
  consumed = total;
  return frame;
}

void FrameDecoder::feed(const void* data, std::size_t len) {
  if (pos_ > 0) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ = 0;
  }
  const auto* p = static_cast<const std::uint8_t*>(data);
  buf_.insert(buf_.end(), p, p + len);
}

std::optional<Frame> FrameDecoder::next() {
  if (bad_) return std::nullopt;
  const std::uint8_t* p = buf_.data() + pos_;
  const std::size_t avail = buf_.size() - pos_;
  if (!frame_header_valid(p, avail)) {
    bad_ = true;
    return std::nullopt;
  }
  std::size_t consumed = 0;
  auto frame = decode_frame(p, avail, consumed);
  pos_ += consumed;
  return frame;
}

void PayloadWriter::put(std::uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

PayloadWriter& PayloadWriter::u8(std::uint8_t v) {
  put(v, 1);
  return *this;
}

PayloadWriter& PayloadWriter::u16(std::uint16_t v) {
  put(v, 2);
  return *this;
}

PayloadWriter& PayloadWriter::u32(std::uint32_t v) {
  put(v, 4);
  return *this;
}

PayloadWriter& PayloadWriter::u64(std::uint64_t v) {
  put(v, 8);
  return *this;
}

PayloadWriter& PayloadWriter::str(std::string_view s) {
  u32(static_cast<std::uint32_t>(s.size()));
  out_.insert(out_.end(), s.begin(), s.end());
  return *this;
}

PayloadWriter& PayloadWriter::strs(const std::vector<std::string>& v) {
  u32(static_cast<std::uint32_t>(v.size()));
  for (const auto& s : v) str(s);
  return *this;
}

bool PayloadReader::need(std::size_t n) {
  if (ok_ && p_.size() - pos_ >= n) return true;
  ok_ = false;
  return false;
}

std::uint64_t PayloadReader::get(int bytes) {
  if (!need(std::size_t(bytes))) return 0;
  std::uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) v |= std::uint64_t(p_[pos_++]) << (8 * i);
  return v;
}

std::string PayloadReader::str() {
  const std::uint32_t n = u32();
  if (!need(n)) return {};
  std::string s(p_.begin() + static_cast<std::ptrdiff_t>(pos_),
                p_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
  pos_ += n;
  return s;
}

std::vector<std::string> PayloadReader::strs() {
  const std::uint32_t n = u32();
  if (!need(std::size_t(n) * 4)) return {};
  std::vector<std::string> v;
  v.reserve(n);
  for (std::uint32_t i = 0; i < n && ok_; ++i) v.push_back(str());
  return v;
}

Frame bytes_frame(MsgType type, std::string_view bytes) {
  return Frame{type, {bytes.begin(), bytes.end()}};
}

Frame error_frame(std::string_view message) {
  return make_frame(MsgType::Error, PayloadWriter().str(message).take());
}

std::vector<std::uint8_t> encode_proc_list(const std::vector<ProcInfo>& procs) {
  PayloadWriter w;
  w.u32(static_cast<std::uint32_t>(procs.size()));
  for (const auto& p : procs) {
    w.u32(p.id).str(p.name).str(p.dir).strs(p.argv).i32(p.pid);
    w.u64(p.created_ms).u64(p.idle_ms).u8(p.exited ? 1 : 0).i32(p.wait_status);
  }
  return w.take();
}

std::optional<std::vector<ProcInfo>> decode_proc_list(const std::vector<std::uint8_t>& payload) {
  PayloadReader r(payload);
  const std::uint32_t n = r.u32();
  std::vector<ProcInfo> procs;
  for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
    ProcInfo p;
    p.id = r.u32();
    p.name = r.str();
    p.dir = r.str();
    p.argv = r.strs();
    p.pid = r.i32();
    p.created_ms = r.u64();
    p.idle_ms = r.u64();
    p.exited = r.u8() != 0;
    p.wait_status = r.i32();
    procs.push_back(std::move(p));
  }
  if (!r.ok()) return std::nullopt;
  return procs;
}

bool write_all(int fd, const void* data, std::size_t len) {
  const auto* p = static_cast<const char*>(data);
  while (len > 0) {
    const ssize_t n = ::write(fd, p, len);
    if (n > 0) {
      p += n;
      len -= std::size_t(n);
    } else if (n < 0 && errno == EAGAIN) {
      pollfd pfd{fd, POLLOUT, 0};
      ::poll(&pfd, 1, -1);
    } else if (!(n < 0 && errno == EINTR)) {
      return false;
    }
  }
  return true;
}

bool send_frame(int fd, const Frame& frame) {
  const auto bytes = encode_frame(frame);
  return write_all(fd, bytes.data(), bytes.size());
}

std::optional<Frame> recv_frame(int fd, FrameDecoder& decoder, int timeout_ms) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (auto frame = decoder.next()) return frame;
    if (decoder.bad()) return std::nullopt;

    int wait = -1;
    if (timeout_ms >= 0) {
      const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
      if (left <= 0) return std::nullopt;
      wait = static_cast<int>(left);
    }
    pollfd pfd{fd, POLLIN, 0};
    const int r = ::poll(&pfd, 1, wait);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return std::nullopt;

    std::uint8_t buf[65536];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (n <= 0) return std::nullopt;
    decoder.feed(buf, std::size_t(n));
  }
}

}  // namespace pmux
