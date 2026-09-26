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
//   KILL        u32 id [, u8 remove]               -> OK once the process has exited | ERROR
//               (exited: removes it; remove = 1: also removes it once it has exited, before OK)
//   REMOVE      u32 id  (exited processes only)    -> OK | ERROR
//   ATTACH      u32 id, u16 rows, u16 cols, str client tty path ("" if unknown)
//                                                  -> OK, SNAPSHOT..., STATE, then OUTPUT / STATE /
//                                                     SNAPSHOT... | ERROR
//               (rows/cols in NEW, ATTACH, VIEW and RESIZE are clamped to 500/1000 by the daemon)
//               The client shows the session on its terminal's alternate screen. The app's own
//               alternate screen switches (DEC private modes 47 / 1047 / 1049) are removed from
//               OUTPUT and followed by a repaint of the screen the app switched to; RIS is
//               followed by CSI ? 1049 h and a repaint. Everything else in OUTPUT is the app's
//               output, byte-exact.
//   VIEW        u32 id [, u16 rows, u16 cols]  (read-only screen; an exited process's screen is
//               resized to rows x cols first)  -> SNAPSHOT..., then OK(str color resets) | ERROR
//   RESIZE      u16 rows, u16 cols
//   INPUT       raw bytes (client -> daemon); ends scroll mode first
//   OUTPUT      raw bytes (daemon -> client)
//   SNAPSHOT    raw bytes (daemon -> client): screen restore, written verbatim; may span frames
//   STATE       daemon -> client: u8 flags, u16 app mouse tracking mode (0 / 9 / 1000 / 1002 /
//               1003), u32 scroll offset (lines above the live screen), u32 history lines
//               flags: StateFlag. Sent after every SNAPSHOT (with Reapply) and, in stream order
//               with OUTPUT, whenever the flags or the mouse mode change. While scrolled the
//               app's modes are frozen at their values on entry and OUTPUT is withheld.
//   SCROLL      client -> daemon: u8 ScrollOp, u16 lines (Up / Down)
//               Scroll mode (per client, attached or viewing): the daemon paints the view with
//               SNAPSHOT and sends STATE with Scrolled set; reaching the bottom (or Exit) paints
//               the live screen (a snapshot; no SIGWINCH) and resumes OUTPUT.
//   DETACH      client -> daemon: -  (detach request; answered with DETACH)
//               daemon -> client: str color resets  (reply, or attached elsewhere)
//   EXITED      i32 wait status, str color resets
//   ERROR       str message
//   STOP        u8 force                           -> OK just before the daemon exits | ERROR
//               (force = 0: ERROR if any process is running; force = 1: SIGHUP every running
//               process, SIGKILL after 3 s, then exit)
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
  Stop,
  State,
  Scroll,
};

// STATE flags.
enum StateFlag : std::uint8_t {
  kStateMouseSgr = 1 << 0,     // the app has ?1006 (SGR mouse encoding)
  kStateMouseUtf8 = 1 << 1,    // ?1005
  kStateMouseUrxvt = 1 << 2,   // ?1015
  kStateAltScreen = 1 << 3,    // the app is on its alternate screen
  kStateCursorKeys = 1 << 4,   // DECCKM (?1)
  kStateScrolled = 1 << 5,     // the client is in scroll mode
  kStateReapply = 1 << 6,      // the terminal's modes were (re)set: apply the mouse modes again
  kStateMousePixels = 1 << 7,  // ?1016
};

enum class ScrollOp : std::uint8_t { Up = 1, Down, PageUp, PageDown, Top, Bottom, Exit };

struct ClientState {
  std::uint8_t flags = 0;  // StateFlag, without Scrolled / Reapply
  std::uint16_t mouse = 0;
  bool operator==(const ClientState&) const = default;
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
  bool exited = false;
  std::int32_t wait_status = 0;
  std::uint64_t created = 0;  // unix seconds
  bool bell = false;          // BEL seen since the last attach
  std::string fg_command;     // foreground process group's argv, joined; empty if unknown
};

std::vector<std::uint8_t> encode_proc_list(const std::vector<ProcInfo>& procs);
std::optional<std::vector<ProcInfo>> decode_proc_list(const std::vector<std::uint8_t>& payload);

// Blocking I/O helpers (client side).
bool write_all(int fd, const void* data, std::size_t len);
bool send_frame(int fd, const Frame& frame);
// Waits up to timeout_ms (-1 = forever) for the next frame; nullopt on EOF, error or timeout.
std::optional<Frame> recv_frame(int fd, FrameDecoder& decoder, int timeout_ms);

}  // namespace pmux
