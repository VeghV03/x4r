#include "UsbLink.h"

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <esp_attr.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <sys/time.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <optional>

#include "SdCardFontSystem.h"
#include "UsbHooks.h"
#include "UsbProtocol.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "network/OtaBootSwitch.h"
#include "util/HeapLows.h"
#include "util/PressLag.h"

UsbLink usbLink;

extern GfxRenderer renderer;  // main.cpp

namespace {

// The RX interrupt drops bytes once its queue is full, and the host sends a
// whole chunk before it waits for the ack: hold one chunk plus a request line.
constexpr size_t kRxQueueBytes = usb::kChunk + usb::kMaxLine;
constexpr uint32_t kSessionIdleMs = 60000;
constexpr uint32_t kTxStallMs = 2000;       // host stopped reading: give the session up
constexpr size_t kBytesPerPoll = 8192;      // bounds one poll() so the UI keeps running
constexpr uint32_t kShortPressMs = 80;      // past the 5 ms debounce, well below every hold threshold
constexpr uint32_t kLongPressMs = 1200;     // past the 1 s go-home and list long-press holds
constexpr uint32_t kPressSettleMs = 15000;  // reply anyway if the screen never settles
constexpr uint32_t kRebootDelayMs = 100;    // lets the last reply leave the port
constexpr uint32_t kRangeGapMs = 50;        // a host reading a file range by range asks again within this

// One transfer buffer serves uploads, downloads and ls lines; one arena holds
// a request's JSON documents (ArduinoJson takes 1 KB per slot pool: one for
// the arguments, one for the hello reply). Static so a session never allocates.
uint8_t io[1024];
uint8_t jsonArena[4096];
usb::ArenaAllocator arena(jsonArena, sizeof(jsonArena));
usb::LineReader lineReader;
char reply[usb::kMaxLine];

RTC_DATA_ATTR usb::HostClock hostClock;

bool linkBroken = false;

// HWCDC returns short writes under host backpressure; retry until the host
// takes everything or stops reading altogether.
bool writeAll(const void* data, size_t len) {
  if (linkBroken) return false;
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t lastProgress = millis();
  while (len > 0) {
    const size_t n = logSerial.write(bytes, len);
    if (n > 0) {
      bytes += n;
      len -= n;
      lastProgress = millis();
    } else if (millis() - lastProgress > kTxStallMs) {
      linkBroken = true;
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

bool writeText(const char* text) { return writeAll(text, strlen(text)); }

struct SerialSink final : usb::ReplySink {
  void send(const char* line, size_t len) override { writeAll(line, len); }
};
SerialSink sink;

void sendOk(uint32_t id, const char* json = "{}") {
  sink.send(reply, usb::formatReply(reply, sizeof(reply), id, "ok", json));
}

void sendError(uint32_t id, usb::Status status) {
  sink.send(reply, usb::formatError(reply, sizeof(reply), id, status));
}

bool isDirectory(const char* path) {
  HalFile f = Storage.open(path);
  return f && f.isDirectory();
}

// The folder that holds `path` must exist before anything is created in it.
bool parentExists(const char* path) {
  char parent[usb::kMaxPath + 1];
  strncpy(parent, path, sizeof(parent) - 1);
  parent[sizeof(parent) - 1] = '\0';
  char* slash = strrchr(parent, '/');
  if (slash == parent) return true;  // in the root
  *slash = '\0';
  return isDirectory(parent);
}

uint64_t sdFreeBytes() {
  const uint64_t total = Storage.totalBytes();
  const uint64_t used = Storage.usedBytes();
  return total > used ? total - used : 0;
}

// put: bytes land in <path>.part, which replaces <path> only once complete.
class FileTarget final : public usb::UploadTarget {
 public:
  bool open(const char* path) {
    snprintf(finalPath, sizeof(finalPath), "%s", path);
    snprintf(partPath, sizeof(partPath), "%s.part", path);
    file = Storage.open(partPath, O_RDWR | O_CREAT | O_TRUNC);
    return static_cast<bool>(file);
  }
  bool write(const uint8_t* data, size_t len) override { return file.write(data, len) == len; }
  usb::Status commit(char*, size_t) override {
    // close() syncs: a failure means the bytes may not all be on the card.
    if (!file.close()) {
      Storage.remove(partPath);
      return usb::fail(usb::Error::Io, "write failed");
    }
    if (!Storage.replaceFile(partPath, finalPath)) {
      Storage.remove(partPath);
      return usb::fail(usb::Error::Io, "rename failed");
    }
    return {};
  }
  void abort() override {
    file.close();
    Storage.remove(partPath);
  }
  const char* path() const { return finalPath; }

 private:
  HalFile file;
  char finalPath[usb::kMaxPath + 1] = {};
  char partPath[usb::kMaxPath + 1] = {};
};

// flash: the image goes raw into the inactive app slot, then otadata points the
// bootloader at it. Same scheme as the SD update (OtaBootSwitch.h explains why
// esp_ota_end's image check is bypassed); the host's SHA-256 guards the bytes.
class FlashTarget final : public usb::UploadTarget {
 public:
  usb::Status open(const usb::FlashArgs& args) {
    partition = esp_ota_get_next_update_partition(nullptr);
    if (!partition) return usb::fail(usb::Error::Io, "no update slot");
    if (args.size > partition->size) return usb::fail(usb::Error::TooLarge, "image larger than the slot");
    memcpy(expected, args.sha256, sizeof(expected));
    written = 0;
    erasedTo = 0;
    return {};
  }
  bool write(const uint8_t* data, size_t len) override {
    // 64 KiB block erases ahead of the writes are much faster than sector erases.
    while (written + len > erasedTo) {
      const size_t block = std::min<size_t>(64 * 1024, partition->size - erasedTo);
      if (esp_partition_erase_range(partition, erasedTo, block) != ESP_OK) return false;
      erasedTo += block;
    }
    if (esp_partition_write(partition, written, data, len) != ESP_OK) return false;
    written += len;
    return true;
  }
  usb::Status commit(char* okJson, size_t cap) override {
    // Hash what is in flash, not what arrived: this checks the writes too, and keeps the
    // SHA engine out of the erase/write sequence (interleaving them gave wrong digests).
    uint8_t digest[32];
    if (!hashSlot(digest)) return usb::fail(usb::Error::Io, "could not read the slot back");
    if (memcmp(digest, expected, sizeof(digest)) != 0) return usb::fail(usb::Error::Crc, "sha256 mismatch");
    uint8_t magic = 0;
    if (esp_partition_read(partition, 0, &magic, 1) != ESP_OK || magic != 0xE9) {
      return usb::fail(usb::Error::BadRequest, "not an ESP32 app image");
    }
    if (!ota_boot::switchTo(partition)) return usb::fail(usb::Error::Io, "could not switch the boot slot");
    snprintf(okJson, cap, "{\"slot\":\"ota_%d\"}", partition->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0);
    return {};
  }
  // The slot only boots once otadata names it, so a partial image is harmless.
  void abort() override {}

 private:
  bool hashSlot(uint8_t (&digest)[32]) const {
    uint8_t buf[1024];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, /*is224=*/0);
    bool ok = true;
    for (size_t at = 0; ok && at < written; at += sizeof(buf)) {
      const size_t n = std::min(sizeof(buf), written - at);
      ok = esp_partition_read(partition, at, buf, n) == ESP_OK;
      if (ok) mbedtls_sha256_update(&sha, buf, n);
    }
    if (ok) mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    return ok;
  }

  const esp_partition_t* partition = nullptr;
  size_t written = 0;
  size_t erasedTo = 0;
  uint8_t expected[32] = {};
};

enum class Mode : uint8_t { Idle, Upload, Download, Press, Reboot };

struct Download {
  // The data line carries the CRC, so the range is read twice: once to sum, once to send.
  enum class Phase : uint8_t { Sum, Send };
  HalFile file;
  Phase phase = Phase::Sum;
  uint32_t id = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t total = 0;  // the file's size
  uint64_t done = 0;
  uint32_t crc = 0;
  bool readFailed = false;
};

struct Press {
  uint32_t id = 0;
  uint8_t button = 0;
  bool released = false;
  uint8_t pollsSinceRelease = 0;
  uint32_t releaseAt = 0;
  uint32_t releasedAt = 0;
};

struct State {
  bool open = false;
  bool dataChanged = false;
  uint32_t lastActivityMs = 0;
  // Keeps the CPU at full speed for the whole session.
  std::optional<HalPowerManager::Lock> powerLock;
  Mode mode = Mode::Idle;
  usb::Upload upload;
  bool uploadIsFlash = false;
  FileTarget fileTarget;
  FlashTarget flashTarget;
  Download download;
  Press press;
  uint32_t rebootAt = 0;
  uint32_t rangeGapEnd = 0;  // millis() until which the next range of a get is expected
};
State state;

void openSession() {
  if (state.open) return;
  state.open = true;
  state.dataChanged = false;
  linkBroken = false;
  state.powerLock.emplace();
  setLogSerialSuppressed(true);
}

void endSession() {
  if (!state.open) return;
  state.upload.cancel();
  if (state.download.file) state.download.file.close();  // close() asserts on a never-opened file
  HalGPIO::setInjectedButtons(0);
  state.mode = Mode::Idle;
  state.open = false;
  state.powerLock.reset();
  setLogSerialSuppressed(false);
  LOG_INF("USB", "Session closed");
  x4::heap::mark("usb");  // whatever the session did: transfers, a flash, presses
  if (state.dataChanged) {
    state.dataChanged = false;
    x4OnHostDataChanged();
  }
}

void applyTime(const usb::TimeArgs& t) {
  const timeval tv{static_cast<time_t>(t.time), 0};
  settimeofday(&tv, nullptr);
  trustedtime::note();
  hostClock.apply(t);
  x4OnHostTime(t.time, hostClock.tzMinutes, hostClock.tzSet);
}

// Every put, rm and mv that changed something; also ends in x4OnHostDataChanged at session end.
void noteWrite(const char* path) {
  state.dataChanged = true;
  x4OnHostWrote(path);
}

// Before each put, rm and mv: an activity with a file open at `path` closes it first.
void releaseForWrite(const char* path) {
  activityManager.beforeHostWrite(path);
}

void handleHello(uint32_t id, JsonObjectConst args) {
  usb::TimeArgs t;
  const usb::Status s = usb::parseTimeArgs(args, false, t);
  if (!s.ok()) return sendError(id, s);
  if (t.present) applyTime(t);
  activityManager.noteHostHello();

  static const BatteryMonitor battery;
  JsonDocument doc(&arena);
  JsonObject out = doc.to<JsonObject>();
  out["proto"] = 2;
  out["fw"] = CROSSPOINT_VERSION;
  out["board"] = "x4";
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running) {
    char slot[8];
    snprintf(slot, sizeof(slot), "ota_%d", running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0);
    out["slot"] = slot;
  }
  uint8_t mac[6];
  if (esp_efuse_mac_get_default(mac) == ESP_OK) {
    char text[18];
    snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    out["mac"] = text;
  }
  out["battery"] = powerManager.getBatteryPercentage();
  out["battery_mv"] = powerManager.getBatteryMillivolts();
  out["charging"] = battery.isCharging();
  out["sd_free"] = sdFreeBytes();
  out["sd_total"] = Storage.totalBytes();
  const HalMemory::HeapStats heap = HalMemory::getInternalHeap();
  out["heap_free"] = heap.freeBytes;
  out["heap_min"] = heap.minFreeBytes;
  out["heap_block"] = heap.largestBlockBytes;
  x4HelloFields(out);
  int n = snprintf(reply, sizeof(reply), "X4 %" PRIu32 " ok ", id);
  n += static_cast<int>(serializeJson(doc, reply + n, sizeof(reply) - n - 1));
  reply[n++] = '\n';
  writeAll(reply, static_cast<size_t>(n));
}

void handleNet(uint32_t id, JsonObjectConst args) {
  JsonDocument doc(&arena);
  JsonObject out = doc.to<JsonObject>();
  if (const char* error = x4NetCommand(args, out)) return sendError(id, usb::fail(usb::Error::BadRequest, error));
  int n = snprintf(reply, sizeof(reply), "X4 %" PRIu32 " ok ", id);
  n += static_cast<int>(serializeJson(doc, reply + n, sizeof(reply) - n - 1));
  reply[n++] = '\n';
  writeAll(reply, static_cast<size_t>(n));
}

void handleTime(uint32_t id, JsonObjectConst args) {
  usb::TimeArgs t;
  const usb::Status s = usb::parseTimeArgs(args, true, t);
  if (!s.ok()) return sendError(id, s);
  applyTime(t);
  sendOk(id);
}

// FAT forbids quotes, backslashes and control characters in names, so this
// guards against a corrupt entry rather than a real one.
size_t jsonEscape(const char* in, char* out, size_t cap) {
  size_t n = 0;
  for (; *in && n + 7 < cap; ++in) {
    const unsigned char c = static_cast<unsigned char>(*in);
    if (c == '"' || c == '\\') {
      out[n++] = '\\';
      out[n++] = static_cast<char>(c);
    } else if (c < 0x20) {
      n += static_cast<size_t>(snprintf(out + n, cap - n, "\\u%04x", c));
    } else {
      out[n++] = static_cast<char>(c);
    }
  }
  out[n] = '\0';
  return n;
}

// The reply streams entry by entry, so a large folder needs no buffer.
void handleLs(uint32_t id, JsonObjectConst args) {
  usb::PathArgs a;
  const usb::Status s = usb::parsePathArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  HalFile dir = Storage.open(a.path);
  if (!dir) return sendError(id, usb::fail(usb::Error::NotFound, "no such path"));
  if (!dir.isDirectory()) return sendError(id, usb::fail(usb::Error::BadRequest, "not a folder"));
  snprintf(reply, sizeof(reply), "X4 %" PRIu32 " ok {\"entries\":[", id);
  writeText(reply);
  char name[usb::kMaxPath + 1];
  bool first = true;
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    entry.getName(name, sizeof(name));
    char* line = reinterpret_cast<char*>(io);
    size_t n = first ? 0 : 1;
    if (!first) line[0] = ',';
    first = false;
    n += snprintf(line + n, sizeof(io) - n, "{\"name\":\"");
    n += jsonEscape(name, line + n, sizeof(io) - n - 64);
    const bool isDir = entry.isDirectory();
    n += snprintf(line + n, sizeof(io) - n, "\",\"dir\":%s,\"size\":%" PRIu64 "}", isDir ? "true" : "false",
                  isDir ? uint64_t{0} : entry.fileSize64());
    if (!writeAll(line, n)) return;
  }
  writeText("]}\n");
}

// The last presses, oldest first: activity, idle before, release, panel start and end, the end of
// the last update of the chain (ms from the press), refresh mode and whether the CPU was idle-clocked.
void handleLag(uint32_t id) {
  presslag::Entry entries[presslag::kCount];
  const size_t n = presslag::snapshot(entries, presslag::kCount);
  snprintf(reply, sizeof(reply), "X4 %" PRIu32 " ok {\"presses\":[", id);
  writeText(reply);
  static constexpr const char* kModes[] = {"full", "half", "fast", "gray"};
  for (size_t i = 0; i < n; ++i) {
    const presslag::Entry& e = entries[i];
    char name[sizeof(e.activity) * 6 + 1];
    jsonEscape(e.activity, name, sizeof(name));
    const int len = snprintf(reply, sizeof(reply),
                             "%s{\"activity\":\"%s\",\"idle\":%" PRIu32
                             ",\"up\":%u,\"panel\":%u,\"done\":%u,\"last\":%u,\"updates\":%u,\"mode\":\"%s\",\"low_clock\":%s}",
                             i ? "," : "", name, e.idleMs, e.upMs, e.panelMs, e.doneMs, e.lastMs, e.updates,
                             e.mode < 4 ? kModes[e.mode] : "?", e.lowClock ? "true" : "false");
    if (!writeAll(reply, static_cast<size_t>(len))) return;
  }
  presslag::Mark marks[presslag::kMarks];
  const size_t m = presslag::marks(marks, presslag::kMarks);
  writeText("],\"marks\":[");
  for (size_t i = 0; i < m; ++i) {
    char tag[sizeof(marks[i].tag) * 6 + 1];
    jsonEscape(marks[i].tag, tag, sizeof(tag));
    const int len = snprintf(reply, sizeof(reply), "%s{\"tag\":\"%s\",\"ms\":%u}", i ? "," : "", tag, marks[i].ms);
    if (!writeAll(reply, static_cast<size_t>(len))) return;
  }
  writeText("]}\n");
}

void handleStat(uint32_t id, JsonObjectConst args) {
  usb::PathArgs a;
  const usb::Status s = usb::parsePathArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  HalFile f = Storage.open(a.path);
  if (!f) return sendError(id, usb::fail(usb::Error::NotFound, "no such path"));
  char json[64];
  if (f.isDirectory()) {
    snprintf(json, sizeof(json), "{\"dir\":true,\"size\":0}");
  } else {
    snprintf(json, sizeof(json), "{\"dir\":false,\"size\":%" PRIu64 "}", f.fileSize64());
  }
  sendOk(id, json);
}

void handleGet(uint32_t id, JsonObjectConst args) {
  usb::GetArgs a;
  const usb::Status s = usb::parseGetArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  x4BeforeHostRead();
  Download& d = state.download;
  d = Download{};
  d.file = Storage.open(a.path);
  if (!d.file) return sendError(id, usb::fail(usb::Error::NotFound, "no such file"));
  if (d.file.isDirectory()) {
    d.file.close();
    return sendError(id, usb::fail(usb::Error::BadRequest, "is a folder"));
  }
  const uint64_t size = d.file.fileSize64();
  if (a.offset > size) {
    d.file.close();
    return sendError(id, usb::fail(usb::Error::BadRequest, "offset past the end"));
  }
  d.id = id;
  d.total = size;
  d.offset = a.offset;
  d.length = (a.toEnd || a.length > size - a.offset) ? size - a.offset : a.length;
  d.file.seek64(d.offset);
  state.mode = Mode::Download;
}

void pollDownload() {
  Download& d = state.download;
  size_t budget = kBytesPerPoll;
  while (budget > 0 && d.done < d.length) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(sizeof(io), d.length - d.done));
    const int got = d.readFailed ? -1 : d.file.read(io, want);
    if (got != static_cast<int>(want)) {
      if (d.phase == Download::Phase::Sum) {
        d.file.close();
        state.mode = Mode::Idle;
        return sendError(d.id, usb::fail(usb::Error::Io, "read failed"));
      }
      // The size is already announced: pad it out, then report the failure.
      d.readFailed = true;
      memset(io, 0, want);
    }
    if (d.phase == Download::Phase::Sum) {
      d.crc = usb::crc32(d.crc, io, want);
    } else if (!writeAll(io, want)) {
      return;  // host gone; the session ends on the next poll
    }
    d.done += want;
    budget -= want;
  }
  if (d.done < d.length) return;
  if (d.phase == Download::Phase::Sum) {
    char json[96];
    snprintf(json, sizeof(json), "{\"size\":%" PRIu64 ",\"crc32\":%" PRIu32 ",\"total\":%" PRIu64 "}", d.length, d.crc,
             d.total);
    sink.send(reply, usb::formatReply(reply, sizeof(reply), d.id, "data", json));
    d.phase = Download::Phase::Send;
    d.done = 0;
    d.file.seek64(d.offset);
    return;
  }
  d.file.close();
  state.mode = Mode::Idle;
  state.rangeGapEnd = millis() + kRangeGapMs;
  if (d.readFailed) return sendError(d.id, usb::fail(usb::Error::Io, "read failed"));
  sendOk(d.id);
}

void handlePut(uint32_t id, JsonObjectConst args) {
  usb::PutArgs a;
  const usb::Status s = usb::parsePutArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  if (!parentExists(a.path)) return sendError(id, usb::fail(usb::Error::NotFound, "folder does not exist"));
  if (isDirectory(a.path)) return sendError(id, usb::fail(usb::Error::BadRequest, "is a folder"));
  if (a.size > sdFreeBytes()) return sendError(id, usb::fail(usb::Error::NoSpace, "not enough space on the card"));
  releaseForWrite(a.path);
  if (!state.fileTarget.open(a.path)) return sendError(id, usb::fail(usb::Error::Io, "cannot create the file"));
  state.uploadIsFlash = false;
  state.mode = Mode::Upload;
  state.upload.begin(id, a.size, true, a.crc32, state.fileTarget, sink, millis());
}

void handleFlash(uint32_t id, JsonObjectConst args) {
  usb::FlashArgs a;
  usb::Status s = usb::parseFlashArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  // Until the running image is confirmed, the other slot holds the last good
  // one, which rollback falls back to; writing over it would leave none.
  esp_ota_img_states_t running = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &running) == ESP_OK &&
      running == ESP_OTA_IMG_PENDING_VERIFY) {
    return sendError(id, usb::fail(usb::Error::Busy, "the running image is not confirmed yet"));
  }
  s = state.flashTarget.open(a);
  if (!s.ok()) return sendError(id, s);
  state.uploadIsFlash = true;
  state.mode = Mode::Upload;
  state.upload.begin(id, a.size, false, 0, state.flashTarget, sink, millis());
}

void pollUpload() {
  const uint32_t now = millis();
  size_t budget = kBytesPerPoll;
  while (budget > 0 && state.upload.active()) {
    const int available = logSerial.available();
    if (available <= 0) break;
    const size_t want = static_cast<size_t>(
        std::min<uint64_t>(std::min<size_t>(static_cast<size_t>(available), sizeof(io)), state.upload.remaining()));
    const size_t got = logSerial.read(io, want);
    state.upload.feed(io, got, now);
    state.lastActivityMs = now;
    budget -= std::min(budget, got);
  }
  state.upload.tick(now);
  if (state.upload.active()) return;
  state.mode = Mode::Idle;
  if (!state.upload.takeCommitted()) return;
  if (state.uploadIsFlash) {
    state.mode = Mode::Reboot;
    state.rebootAt = now + kRebootDelayMs;
  } else {
    noteWrite(state.fileTarget.path());
  }
}

void handleRm(uint32_t id, JsonObjectConst args) {
  usb::RmArgs a;
  const usb::Status s = usb::parseRmArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  HalFile f = Storage.open(a.path);
  if (!f) return sendError(id, usb::fail(usb::Error::NotFound, "no such path"));
  releaseForWrite(a.path);
  bool removed;
  if (!f.isDirectory()) {
    f.close();
    removed = Storage.remove(a.path);
  } else if (a.recursive) {
    f.close();
    removed = Storage.removeDir(a.path);
  } else {
    const bool empty = !f.openNextFile();
    f.close();
    if (!empty) return sendError(id, usb::fail(usb::Error::BadRequest, "folder not empty"));
    removed = Storage.rmdir(a.path);
  }
  noteWrite(a.path);  // a failed recursive remove may still have removed some files
  if (!removed) return sendError(id, usb::fail(usb::Error::Io, "remove failed"));
  sendOk(id);
}

void handleMkdir(uint32_t id, JsonObjectConst args) {
  usb::PathArgs a;
  const usb::Status s = usb::parsePathArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  if (Storage.exists(a.path)) {
    if (isDirectory(a.path)) return sendOk(id);
    return sendError(id, usb::fail(usb::Error::Exists, "a file has that name"));
  }
  if (!Storage.mkdir(a.path, true)) return sendError(id, usb::fail(usb::Error::Io, "mkdir failed"));
  state.dataChanged = true;
  sendOk(id);
}

// mv onto an existing name. FAT matches names without case and through short
// 8.3 aliases, so `to` may be the very entry `from` names: `to` is parked under
// a free name first, and `from` vanishing with it proves they are one entry.
// Nothing is removed until the two are known to differ.
usb::Status moveOnto(const char* from, const char* to) {
  const bool folders = isDirectory(from) || isDirectory(to);
  // A folder never replaces anything; only its own name may change case.
  if (folders && !usb::samePathIgnoringCase(from, to)) return usb::fail(usb::Error::Exists, "cannot replace a folder");
  char aside[usb::kMaxPath + 1];
  if (snprintf(aside, sizeof(aside), "%s.x4mv", to) >= static_cast<int>(sizeof(aside))) {
    return usb::fail(usb::Error::BadRequest, "path too long");
  }
  if (Storage.exists(aside)) return usb::fail(usb::Error::Exists, "temporary name in use");
  if (!Storage.rename(to, aside)) return usb::fail(usb::Error::Io, "move failed");
  if (!Storage.exists(from)) {
    if (Storage.rename(aside, to)) return {};
    Storage.rename(aside, from);
    return usb::fail(usb::Error::Io, "move failed");
  }
  if (folders || !Storage.rename(from, to)) {
    Storage.rename(aside, to);
    return folders ? usb::fail(usb::Error::Exists, "cannot replace a folder")
                   : usb::fail(usb::Error::Io, "move failed");
  }
  if (!Storage.remove(aside)) LOG_ERR("USB", "Cannot remove %s", aside);
  return {};
}

void handleMv(uint32_t id, JsonObjectConst args) {
  usb::MvArgs a;
  const usb::Status s = usb::parseMvArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  if (!Storage.exists(a.from)) return sendError(id, usb::fail(usb::Error::NotFound, "no such path"));
  if (strcmp(a.from, a.to) == 0) return sendOk(id);
  if (usb::isInsideIgnoringCase(a.to, a.from)) {
    return sendError(id, usb::fail(usb::Error::BadRequest, "cannot move a folder into itself"));
  }
  if (!parentExists(a.to)) return sendError(id, usb::fail(usb::Error::NotFound, "folder does not exist"));
  releaseForWrite(a.from);
  releaseForWrite(a.to);
  usb::Status moved;
  if (!Storage.exists(a.to)) {
    if (!Storage.rename(a.from, a.to)) moved = usb::fail(usb::Error::Io, "move failed");
  } else {
    moved = moveOnto(a.from, a.to);
  }
  if (!moved.ok()) return sendError(id, moved);
  noteWrite(a.from);
  noteWrite(a.to);
  sendOk(id);
}

// The render lock keeps the render task from drawing while the frame is sent.
// Degrees clockwise that turn the panel-native frame upright for the reader.
unsigned screenshotRotation() {
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      return 90;
    case GfxRenderer::LandscapeClockwise:
      return 180;
    case GfxRenderer::PortraitInverted:
      return 270;
    case GfxRenderer::LandscapeCounterClockwise:
      return 0;
  }
  return 0;
}

void handleScreenshot(uint32_t id) {
  RenderLock lock;
  const uint8_t* frame = display.getFrameBuffer();
  const uint32_t size = display.getBufferSize();
  char json[144];
  snprintf(json, sizeof(json),
           "{\"size\":%" PRIu32 ",\"crc32\":%" PRIu32 ",\"width\":%u,\"height\":%u,\"bpp\":1,\"rotate\":%u}", size,
           usb::crc32(0, frame, size), display.getDisplayWidth(), display.getDisplayHeight(), screenshotRotation());
  sink.send(reply, usb::formatReply(reply, sizeof(reply), id, "data", json));
  if (!writeAll(frame, size)) return;
  sendOk(id);
}

void handlePress(uint32_t id, JsonObjectConst args) {
  usb::PressArgs a;
  const usb::Status s = usb::parsePressArgs(args, a);
  if (!s.ok()) return sendError(id, s);
  state.press = Press{};
  state.press.id = id;
  state.press.button = a.button;
  state.press.releaseAt = millis() + (a.longPress ? kLongPressMs : kShortPressMs);
  HalGPIO::setInjectedButtons(static_cast<uint8_t>(1u << a.button));
  state.mode = Mode::Press;
}

// Replies once the release has reached the activity and the resulting render is done.
void pollPress() {
  Press& p = state.press;
  const uint32_t now = millis();
  if (!p.released) {
    if (static_cast<int32_t>(now - p.releaseAt) < 0) return;
    HalGPIO::setInjectedButtons(0);
    p.released = true;
    p.releasedAt = now;
    return;
  }
  // The same cap covers a user holding the same button down.
  const bool settling = now - p.releasedAt < kPressSettleMs;
  if (gpio.isPressed(p.button) && settling) return;  // release not through debounce yet
  if (p.pollsSinceRelease < 2 && settling) {
    ++p.pollsSinceRelease;
    return;
  }
  if (!activityManager.renderSettled() && settling) return;
  state.mode = Mode::Idle;
  sendOk(p.id);
}

void handleRequest(const usb::Request& req) {
  arena.reset();
  JsonDocument doc(&arena);
  if (deserializeJson(doc, req.args) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    return sendError(req.id, usb::fail(usb::Error::BadRequest, "arguments must be a JSON object"));
  }
  const JsonObjectConst args = doc.as<JsonObjectConst>();
  const char* cmd = req.command;
  if (strcmp(cmd, "hello") == 0) return handleHello(req.id, args);
  if (strcmp(cmd, "time") == 0) return handleTime(req.id, args);
  if (strcmp(cmd, "ls") == 0) return handleLs(req.id, args);
  if (strcmp(cmd, "stat") == 0) return handleStat(req.id, args);
  if (strcmp(cmd, "get") == 0) return handleGet(req.id, args);
  if (strcmp(cmd, "put") == 0) return handlePut(req.id, args);
  if (strcmp(cmd, "rm") == 0) return handleRm(req.id, args);
  if (strcmp(cmd, "mkdir") == 0) return handleMkdir(req.id, args);
  if (strcmp(cmd, "mv") == 0) return handleMv(req.id, args);
  if (strcmp(cmd, "screenshot") == 0) return handleScreenshot(req.id);
  if (strcmp(cmd, "press") == 0) return handlePress(req.id, args);
  if (strcmp(cmd, "lag") == 0) return handleLag(req.id);
  if (strcmp(cmd, "heap") == 0) {
    x4::heap::mark("usb");
    JsonDocument heap(&arena);
    x4::heap::report(heap.to<JsonObject>());
    char json[448];
    serializeJson(heap, json, sizeof(json));
    return sendOk(req.id, json);
  }
  if (strcmp(cmd, "net") == 0) return handleNet(req.id, args);
  if (strcmp(cmd, "screen") == 0) {
    JsonDocument screen(&arena);
    JsonObject out = screen.to<JsonObject>();
    out["activity"] = activityManager.currentActivityName();
    x4ScreenFields(out);
    char json[256];
    serializeJson(screen, json, sizeof(json));
    return sendOk(req.id, json);
  }
  if (strcmp(cmd, "flash") == 0) return handleFlash(req.id, args);
  if (strcmp(cmd, "reboot") == 0) {
    sendOk(req.id);
    state.mode = Mode::Reboot;
    state.rebootAt = millis() + kRebootDelayMs;
    return;
  }
  if (strcmp(cmd, "bye") == 0) {
    sendOk(req.id);
    return endSession();
  }
  sendError(req.id, usb::fail(usb::Error::UnknownCommand, "unknown command"));
}

// Reads request bytes until one line is complete. During a press only busy is
// answered, since one command is in flight at a time.
void pollLines() {
  for (size_t budget = usb::kMaxLine + 2; budget > 0 && logSerial.available() > 0; --budget) {
    const usb::LineReader::Result r = lineReader.push(static_cast<char>(logSerial.read()));
    if (r == usb::LineReader::Result::Partial) continue;
    usb::Request req;
    const usb::Parse parsed = usb::parseRequest(lineReader.line(), req);
    if (parsed == usb::Parse::NotForUs) continue;
    openSession();
    state.lastActivityMs = millis();
    if (r == usb::LineReader::Result::Overflow) {
      sendError(req.id, usb::fail(usb::Error::TooLarge, "request line too long"));
    } else if (parsed == usb::Parse::Bad) {
      sendError(req.id, usb::fail(usb::Error::BadRequest, "malformed request line"));
    } else if (state.mode == Mode::Press) {
      sendError(req.id, usb::fail(usb::Error::Busy, "a press is in progress"));
    } else {
      handleRequest(req);
    }
    return;
  }
}

}  // namespace

void UsbLink::begin() { logSerial.setRxBufferSize(kRxQueueBytes); }

bool UsbLink::sessionOpen() const { return state.open; }

bool UsbLink::transferring() const {
  return state.mode == Mode::Upload || state.mode == Mode::Download ||
         (state.open && static_cast<int32_t>(millis() - state.rangeGapEnd) < 0);
}

void UsbLink::poll() {
  // A DTR/RTS-independent end: cable gone, host silent, or host not reading.
  if (state.open && (linkBroken || !gpio.isUsbConnected() ||
                     (state.mode == Mode::Idle && millis() - state.lastActivityMs > kSessionIdleMs))) {
    endSession();
  }
  switch (state.mode) {
    case Mode::Idle:
    case Mode::Press:
      pollLines();
      if (state.mode == Mode::Press) pollPress();
      break;
    case Mode::Upload:
      pollUpload();
      break;
    case Mode::Download:
      pollDownload();
      state.lastActivityMs = millis();
      break;
    case Mode::Reboot:
      if (static_cast<int32_t>(millis() - state.rebootAt) >= 0) {
        logSerial.flush();
        esp_restart();
      }
      break;
  }
}

__attribute__((weak)) void x4OnHostDataChanged() {
  sdFontSystem.markRegistryDirty();
  if (activityManager.isHomeActivity()) activityManager.goHome();
}

__attribute__((weak)) void x4HelloFields(JsonObject) {}
__attribute__((weak)) void x4ScreenFields(JsonObject) {}
__attribute__((weak)) void x4BeforeHostRead() {}

__attribute__((weak)) void x4OnHostTime(int64_t, int16_t, bool) {}

__attribute__((weak)) const char* x4NetCommand(JsonObjectConst, JsonObject) { return "this build has no Wi-Fi"; }

__attribute__((weak)) void x4OnHostWrote(const char*) {}

bool x4HostClock(int16_t& tzMinutes) {
  if (hostClock.tzSet) tzMinutes = hostClock.tzMinutes;
  return hostClock.set;
}
