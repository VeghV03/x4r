#include "FeedHighlights.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "DeckStore.h"
#include "Learn.h"
#include "LearnPaths.h"

namespace x4::feed {

namespace {

// One mark, stored as-is (little-endian). A key of 0 is a deleted mark.
struct MarkRecord {
  uint64_t key;  // itemIdHash() of the step key
  uint16_t start;
  uint16_t end;
  char id[12];
};
static_assert(sizeof(MarkRecord) == 24, "MarkRecord is an on-card format");

uint64_t keyHash(const char* key) {
  const uint64_t hash = itemIdHash(key);
  return hash ? hash : 1;
}

bool readRecord(HalFile& file, MarkRecord& out) {
  return file.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
}

bool appendRecord(const MarkRecord& record) {
  Storage.mkdir(paths::STATE);
  HalFile file = Storage.open(paths::MARKS, O_RDWR | O_CREAT | O_AT_END);
  if (!file) return false;
  // A record cut short by a power cut is overwritten.
  const size_t size = file.fileSize();
  bool ok = size % sizeof(record) == 0 || file.seekSet(size - size % sizeof(record));
  ok = ok && file.write(&record, sizeof(record)) == sizeof(record);
  return file.close() && ok;
}

}  // namespace

uint8_t loadMarks(const char* key, Mark* out, const uint8_t max) {
  if (!Storage.exists(paths::MARKS)) return 0;
  HalFile file = Storage.open(paths::MARKS);
  if (!file) return 0;
  const uint64_t hash = keyHash(key);
  uint8_t count = 0;
  MarkRecord record{};
  while (count < max && readRecord(file, record)) {
    if (record.key != hash) continue;
    Mark& m = out[count++];
    m.start = record.start;
    m.end = record.end;
    std::memcpy(m.id, record.id, sizeof(m.id));
    m.id[sizeof(m.id) - 1] = '\0';
  }
  file.close();
  return count;
}

bool save(const Source& source, const uint16_t start, const uint16_t end, const char* text, const HighlightKind kind,
          Mark& out) {
  // The step's own pseudo book when it has none.
  char pseudo[ID_MAX * 2 + 16];
  const bool ownBook = source.book[0] != '\0';
  if (!ownBook) snprintf(pseudo, sizeof(pseudo), "stream/%s", source.key);

  HighlightInput input;
  input.book = ownBook ? source.book : pseudo;
  input.title = ownBook ? source.bookTitle : source.title;
  input.text = text;
  input.start = start;
  input.end = end;
  input.kind = kind;
  input.item = source.key;
  input.itemKind = source.kind;
  input.itemTitle = source.title;
  char id[16];
  if (!learn::appendHighlight(input, id)) return false;

  out.start = start;
  out.end = end;
  std::memset(out.id, 0, sizeof(out.id));
  std::strncpy(out.id, id, sizeof(out.id) - 1);
  MarkRecord record{};
  record.key = keyHash(source.key);
  record.start = start;
  record.end = end;
  std::memcpy(record.id, out.id, sizeof(record.id));
  // The highlight is logged; without its mark it is only not shaded.
  if (!appendRecord(record)) LOG_ERR("X4", "Failed to save the mark of %s", id);
  return true;
}

bool removeOverlapping(const char* key, const uint16_t start, const uint16_t end) {
  if (!Storage.exists(paths::MARKS)) return false;
  HalFile file = Storage.open(paths::MARKS, O_RDWR);
  if (!file) return false;
  const uint64_t hash = keyHash(key);
  bool removed = false;
  MarkRecord record{};
  for (uint32_t at = 0; file.seekSet(at) && readRecord(file, record); at += sizeof(record)) {
    if (record.key != hash || record.start >= end || start >= record.end) continue;
    char id[sizeof(record.id) + 1] = {};
    std::memcpy(id, record.id, sizeof(record.id));
    if (!learn::forgetHighlight(id)) LOG_ERR("X4", "Failed to log deletion of %s", id);
    const uint64_t gone = 0;
    if (!file.seekSet(at) || file.write(&gone, sizeof(gone)) != sizeof(gone)) {
      LOG_ERR("X4", "Failed to delete the mark of %s", id);
      break;
    }
    removed = true;
  }
  file.close();
  return removed;
}

bool deckBook(const char* deckId, char* out, const size_t capacity) {
  constexpr size_t HEADER_MAX = 1024;
  char path[64];
  decks::deckPath(deckId, path);
  HalFile file = Storage.open(path);
  if (!file) return false;
  auto header = makeUniqueNoThrow<char[]>(HEADER_MAX);
  const int got = header ? file.read(header.get(), HEADER_MAX - 1) : -1;
  file.close();
  if (got <= 0) return false;
  header[got] = '\0';
  if (char* end = std::strchr(header.get(), '\n')) *end = '\0';
  JsonDocument filter;
  filter["book"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, header.get(), DeserializationOption::Filter(filter))) return false;
  const char* book = doc["book"] | "";
  if (!*book || std::strlen(book) >= capacity) return false;
  std::strcpy(out, book);
  return true;
}

}  // namespace x4::feed
