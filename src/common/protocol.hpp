#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pmux {

// Payloads (all integers little-endian; str = u32 length + bytes; strs = u32 count + str...):
//   LIST        -                                  -> LIST_REPLY
//   LIST_REPLY  u32 count, ProcInfo...
//   NEW         str name, str dir, strs argv, strs env, u16 rows, u16 cols [, u32 umask]
//                                                  -> OK(u32 id, str name) | ERROR
//               (umask: the client's, applied to the process; optional for older clients)
//   RENAME      u32 id, str name                   -> OK | ERROR
//   KILL        u32 id  (exited: removes it)       -> OK once the process has exited | ERROR
//   REMOVE      u32 id  (exited processes only)    -> OK | ERROR
//   ATTACH      u32 id, u16 rows, u16 cols, str client tty path ("" if unknown)
//                                                  -> OK, SNAPSHOT..., then OUTPUT... | ERROR
//               (rows/cols in NEW, ATTACH and RESIZE are clamped to 500/1000 by the daemon)
//   VIEW        u32 id  (read-only screen)         -> SNAPSHOT..., then OK(str color resets) | ERROR
//   RESIZE      u16 rows, u16 cols
//   INPUT       raw bytes (client -> daemon)
//   OUTPUT      raw bytes (daemon -> client)
//   SNAPSHOT    raw bytes (daemon -> client): screen restore, written verbatim; may span frames
//   DETACH      client -> daemon: -  (detach request; answered with DETACH)
//               daemon -> client: str color resets  (reply, or attached elsewhere)
//   EXITED      i32 wait status, str color resets
//   ERROR       str message
enum class MsgType : std::uint8_t {
  List = 1,
  ListReply,
  New,
  Rename,
  Kill,
  Remove,
  Attach,
  Resize,
  Input,
  Output,
  Snapshot,
  Detach,
  Exited,
  Error,
  Ok,
  View,
};

struct Frame {
  MsgType type;
  std::vector<std::uint8_t> payload;
};

// Wire format: [u32 len][u8 type][payload], len (little-endian) counts type + payload.
inline constexpr std::size_t kFrameHeaderSize = 5;
inline constexpr std::uint32_t kMaxFrameLen = 16u << 20;

std::vector<std::uint8_t> encode_frame(const Frame& frame);

// Decodes one complete frame from the front of buf; returns bytes consumed via `consumed`.
// Returns nullopt (consumed = 0) if buf holds no complete frame yet.
std::optional<Frame> decode_frame(const std::uint8_t* buf, std::size_t len, std::size_t& consumed);

// False if buf starts with a length field that can never be valid.
bool frame_header_valid(const std::uint8_t* buf, std::size_t len);

// Accumulates stream bytes and yields complete frames.
class FrameDecoder {
 public:
  void feed(const void* data, std::size_t len);
  std::optional<Frame> next();
  bool bad() const { return bad_; }

 private:
  std::vector<std::uint8_t> buf_;
  std::size_t pos_ = 0;
  bool bad_ = false;
};

class PayloadWriter {
 public:
  PayloadWriter& u8(std::uint8_t v);
  PayloadWriter& u16(std::uint16_t v);
  PayloadWriter& u32(std::uint32_t v);
  PayloadWriter& i32(std::int32_t v) { return u32(static_cast<std::uint32_t>(v)); }
  PayloadWriter& u64(std::uint64_t v);
  PayloadWriter& str(std::string_view s);
  PayloadWriter& strs(const std::vector<std::string>& v);
  std::vector<std::uint8_t> take() { return std::move(out_); }

 private:
  void put(std::uint64_t v, int bytes);
  std::vector<std::uint8_t> out_;
};

// Bounds-checked reader; on underflow ok() turns false and reads return zero/empty.
class PayloadReader {
 public:
  explicit PayloadReader(const std::vector<std::uint8_t>& payload) : p_(payload) {}
  std::uint8_t u8() { return static_cast<std::uint8_t>(get(1)); }
  std::uint16_t u16() { return static_cast<std::uint16_t>(get(2)); }
  std::uint32_t u32() { return static_cast<std::uint32_t>(get(4)); }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  std::uint64_t u64() { return get(8); }
  std::string str();
  std::vector<std::string> strs();
  bool ok() const { return ok_; }
  bool at_end() const { return pos_ >= p_.size(); }

 private:
  bool need(std::size_t n);
  std::uint64_t get(int bytes);
  const std::vector<std::uint8_t>& p_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

inline Frame make_frame(MsgType type, std::vector<std::uint8_t> payload = {}) {
  return Frame{type, std::move(payload)};
}
Frame bytes_frame(MsgType type, std::string_view bytes);
Frame error_frame(std::string_view message);

struct ProcInfo {
  std::uint32_t id = 0;
  std::string name;
  std::string dir;
  std::vector<std::string> argv;
  std::int32_t pid = 0;
  std::uint64_t created_ms = 0;  // unix epoch
  std::uint64_t idle_ms = 0;     // since last output, or since start if none
  bool exited = false;
  std::int32_t wait_status = 0;
  std::uint64_t created = 0;      // unix seconds
  std::uint64_t last_output = 0;  // unix seconds, 0 if none
  bool bell = false;              // BEL seen since the last attach
  std::string fg_command;         // foreground process group's argv, joined; empty if unknown
};

std::vector<std::uint8_t> encode_proc_list(const std::vector<ProcInfo>& procs);
std::optional<std::vector<ProcInfo>> decode_proc_list(const std::vector<std::uint8_t>& payload);

// Blocking I/O helpers (client side).
bool write_all(int fd, const void* data, std::size_t len);
bool send_frame(int fd, const Frame& frame);
// Waits up to timeout_ms (-1 = forever) for the next frame; nullopt on EOF, error or timeout.
std::optional<Frame> recv_frame(int fd, FrameDecoder& decoder, int timeout_ms);

}  // namespace pmux
