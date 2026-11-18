#include "UsbProtocol.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace usb {

const char* errorCode(Error e) {
  switch (e) {
    case Error::None:
      return "ok";
    case Error::BadRequest:
      return "bad_request";
    case Error::NotFound:
      return "not_found";
    case Error::Exists:
      return "exists";
    case Error::Io:
      return "io";
    case Error::NoSpace:
      return "no_space";
    case Error::Crc:
      return "crc";
    case Error::Busy:
      return "busy";
    case Error::TooLarge:
      return "too_large";
    case Error::UnknownCommand:
      return "unknown_command";
  }
  return "io";
}

namespace {

struct CrcTable {
  uint32_t entries[256];
  constexpr CrcTable() : entries() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      entries[i] = c;
    }
  }
};
constexpr CrcTable kCrcTable;

}  // namespace

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = kCrcTable.entries[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

LineReader::Result LineReader::push(char c) {
  if (c == '\n') {
    if (len > 0 && buf[len - 1] == '\r') --len;
    buf[len] = '\0';
    len = 0;
    const bool tooLong = overflowed;
    overflowed = false;
    return tooLong ? Result::Overflow : Result::Line;
  }
  if (len < kMaxLine) {
    buf[len++] = c;
  } else {
    overflowed = true;
  }
  return Result::Partial;
}

Parse parseRequest(const char* line, Request& req) {
  req = Request{};
  if (strncmp(line, "X4 ", 3) != 0) return Parse::NotForUs;
  const char* p = line + 3;
  if (*p < '0' || *p > '9') return Parse::Bad;
  uint64_t id = 0;
  while (*p >= '0' && *p <= '9') {
    id = id * 10 + static_cast<uint64_t>(*p++ - '0');
    if (id > UINT32_MAX) return Parse::Bad;
  }
  req.id = static_cast<uint32_t>(id);
  if (*p++ != ' ') return Parse::Bad;
  size_t n = 0;
  while ((*p >= 'a' && *p <= 'z') || *p == '_') {
    if (n + 1 >= sizeof(req.command)) return Parse::Bad;
    req.command[n++] = *p++;
  }
  if (n == 0 || (*p != '\0' && *p != ' ')) return Parse::Bad;
  while (*p == ' ') ++p;
  if (*p != '\0') req.args = p;
  return Parse::Ok;
}

bool validPath(const char* path) {
  if (!path || path[0] != '/') return false;
  const size_t len = strlen(path);
  if (len > kMaxPath) return false;
  if (len == 1) return true;  // the root
  const char* component = path + 1;
  for (const char* p = path + 1;; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c == '/' || c == '\0') {
      const size_t n = static_cast<size_t>(p - component);
      if (n == 0) return false;  // "//" or a trailing "/"
      if ((n == 1 && component[0] == '.') || (n == 2 && component[0] == '.' && component[1] == '.')) return false;
      if (c == '\0') return true;
      component = p + 1;
    } else if (c < 0x20 || c == 0x7F || c == '\\') {
      return false;
    }
  }
}

namespace {

char asciiLower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Length of the common prefix of a and b, ignoring ASCII case.
size_t commonPrefixIgnoringCase(const char* a, const char* b) {
  size_t n = 0;
  while (a[n] != '\0' && asciiLower(a[n]) == asciiLower(b[n])) ++n;
  return n;
}

}  // namespace

bool samePathIgnoringCase(const char* a, const char* b) {
  const size_t n = commonPrefixIgnoringCase(a, b);
  return a[n] == '\0' && b[n] == '\0';
}

bool isInsideIgnoringCase(const char* path, const char* folder) {
  const size_t folderLen = strlen(folder);
  if (commonPrefixIgnoringCase(folder, path) != folderLen) return false;
  if (folderLen == 1) return path[1] != '\0';  // everything but the root is inside it
  return path[folderLen] == '/';
}

namespace {

// Each block carries its size in front, so a block that is not the newest can
// still be copied when it grows.
constexpr size_t kAlign = 8;
constexpr size_t kHeader = kAlign;

size_t roundUp(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }

size_t& blockSize(void* pointer) { return *reinterpret_cast<size_t*>(static_cast<uint8_t*>(pointer) - kHeader); }

}  // namespace

void* ArenaAllocator::allocate(size_t size) {
  const size_t start = roundUp(used);
  if (start + kHeader + size > capacity) return nullptr;
  last = start;
  used = start + kHeader + size;
  void* pointer = base + start + kHeader;
  blockSize(pointer) = size;
  return pointer;
}

void ArenaAllocator::deallocate(void*) {}

void* ArenaAllocator::reallocate(void* pointer, size_t newSize) {
  if (!pointer) return allocate(newSize);
  const size_t offset = static_cast<size_t>(static_cast<uint8_t*>(pointer) - base) - kHeader;
  if (offset == last) {
    if (offset + kHeader + newSize > capacity) return nullptr;
    used = offset + kHeader + newSize;
    blockSize(pointer) = newSize;
    return pointer;
  }
  const size_t oldSize = blockSize(pointer);
  if (newSize <= oldSize) return pointer;  // shrinkToFit: keep the block; reset() reclaims it
  void* moved = allocate(newSize);
  if (moved) memcpy(moved, pointer, oldSize < newSize ? oldSize : newSize);
  return moved;
}

namespace {

constexpr int64_t kEarliestTime = 1735689600;  // 2025-01-01
constexpr int64_t kLatestTime = 4102444800;    // 2100-01-01

Status pathField(JsonObjectConst args, const char* key, const char*& out) {
  JsonVariantConst v = args[key];
  if (!v.is<const char*>()) return fail(Error::BadRequest, "path missing");
  out = v.as<const char*>();
  if (!validPath(out)) return fail(Error::BadRequest, "path must be absolute, without . or ..");
  return {};
}

Status optionalBool(JsonObjectConst args, const char* key, bool& out) {
  JsonVariantConst v = args[key];
  if (v.isNull()) return {};
  if (!v.is<bool>()) return fail(Error::BadRequest, "flag must be true or false");
  out = v.as<bool>();
  return {};
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

Status parsePathArgs(JsonObjectConst args, PathArgs& out) { return pathField(args, "path", out.path); }

Status parseTimeArgs(JsonObjectConst args, bool required, TimeArgs& out) {
  out = TimeArgs{};
  JsonVariantConst time = args["time"];
  if (time.isNull()) return required ? fail(Error::BadRequest, "time missing") : Status{};
  if (!time.is<int64_t>()) return fail(Error::BadRequest, "time must be Unix seconds");
  out.time = time.as<int64_t>();
  if (out.time < kEarliestTime || out.time >= kLatestTime) return fail(Error::BadRequest, "time out of range");
  JsonVariantConst tz = args["tz"];
  if (!tz.isNull()) {
    if (!tz.is<int>() || tz.as<int>() < -1440 || tz.as<int>() > 1440) {
      return fail(Error::BadRequest, "tz must be minutes east of UTC");
    }
    out.tzMinutes = static_cast<int16_t>(tz.as<int>());
    out.tzPresent = true;
  }
  out.present = true;
  return {};
}

void HostClock::apply(const TimeArgs& t) {
  if (!t.present) return;
  set = true;
  if (!t.tzPresent) return;
  tzSet = true;
  tzMinutes = t.tzMinutes;
}

Status parseGetArgs(JsonObjectConst args, GetArgs& out) {
  out = GetArgs{};
  Status s = pathField(args, "path", out.path);
  if (!s.ok()) return s;
  JsonVariantConst offset = args["offset"];
  if (!offset.isNull()) {
    if (!offset.is<uint64_t>()) return fail(Error::BadRequest, "offset must be a byte count");
    out.offset = offset.as<uint64_t>();
  }
  JsonVariantConst length = args["length"];
  if (!length.isNull()) {
    if (!length.is<uint64_t>()) return fail(Error::BadRequest, "length must be a byte count or null");
    out.toEnd = false;
    out.length = length.as<uint64_t>();
  }
  return {};
}

Status parsePutArgs(JsonObjectConst args, PutArgs& out) {
  out = PutArgs{};
  Status s = pathField(args, "path", out.path);
  if (!s.ok()) return s;
  if (strlen(out.path) + 5 > kMaxPath) return fail(Error::BadRequest, "path too long");  // room for ".part"
  if (!args["size"].is<uint64_t>()) return fail(Error::BadRequest, "size must be a byte count");
  if (!args["crc32"].is<uint32_t>()) return fail(Error::BadRequest, "crc32 must be an unsigned 32-bit number");
  out.size = args["size"].as<uint64_t>();
  out.crc32 = args["crc32"].as<uint32_t>();
  return {};
}

Status parseRmArgs(JsonObjectConst args, RmArgs& out) {
  out = RmArgs{};
  Status s = pathField(args, "path", out.path);
  if (!s.ok()) return s;
  if (strcmp(out.path, "/") == 0) return fail(Error::BadRequest, "cannot remove the root");
  return optionalBool(args, "recursive", out.recursive);
}

Status parseMvArgs(JsonObjectConst args, MvArgs& out) {
  out = MvArgs{};
  Status s = pathField(args, "from", out.from);
  if (!s.ok()) return s;
  s = pathField(args, "to", out.to);
  if (!s.ok()) return s;
  if (strcmp(out.from, "/") == 0 || strcmp(out.to, "/") == 0) return fail(Error::BadRequest, "cannot move the root");
  return {};
}

Status parsePressArgs(JsonObjectConst args, PressArgs& out) {
  out = PressArgs{};
  // Same order as the HalGPIO::BTN_* indices.
  static constexpr const char* kButtons[] = {"back", "confirm", "left", "right", "up", "down", "power"};
  JsonVariantConst button = args["button"];
  if (!button.is<const char*>()) return fail(Error::BadRequest, "button missing");
  const char* name = button.as<const char*>();
  bool found = false;
  for (uint8_t i = 0; i < sizeof(kButtons) / sizeof(kButtons[0]); ++i) {
    if (strcmp(name, kButtons[i]) == 0) {
      out.button = i;
      found = true;
    }
  }
  if (!found) return fail(Error::BadRequest, "unknown button");
  return optionalBool(args, "long", out.longPress);
}

Status parseFlashArgs(JsonObjectConst args, FlashArgs& out) {
  out = FlashArgs{};
  if (!args["size"].is<uint32_t>() || args["size"].as<uint32_t>() == 0) {
    return fail(Error::BadRequest, "size must be a positive byte count");
  }
  out.size = args["size"].as<uint32_t>();
  JsonVariantConst sha = args["sha256"];
  const char* hex = sha.is<const char*>() ? sha.as<const char*>() : "";
  if (strlen(hex) != 64) return fail(Error::BadRequest, "sha256 must be 64 hex digits");
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexDigit(hex[2 * i]);
    const int lo = hexDigit(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return fail(Error::BadRequest, "sha256 must be 64 hex digits");
    out.sha256[i] = static_cast<uint8_t>(hi << 4 | lo);
  }
  return {};
}

size_t formatReply(char* buf, size_t cap, uint32_t id, const char* kind, const char* json) {
  const int n = snprintf(buf, cap, "X4 %" PRIu32 " %s %s\n", id, kind, json);
  if (n < 0) return 0;
  if (static_cast<size_t>(n) < cap) return static_cast<size_t>(n);
  buf[cap - 2] = '\n';  // truncated: still end the line
  return cap - 1;
}

// Messages are fixed strings without quotes or backslashes, so they need no escaping.
size_t formatError(char* buf, size_t cap, uint32_t id, Status status) {
  char json[160];
  snprintf(json, sizeof(json), "{\"code\":\"%s\",\"message\":\"%s\"}", errorCode(status.code), status.message);
  return formatReply(buf, cap, id, "err", json);
}

void Upload::begin(uint32_t requestId, uint64_t totalSize, bool verifyCrc, uint32_t crcExpected,
                   UploadTarget& uploadTarget, ReplySink& sink, uint32_t nowMs) {
  target = &uploadTarget;
  out = &sink;
  id = requestId;
  size = totalSize;
  received = 0;
  crc = 0;
  expectedCrc = crcExpected;
  checkCrc = verifyCrc;
  writeFailed = false;
  committed = false;
  lastByteMs = nowMs;
  char line[64];
  out->send(line, formatReply(line, sizeof(line), id, "ready", "{\"chunk\":4096}"));
  if (size == 0) finish();
}

void Upload::feed(const uint8_t* data, size_t len, uint32_t nowMs) {
  if (!active() || len == 0) return;
  if (len > remaining()) len = static_cast<size_t>(remaining());
  lastByteMs = nowMs;
  while (len > 0 && active()) {
    const size_t toBoundary = kChunk - static_cast<size_t>(received % kChunk);
    const size_t piece = len < toBoundary ? len : toBoundary;
    if (!writeFailed) {
      crc = crc32(crc, data, piece);
      writeFailed = !target->write(data, piece);
    }
    received += piece;
    data += piece;
    len -= piece;
    if (received % kChunk != 0 && received != size) continue;
    if (writeFailed) {
      end(fail(Error::Io, "write failed"));
      return;
    }
    char line[64];
    char json[32];
    snprintf(json, sizeof(json), "{\"n\":%" PRIu64 "}", received);
    out->send(line, formatReply(line, sizeof(line), id, "ack", json));
    if (received == size) finish();
  }
}

void Upload::tick(uint32_t nowMs) {
  if (active() && nowMs - lastByteMs >= kStallMs) end(fail(Error::Io, "upload stalled"));
}

void Upload::cancel() {
  if (!active()) return;
  target->abort();
  target = nullptr;
}

bool Upload::takeCommitted() {
  const bool was = committed;
  committed = false;
  return was;
}

void Upload::finish() {
  if (checkCrc && crc != expectedCrc) {
    end(fail(Error::Crc, "crc mismatch"));
    return;
  }
  char json[96] = "{}";
  const Status status = target->commit(json, sizeof(json));
  target = nullptr;  // commit cleans up after itself on failure
  char line[160];
  if (status.ok()) {
    committed = true;
    out->send(line, formatReply(line, sizeof(line), id, "ok", json));
  } else {
    out->send(line, formatError(line, sizeof(line), id, status));
  }
}

void Upload::end(Status status) {
  target->abort();
  target = nullptr;
  char line[160];
  out->send(line, formatError(line, sizeof(line), id, status));
}

}  // namespace usb
