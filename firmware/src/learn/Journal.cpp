#include "Journal.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Item.h"
#include "LearnPaths.h"
#include "StateFile.h"

namespace x4 {

namespace {


void setAt(JsonDocument& doc, const int64_t at) {
  if (at > 0) {
    doc["at"] = at;
  } else {
    doc["at"] = nullptr;
  }
}

// Longest prefix of `text` within `limit` bytes that does not split a UTF-8 sequence.
size_t utf8Prefix(const char* text, const size_t limit) {
  size_t len = std::strlen(text);
  if (len <= limit) return len;
  len = limit;
  while (len > 0 && (static_cast<uint8_t>(text[len]) & 0xC0) == 0x80) --len;
  return len;
}

}  // namespace

bool appendJsonLine(const char* path, const char* line, const size_t length) {
  Storage.mkdir(paths::ROOT);
  HalFile file = Storage.open(path, O_RDWR | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("X4", "Cannot open %s for append", path);
    return false;
  }
  const size_t size = file.fileSize();
  bool ok = true;
  if (size > 0) {
    uint8_t last = '\n';
    ok = file.seekSet(size - 1) && file.read(&last, 1) == 1 && file.seekSet(size);
    if (ok && last != '\n') ok = file.write("\n", 1) == 1;
  }
  ok = ok && file.write(line, length) == length && file.write("\n", 1) == 1;
  ok = file.close() && ok;
  if (!ok) LOG_ERR("X4", "Append to %s failed", path);
  return ok;
}

bool appendJson(const char* path, const JsonDocument& doc) {
  const size_t length = measureJson(doc);
  auto line = makeUniqueNoThrow<char[]>(length + 1);
  if (!line) {
    LOG_ERR("X4", "OOM: JSON line (%u bytes)", static_cast<unsigned>(length + 1));
    return false;
  }
  serializeJson(doc, line.get(), length + 1);
  return appendJsonLine(path, line.get(), length);
}

void highlightId(const HighlightInput& input, char (&id)[16]) {
  const char* book = input.book[0] == '/' ? input.book + 1 : input.book;
  // Same passage, same id: a repeated highlight collapses on the host.
  uint64_t hash = fnv1a64(book, std::strlen(book));
  const uint32_t pos[3] = {input.spine, input.start, input.end};
  hash = fnv1a64(pos, sizeof(pos), hash);
  hash = fnv1a64(input.text, utf8Prefix(input.text, BODY_MAX), hash);
  // A daily-set highlight: the same words of another step are another highlight.
  if (input.item[0]) hash = fnv1a64(input.item, std::strlen(input.item), hash);
  snprintf(id, sizeof(id), "h-%08lx", static_cast<unsigned long>(hash ^ (hash >> 32)) & 0xffffffffUL);
}

bool writeHighlight(const HighlightInput& input, const uint32_t day, const int64_t at, char (&id)[16]) {
  const char* book = input.book[0] == '/' ? input.book + 1 : input.book;
  const size_t textLength = utf8Prefix(input.text, BODY_MAX);
  const bool fromSet = input.item[0] != '\0';
  highlightId(input, id);

  JsonDocument doc;
  doc["id"] = id;
  doc["kind"] = input.kind == HighlightKind::Question ? "question" : "highlight";
  doc["book"] = book;
  if (fromSet) {
    if (input.title[0]) doc["title"] = input.title;
    JsonObject item = doc["item"].to<JsonObject>();
    item["id"] = input.item;
    item["kind"] = input.itemKind;
    if (input.itemTitle[0]) item["title"] = std::string_view(input.itemTitle, utf8Prefix(input.itemTitle, SHORT_MAX));
  } else {
    doc["title"] = input.title;
    doc["author"] = input.author;
    doc["chapter"] = input.chapter;
  }
  doc["text"] = std::string_view(input.text, textLength);
  JsonObject posObject = doc["pos"].to<JsonObject>();
  if (!fromSet) posObject["spine"] = input.spine;
  posObject["start"] = input.start;
  posObject["end"] = input.end;
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::HIGHLIGHTS, doc);
}

namespace {

constexpr char DELETED_KEY[] = "\"deleted\":true";
constexpr size_t BLANK_MAX = 16;  // lines per id; a passage highlighted more often keeps the rest

// Overwrites [start, end) of an open file with spaces.
bool blank(HalFile& file, const uint32_t start, const uint32_t end) {
  char spaces[64];
  std::memset(spaces, ' ', sizeof(spaces));
  if (!file.seekSet(start)) return false;
  for (uint32_t at = start; at < end;) {
    const size_t n = std::min<size_t>(sizeof(spaces), end - at);
    if (file.write(spaces, n) != n) return false;
    at += n;
  }
  return true;
}

// Blanks the lines of `path` that start with {"id":"<id>". The device writes `id` first.
bool blankLines(const char* path, const char* id) {
  if (!Storage.exists(path)) return true;
  char needle[ID_MAX + 12];
  const int needleLength = snprintf(needle, sizeof(needle), "{\"id\":\"%s\"", id);
  if (needleLength <= 0 || static_cast<size_t>(needleLength) >= sizeof(needle)) return false;
  HalFile file = Storage.open(path, O_RDWR);
  if (!file) return false;
  uint32_t starts[BLANK_MAX];
  uint32_t ends[BLANK_MAX];
  size_t found = 0;
  uint32_t lineStart = 0;
  int matched = 0;  // bytes of `needle` matched at the line start; -1 once it cannot match
  uint32_t at = 0;
  uint8_t chunk[128];
  int got = 0;
  while (found < BLANK_MAX && (got = file.read(chunk, sizeof(chunk))) > 0) {
    for (int i = 0; i < got; ++i, ++at) {
      const char c = static_cast<char>(chunk[i]);
      if (c == '\n') {
        if (matched == needleLength && found < BLANK_MAX) {
          starts[found] = lineStart;
          ends[found++] = at;
        }
        lineStart = at + 1;
        matched = 0;
      } else if (matched >= 0 && matched < needleLength) {
        matched = c == needle[matched] ? matched + 1 : -1;
      }
    }
  }
  bool ok = got >= 0;
  for (size_t i = 0; ok && i < found; ++i) ok = blank(file, starts[i], ends[i]);
  // A last line without a line end is a cut-off write; appendJsonLine starts a new line after it.
  ok = file.close() && ok;
  if (!ok) LOG_ERR("X4", "Blanking %s in %s failed", id, path);
  return ok;
}

}  // namespace

bool writeHighlightDeletion(const char* id, const uint32_t day, const int64_t at) {
  const bool blanked = blankLines(paths::HIGHLIGHTS, id);
  JsonDocument doc;
  doc["id"] = id;
  doc["deleted"] = true;
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::HIGHLIGHTS, doc) && blanked;
}

bool isHighlightDeletion(const char* line, const size_t length) {
  // Inside a JSON string the quotes would be escaped, so this only matches the key.
  const size_t keyLength = sizeof(DELETED_KEY) - 1;
  for (size_t i = 0; i + keyLength <= length; ++i) {
    if (std::memcmp(line + i, DELETED_KEY, keyLength) == 0) return true;
  }
  return false;
}

bool writeReview(const char* deckId, const char* cardId, const CardType type, const fsrs::Grade grade,
                 const int8_t correct, const int8_t pick, const uint32_t ms, const uint32_t day, const int64_t at) {
  char key[2 * ID_MAX + 2];
  snprintf(key, sizeof(key), "%s/%s", deckId, cardId);
  JsonDocument doc;
  doc["card"] = key;
  doc["type"] = kindOf(static_cast<Shape>(type)).name;
  doc["grade"] = static_cast<uint8_t>(grade);
  if (correct >= 0) doc["correct"] = correct > 0;
  if (pick >= 0) doc["pick"] = pick;
  doc["ms"] = ms;
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::REVIEWS, doc);
}

bool writeSeen(const char* itemId, const uint32_t day, const int64_t at) {
  JsonDocument doc;
  doc["id"] = itemId;
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::SEEN_LOG, doc);
}

bool writeSeenArticle(const char* itemId, const ArticleSeen& seen, const uint32_t day, const int64_t at) {
  JsonDocument doc;
  doc["id"] = itemId;
  doc["opened"] = seen.opened;
  if (seen.pick >= 0) {
    JsonObject predict = doc["predict"].to<JsonObject>();
    predict["pick"] = seen.pick;
    predict["correct"] = seen.correct;
    predict["ms"] = seen.ms;
  }
  if (seen.link >= 0) {
    JsonObject link = doc["link"].to<JsonObject>();
    link["pick"] = seen.link;
    link["correct"] = seen.linkCorrect;
    link["ms"] = seen.linkMs;
  }
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::SEEN_LOG, doc);
}

bool writeSeenForecast(const char* itemId, const uint8_t percent, const uint32_t ms, const uint32_t day,
                       const int64_t at, const char* readId) {
  JsonDocument doc;
  doc["id"] = itemId;
  doc["p"] = percent;
  doc["ms"] = ms;
  if (readId && *readId) doc["read"] = readId;
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::SEEN_LOG, doc);
}

bool writeSeenResult(const char* itemId, const bool correct, const int8_t pick, const uint32_t ms,
                     const uint32_t day, const int64_t at) {
  JsonDocument doc;
  doc["id"] = itemId;
  doc["correct"] = correct;
  if (pick >= 0) doc["pick"] = pick;
  doc["ms"] = ms;
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::SEEN_LOG, doc);
}

bool writeReadLog(const char* book, const float progress, const bool finished, const uint32_t ms, const uint32_t day,
                  const int64_t at) {
  JsonDocument doc;
  doc["book"] = book;
  doc["progress"] = std::round(static_cast<double>(progress) * 100.0) / 100.0;
  doc["finished"] = finished;
  doc["ms"] = ms;
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::READ_LOG, doc);
}

bool writeLookup(const char* word, const char* book, const char* title, const uint32_t day, const int64_t at) {
  constexpr size_t WORD_MAX = 64;
  constexpr size_t TITLE_MAX = 200;
  JsonDocument doc;
  doc["word"] = std::string_view(word, utf8Prefix(word, WORD_MAX));
  doc["book"] = book;
  if (title && *title) doc["title"] = std::string_view(title, utf8Prefix(title, TITLE_MAX));
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::LOOKUPS, doc);
}

bool writeVisit(const char* section, const char* issue, const uint32_t ms, const uint32_t day, const int64_t at) {
  JsonDocument doc;
  doc["section"] = section;
  if (issue && *issue) {
    doc["issue"] = issue;
  } else {
    doc["issue"] = nullptr;
  }
  doc["ms"] = ms;
  doc["day"] = day;
  setAt(doc, at);
  return appendJson(paths::VISITS, doc);
}

bool writeStats(const LearnState& state, void (*books)(JsonObject out), void (*play)(JsonObject out)) {
  JsonDocument doc;
  doc["day"] = state.day;
  if (state.retention >= 0.0f) {
    doc["retention"] = std::round(static_cast<double>(state.retention) * 1000.0) / 1000.0;
  } else {
    doc["retention"] = nullptr;
  }
  doc["cards"] = state.cards;
  doc["due"] = state.due;
  JsonObject bookObject = doc["books"].to<JsonObject>();
  if (books) books(bookObject);
  if (play) play(doc["play"].to<JsonObject>());

  const size_t length = measureJson(doc);
  auto out = makeUniqueNoThrow<char[]>(length + 2);
  if (!out) {
    LOG_ERR("X4", "OOM: stats.json (%u bytes)", static_cast<unsigned>(length + 2));
    return false;
  }
  serializeJson(doc, out.get(), length + 1);
  out[length] = '\n';
  Storage.mkdir(paths::ROOT);
  return statefile::replace(paths::STATS_TMP, paths::STATS, {{out.get(), length + 1}});
}

}  // namespace x4
