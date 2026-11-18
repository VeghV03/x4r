#include "BookStats.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "LearnPaths.h"
#include "LearnTypes.h"
#include "StateFile.h"

namespace x4::books {

namespace {

constexpr uint32_t MAGIC = 0x42423458;  // "X4BB"
constexpr uint32_t VERSION = 2;

struct Header {
  uint32_t magic;
  uint32_t version;
  uint32_t recordSize;
  uint32_t shiftSeq;  // the last day shift its records got (shiftDays)
};

// The part of a record a lookup reads.
struct Head {
  uint64_t hash;  // FNV-1a 64 of the key
  uint32_t readMs;
  uint32_t pages;
  uint32_t seq;  // larger = read more recently
  uint32_t day;  // the day of the last read; 0 = unknown
  int64_t at;    // Unix seconds of the last read; 0 = unknown
};
struct Record {
  Head head;
  char key[KEY_MAX + 1];
};
static_assert(sizeof(Head) == 32 && sizeof(Record) == 512, "Record is an on-card format");

// Version 1: keys up to 147 bytes, no last read.
struct RecordV1 {
  char key[148];
  uint32_t readMs;
  uint32_t pages;
  uint32_t seq;
};
static_assert(sizeof(RecordV1) == 160, "RecordV1 is an on-card format");

uint32_t saturatingAdd(const uint32_t a, const uint32_t b) { return a > UINT32_MAX - b ? UINT32_MAX : a + b; }
uint64_t keyHash(const char* key) { return fnv1a64(key, std::strlen(key)); }
size_t offsetOf(const int index) { return sizeof(Header) + index * sizeof(Record); }

bool readHeader(HalFile& file, Header& header) {
  return file.seekSet(0) && file.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) &&
         header.magic == MAGIC;
}

bool isV1(const Header& header) { return header.version == 1 && header.recordSize == sizeof(RecordV1); }

// Rewrites a version 1 file as version 2, keeping every book's totals. On failure the version 1
// file stays, and so does its lastread.bin.
bool migrate() {
  HalFile old;
  if (!Storage.openFileForRead("X4", paths::BOOKS, old)) return false;
  HalFile out = Storage.open(paths::BOOKS_TMP, O_RDWR | O_CREAT | O_TRUNC);
  const Header header{MAGIC, VERSION, sizeof(Record), 0};
  bool ok = out && out.write(&header, sizeof(header)) == sizeof(header);
  const size_t count = (old.fileSize() - sizeof(Header)) / sizeof(RecordV1);
  for (size_t i = 0; ok && i < count && i < MAX_BOOKS; ++i) {
    RecordV1 v1{};
    if (!old.seekSet(sizeof(Header) + i * sizeof(RecordV1)) ||
        old.read(&v1, sizeof(v1)) != static_cast<int>(sizeof(v1)) || v1.key[sizeof(v1.key) - 1] != '\0') {
      continue;
    }
    Record r{};
    r.head = Head{keyHash(v1.key), v1.readMs, v1.pages, v1.seq, 0, 0};
    std::memcpy(r.key, v1.key, sizeof(v1.key));
    ok = out.write(&r, sizeof(r)) == sizeof(r);
  }
  old.close();
  if (!out || !statefile::commit(out, ok, paths::BOOKS_TMP, paths::BOOKS)) {
    LOG_ERR("X4", "Cannot move the reading stats to version 2");
    return false;
  }
  LOG_INF("X4", "Reading stats moved to version 2");
  // The last reads kept beside version 1, by builds of one day; version 2 holds them.
  if (Storage.exists(paths::LAST_READ_V1)) Storage.remove(paths::LAST_READ_V1);
  return true;
}

// Whether books.bin is on the card, after recovering a replace cut by power loss.
bool present() {
  statefile::recover(paths::BOOKS_TMP, paths::BOOKS);
  return Storage.exists(paths::BOOKS);
}

// books.bin opened for writing (created if missing) or reading, a version 1 file rewritten
// first. `count` gets its records, or -1 when it is new or not a books file.
bool openBooks(HalFile& file, const bool write, int& count) {
  for (int pass = 0; pass < 2; ++pass) {
    if (write) {
      file = Storage.open(paths::BOOKS, O_RDWR | O_CREAT);
    } else if (!Storage.openFileForRead("X4", paths::BOOKS, file)) {
      return false;
    }
    if (!file) return false;
    Header header{};
    const bool known = readHeader(file, header);
    if (known && isV1(header)) {
      // Never written over as an unknown file: a failed migration keeps the totals for the next try.
      file.close();
      if (pass == 0 && migrate()) continue;
      return false;
    }
    count = -1;
    if (known && header.version == VERSION && header.recordSize == sizeof(Record)) {
      const size_t n = (file.fileSize() - sizeof(Header)) / sizeof(Record);
      count = static_cast<int>(n < MAX_BOOKS ? n : MAX_BOOKS);
    }
    return true;
  }
  return false;
}

bool readHead(HalFile& file, const int index, Head& out) {
  return file.seekSet(offsetOf(index)) && file.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
}

// The record of `key` in a version 2 file, or -1.
int indexOf(HalFile& file, const int count, const uint64_t hash, Head& out) {
  for (int i = 0; i < count; ++i) {
    if (readHead(file, i, out) && out.hash == hash) return i;
  }
  return -1;
}

}  // namespace

bool add(const char* key, const uint32_t readMs, const uint32_t pages, const uint32_t day, const int64_t at) {
  const size_t length = std::strlen(key);
  if (length == 0 || length > KEY_MAX) return false;
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  present();
  HalFile file;
  int count = -1;
  if (!openBooks(file, true, count)) {
    LOG_ERR("X4", "Cannot open %s", paths::BOOKS);
    return false;
  }
  bool ok = true;
  if (count < 0) {
    // New or unreadable: start over.
    const Header header{MAGIC, VERSION, sizeof(Record), 0};
    ok = file.truncate(0) && file.seekSet(0) && file.write(&header, sizeof(header)) == sizeof(header);
    count = 0;
  }

  // The book's record, else a free slot, else the least recently read one.
  const uint64_t hash = keyHash(key);
  int slot = count < MAX_BOOKS ? count : 0;
  uint32_t oldestSeq = UINT32_MAX;
  uint32_t lastSeq = 0;
  Record record{};
  bool found = false;
  for (int i = 0; ok && i < count; ++i) {
    Head h{};
    if (!readHead(file, i, h)) continue;
    if (h.seq > lastSeq) lastSeq = h.seq;
    if (!found && h.hash == hash) {
      record.head = h;
      slot = i;
      found = true;
    } else if (!found && count == MAX_BOOKS && h.seq < oldestSeq) {
      oldestSeq = h.seq;
      slot = i;
    }
  }
  if (!found) record.head.hash = hash;
  std::memcpy(record.key, key, length);
  record.head.readMs = saturatingAdd(record.head.readMs, readMs);
  record.head.pages = saturatingAdd(record.head.pages, pages);
  record.head.seq = lastSeq + 1;
  record.head.day = day;
  record.head.at = at;
  ok = ok && file.seekSet(offsetOf(slot)) && file.write(&record, sizeof(record)) == sizeof(record);
  ok = file.close() && ok;
  if (!ok) LOG_ERR("X4", "Cannot save reading stats");
  return ok;
}

bool find(const char* key, uint32_t& readMs, uint32_t& pages) {
  readMs = pages = 0;
  HalFile file;
  int count = -1;
  if (!present() || !openBooks(file, false, count)) return false;
  Head h{};
  const bool found = indexOf(file, count, keyHash(key), h) >= 0;
  file.close();
  if (found) {
    readMs = h.readMs;
    pages = h.pages;
  }
  return found;
}

bool lastRead(const char* key, LastRead& out) {
  HalFile file;
  int count = -1;
  if (!present() || !openBooks(file, false, count)) return false;
  Head h{};
  const bool found = indexOf(file, count, keyHash(key), h) >= 0 && h.day > 0;
  file.close();
  if (found) out = LastRead{h.day, h.at};
  return found;
}

bool shiftDays(const int32_t delta, const uint32_t seq) {
  HalFile in;
  int count = -1;
  if (!present()) return true;
  if (!openBooks(in, false, count)) return false;
  Header header{};
  if (count < 0 || !readHeader(in, header) || header.shiftSeq == seq) {
    in.close();
    return true;
  }
  HalFile out = Storage.open(paths::BOOKS_TMP, O_RDWR | O_CREAT | O_TRUNC);
  header.shiftSeq = seq;
  bool ok = out && out.write(&header, sizeof(header)) == sizeof(header);
  for (int i = 0; ok && i < count; ++i) {
    Record r{};
    ok = in.seekSet(offsetOf(i)) && in.read(&r, sizeof(r)) == static_cast<int>(sizeof(r));
    if (r.head.day != 0) {
      const int64_t moved = static_cast<int64_t>(r.head.day) + delta;
      r.head.day = moved < 1 ? 1 : static_cast<uint32_t>(moved);
    }
    ok = ok && out.write(&r, sizeof(r)) == sizeof(r);
  }
  in.close();
  if (!out || !statefile::commit(out, ok, paths::BOOKS_TMP, paths::BOOKS)) {
    LOG_ERR("X4", "Day shift of the reading stats failed");
    return false;
  }
  return true;
}

void addTo(JsonObject books) {
  HalFile file;
  int count = -1;
  if (!present() || !openBooks(file, false, count)) return;
  for (int i = 0; i < count; ++i) {
    Record r{};
    if (!file.seekSet(offsetOf(i)) || file.read(&r, sizeof(r)) != static_cast<int>(sizeof(r)) || r.key[0] == '\0' ||
        r.key[KEY_MAX] != '\0') {
      continue;
    }
    JsonObject entry = books[static_cast<const char*>(r.key)].to<JsonObject>();
    entry["read_ms"] = r.head.readMs;
    entry["pages"] = r.head.pages;
  }
  file.close();
}

}  // namespace x4::books
