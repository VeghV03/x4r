#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

// The parts of the x4 USB protocol (docs/PROTOCOL.md) that need no hardware:
// line framing, request parsing, argument checks, CRC-32 and the upload state
// machine. UsbLink feeds them from the serial port; host tests feed them bytes.
namespace usb {

constexpr size_t kMaxLine = 512;  // longest request line; longer lines get too_large
constexpr size_t kChunk = 4096;   // upload chunk; one ack per chunk
constexpr uint32_t kStallMs = 5000;
constexpr size_t kMaxPath = 255;

enum class Error : uint8_t { None, BadRequest, NotFound, Exists, Io, NoSpace, Crc, Busy, TooLarge, UnknownCommand };
const char* errorCode(Error e);

struct Status {
  Error code = Error::None;
  const char* message = "";
  bool ok() const { return code == Error::None; }
};
inline Status fail(Error code, const char* message) { return {code, message}; }

// Standard CRC-32 (IEEE, zlib's crc32). Start with 0 and feed the previous result back in.
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len);

// Assembles one request line from single bytes, without allocating.
class LineReader {
 public:
  enum class Result { Partial, Line, Overflow };
  // Line: line() holds the text without its "\n" or "\r\n". Overflow: the line
  // ended past kMaxLine; line() holds its first kMaxLine bytes.
  Result push(char c);
  const char* line() const { return buf; }

 private:
  char buf[kMaxLine + 1] = {};
  size_t len = 0;
  bool overflowed = false;
};

// "X4 <id> <command> <json-object>"; a missing object reads as {}.
struct Request {
  uint32_t id = 0;
  char command[12] = {};
  const char* args = "{}";
};
enum class Parse { Ok, NotForUs, Bad };
// NotForUs: the line does not start with "X4 " (the host's own echo, noise).
// Bad: req.id holds the id when one could be read, else 0.
Parse parseRequest(const char* line, Request& req);

// An absolute path of at most kMaxPath bytes, with no empty, "." or ".."
// component, backslash or control character. "/" alone is the root.
bool validPath(const char* path);

// FAT matches names without regard to ASCII case. For valid paths:
// samePathIgnoringCase is true when both name the same entry by spelling;
// isInsideIgnoringCase when `path` lies below `folder` (not at it).
bool samePathIgnoringCase(const char* a, const char* b);
bool isInsideIgnoringCase(const char* path, const char* folder);

// ArduinoJson allocator over a caller-owned buffer, so one request's documents
// never touch the heap. reset() reclaims everything once those documents are gone.
class ArenaAllocator : public ArduinoJson::Allocator {
 public:
  ArenaAllocator(uint8_t* buffer, size_t size) : base(buffer), capacity(size) {}
  void* allocate(size_t size) override;
  void deallocate(void* pointer) override;
  void* reallocate(void* pointer, size_t newSize) override;
  void reset() { used = last = 0; }

 private:
  uint8_t* base;
  size_t capacity;
  size_t used = 0;
  size_t last = 0;  // offset of the newest block, the only one that can grow in place
};

// Argument checks, one per command that takes arguments.
struct PathArgs {
  const char* path = nullptr;
};
struct TimeArgs {
  bool present = false;
  int64_t time = 0;
  bool tzPresent = false;  // without "tz" the previous offset stays
  int16_t tzMinutes = 0;
};
// The host's wall clock as the device last heard it (hello or time).
struct HostClock {
  bool set = false;    // the host set the clock since power-on
  bool tzSet = false;  // and sent an offset at least once
  int16_t tzMinutes = 0;
  // A time without "tz" keeps the offset sent before.
  void apply(const TimeArgs& t);
};
struct GetArgs {
  const char* path = nullptr;
  uint64_t offset = 0;
  bool toEnd = true;
  uint64_t length = 0;
};
struct PutArgs {
  const char* path = nullptr;
  uint64_t size = 0;
  uint32_t crc32 = 0;
};
struct RmArgs {
  const char* path = nullptr;
  bool recursive = false;
};
struct MvArgs {
  const char* from = nullptr;
  const char* to = nullptr;
};
struct PressArgs {
  uint8_t button = 0;  // HalGPIO::BTN_* index
  bool longPress = false;
};
struct FlashArgs {
  uint32_t size = 0;
  uint8_t sha256[32] = {};
};

Status parsePathArgs(JsonObjectConst args, PathArgs& out);
// `required` is false for hello, where the clock is optional.
Status parseTimeArgs(JsonObjectConst args, bool required, TimeArgs& out);
Status parseGetArgs(JsonObjectConst args, GetArgs& out);
Status parsePutArgs(JsonObjectConst args, PutArgs& out);
Status parseRmArgs(JsonObjectConst args, RmArgs& out);
Status parseMvArgs(JsonObjectConst args, MvArgs& out);
Status parsePressArgs(JsonObjectConst args, PressArgs& out);
Status parseFlashArgs(JsonObjectConst args, FlashArgs& out);

// Reply lines, each ending in "\n". Return the length written (truncated to fit).
size_t formatReply(char* buf, size_t cap, uint32_t id, const char* kind, const char* json);
size_t formatError(char* buf, size_t cap, uint32_t id, Status status);

// Where reply lines go: the serial port on the device, a string in tests.
class ReplySink {
 public:
  virtual void send(const char* line, size_t len) = 0;

 protected:
  ~ReplySink() = default;
};

// Where upload bytes go: a .part file for put, the inactive app slot for flash.
class UploadTarget {
 public:
  virtual bool write(const uint8_t* data, size_t len) = 0;
  // All bytes arrived and passed the size and CRC checks: make them permanent.
  // On success writes the ok reply's JSON object into `okJson`; on failure
  // removes what was written.
  virtual Status commit(char* okJson, size_t cap) = 0;
  // Drop what was written. Not called after commit().
  virtual void abort() = 0;

 protected:
  ~UploadTarget() = default;
};

// One put or flash upload: ready, then one ack per chunk, then ok or err.
// The device acks when the byte count reaches a multiple of kChunk or the
// announced size, so every chunk but the last must be exactly kChunk bytes.
// After a write error the rest of the chunk is drained and the err takes the
// ack's place.
class Upload {
 public:
  // checkCrc is false for flash, whose target verifies a SHA-256 instead.
  void begin(uint32_t id, uint64_t size, bool checkCrc, uint32_t expectedCrc, UploadTarget& target, ReplySink& out,
             uint32_t nowMs);
  bool active() const { return target != nullptr; }
  // Bytes still to come; never read more than this from the port.
  uint64_t remaining() const { return size - received; }
  void feed(const uint8_t* data, size_t len, uint32_t nowMs);
  // Aborts with err io after kStallMs without a byte.
  void tick(uint32_t nowMs);
  // The session went away: drop the upload without a reply.
  void cancel();
  // True once, after an upload whose commit succeeded.
  bool takeCommitted();

 private:
  void finish();
  void end(Status status);

  UploadTarget* target = nullptr;
  ReplySink* out = nullptr;
  uint32_t id = 0;
  uint64_t size = 0;
  uint64_t received = 0;
  uint32_t crc = 0;
  uint32_t expectedCrc = 0;
  bool checkCrc = false;
  bool writeFailed = false;
  bool committed = false;
  uint32_t lastByteMs = 0;
};

}  // namespace usb
