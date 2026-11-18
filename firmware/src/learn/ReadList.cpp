#include "ReadList.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "Journal.h"
#include "Learn.h"
#include "LearnPaths.h"
#include "StateFile.h"

namespace x4::readlist {

namespace {

constexpr uint32_t DONE_MAGIC = 0x44523458;  // "X4RD"

struct DoneHeader {
  uint32_t magic;
  uint32_t count;
};

const char* bookKey(const char* path) { return *path == '/' ? path + 1 : path; }

uint64_t bookHash(const char* key) { return fnv1a64(key, std::strlen(key)); }

uint16_t loadDone(uint64_t* out) {
  statefile::recover(paths::READ_DONE_TMP, paths::READ_DONE);
  HalFile file;
  if (!Storage.exists(paths::READ_DONE) || !Storage.openFileForRead("READ", paths::READ_DONE, file)) return 0;
  DoneHeader header{};
  uint16_t count = 0;
  if (file.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) && header.magic == DONE_MAGIC) {
    const uint32_t want = std::min<uint32_t>(header.count, DONE_MAX);
    const int got = file.read(out, want * sizeof(uint64_t));
    count = got > 0 ? static_cast<uint16_t>(static_cast<size_t>(got) / sizeof(uint64_t)) : 0;
  }
  file.close();
  return count;
}

bool writeDone(const uint64_t* hashes, const uint16_t count) {
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  const DoneHeader header{DONE_MAGIC, count};
  return statefile::replace(paths::READ_DONE_TMP, paths::READ_DONE,
                            {{&header, sizeof(header)}, {hashes, count * sizeof(uint64_t)}});
}

void addDone(const uint64_t hash) {
  auto hashes = makeUniqueNoThrow<uint64_t[]>(DONE_MAX);
  if (!hashes) {
    LOG_ERR("READ", "OOM: finished list (%u bytes)", static_cast<unsigned>(DONE_MAX * sizeof(uint64_t)));
    return;
  }
  uint16_t count = loadDone(hashes.get());
  if (std::find(hashes.get(), hashes.get() + count, hash) != hashes.get() + count) return;
  if (count >= DONE_MAX) {
    std::memmove(hashes.get(), hashes.get() + 1, (count - 1) * sizeof(uint64_t));
    --count;
  }
  hashes[count++] = hash;
  if (!writeDone(hashes.get(), count)) LOG_ERR("READ", "Cannot save the finished list");
}

}  // namespace

void copyCut(char* out, const char* s, const size_t limit) {
  size_t len = std::strlen(s);
  if (len > limit) {
    len = limit;
    while (len > 0 && (static_cast<uint8_t>(s[len]) & 0xC0) == 0x80) --len;
  }
  std::memcpy(out, s, len);
  out[len] = '\0';
}

bool parseEntry(const char* line, const size_t length, Entry& out) {
  JsonDocument doc;
  if (deserializeJson(doc, line, length) || !doc.is<JsonObject>()) return false;
  const char* id = doc["id"].as<const char*>();
  const char* title = doc["title"].as<const char*>();
  const char* book = doc["book"].as<const char*>();
  if (!id || !*id || std::strlen(id) > ID_MAX || !title || !*title || !book) return false;
  book = bookKey(book);
  if (!*book || std::strlen(book) > BOOK_BYTES) return false;
  out = Entry{};
  copyCut(out.title, title, TITLE_BYTES);
  std::memcpy(out.book, book, std::strlen(book) + 1);
  const int minutes = doc["minutes"].is<int>() ? doc["minutes"].as<int>() : 0;
  out.minutes = static_cast<uint16_t>(std::clamp(minutes, 0, 999));
  return true;
}

namespace {

// Appends the items whose finished state is `wantFinished` to `out` from `count`, up to `max`.
uint8_t scan(LineBuffer& buffer, const uint64_t* done, const uint16_t doneCount, const bool wantFinished, Entry* out,
             uint8_t count, const uint8_t max) {
  HalFile file;
  if (count >= max || !Storage.exists(paths::READ_LIST) || !Storage.openFileForRead("READ", paths::READ_LIST, file)) {
    return count;
  }
  const uint64_t* doneEnd = done + doneCount;
  JsonlReader reader(file, buffer);
  size_t length = 0;
  uint32_t offset = 0;
  JsonlReader::Line line;
  Entry entry;
  char path[BOOK_BYTES + 2];
  while (count < max && (line = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (line != JsonlReader::Line::Ok) continue;
    if (!parseEntry(buffer.text, length, entry)) {
      LOG_ERR("READ", "Skipped read.jsonl line at %u", static_cast<unsigned>(offset));
      continue;
    }
    if ((std::find(done, doneEnd, bookHash(entry.book)) != doneEnd) != wantFinished) continue;
    snprintf(path, sizeof(path), "/%s", entry.book);
    if (!Storage.exists(path)) continue;
    if (out) out[count] = entry;
    ++count;
  }
  file.close();
  return count;
}

std::unique_ptr<uint64_t[]> loadDoneList(uint16_t& count) {
  count = 0;
  auto done = makeUniqueNoThrow<uint64_t[]>(DONE_MAX);
  if (!done) {
    LOG_ERR("READ", "OOM: finished list (%u bytes)", static_cast<unsigned>(DONE_MAX * sizeof(uint64_t)));
    return done;
  }
  count = loadDone(done.get());
  return done;
}

}  // namespace

uint8_t load(LineBuffer& buffer, Entry* out, const uint8_t max) {
  uint16_t doneCount = 0;
  const auto done = loadDoneList(doneCount);
  if (!done) return 0;
  return scan(buffer, done.get(), doneCount, false, out, 0, max);
}

uint8_t loadAll(LineBuffer& buffer, Entry* out, const uint8_t max, uint8_t& unfinished) {
  uint16_t doneCount = 0;
  const auto done = loadDoneList(doneCount);
  unfinished = 0;
  if (!done) return 0;
  unfinished = scan(buffer, done.get(), doneCount, false, out, 0, max);
  return scan(buffer, done.get(), doneCount, true, out, unfinished, max);
}

Done loadFinished() {
  Done done;
  done.hashes = loadDoneList(done.count);
  return done;
}

bool Done::has(const char* key) const {
  if (!hashes) return false;
  const uint64_t* begin = hashes.get();
  const uint64_t* end = begin + count;
  return std::find(begin, end, bookHash(bookKey(key))) != end;
}

bool isReadBook(const char* path) {
  constexpr size_t PREFIX = sizeof(paths::READ_BOOKS) - 2;  // without the leading '/' and the NUL
  return std::strncmp(bookKey(path), paths::READ_BOOKS + 1, PREFIX) == 0 && bookKey(path)[PREFIX] != '\0';
}

bool isFinished(const char* key) {
  auto done = makeUniqueNoThrow<uint64_t[]>(DONE_MAX);
  if (!done) return false;
  const uint16_t count = loadDone(done.get());
  return std::find(done.get(), done.get() + count, bookHash(bookKey(key))) != done.get() + count;
}

bool finished(const float progress, const bool atEnd) { return atEnd || progress >= FINISHED_AT; }

void onBookClosed(const char* bookPath, float progress, const bool atEnd, const uint32_t ms) {
  progress = atEnd ? 1.0f : std::clamp(progress, 0.0f, 1.0f);
  const char* key = bookKey(bookPath);
  const size_t length = std::strlen(key);
  if (length == 0 || key[length - 1] == '/') return;  // a folder, no book
  // A book's session is logged too, for its minutes of the week (D26).
  if (!isReadBook(bookPath)) {
    writeReadLog(key, progress, atEnd, ms, learn::today(), learn::wallClock());
    return;
  }
  const bool done = finished(progress, atEnd);
  writeReadLog(key, progress, done, ms, learn::today(), learn::wallClock());
  if (done) {
    addDone(bookHash(key));
    learn::noteArticleFinished(bookPath);  // one screen long: no page to turn
  }
}

}  // namespace x4::readlist
