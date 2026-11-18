#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "Fsrs.h"
#include "LearnTypes.h"
#include "SetRules.h"

// Writers for the device-owned files of docs/DATA.md.
namespace x4 {

enum class HighlightKind : uint8_t { Highlight, Question };

struct HighlightInput {
  const char* book = "";     // book path; a leading '/' is dropped to form the book key
  const char* title = "";
  const char* author = "";
  const char* chapter = "";
  const char* text = "";     // cut to 4000 bytes on a UTF-8 boundary
  uint16_t spine = 0;
  uint32_t start = 0;
  uint32_t end = 0;
  HighlightKind kind = HighlightKind::Highlight;
  // A highlight made on a step: the stream item id (or "<deck>/<card id>"),
  // its kind and title. Then `start`/`end` are offsets in the step's text and
  // `spine`, `author` and `chapter` are not written.
  const char* item = "";
  const char* itemKind = "";
  const char* itemTitle = "";  // cut to 200 bytes
};

// Appends one line (no line end in `line`) to a JSONL file and closes it again:
// the host pulls a log by renaming it, so the next append must create a fresh
// file. Starts a new line first when a previous write was cut short.
bool appendJsonLine(const char* path, const char* line, size_t length);
// Serializes `doc` as one line and appends it to `path`.
bool appendJson(const char* path, const JsonDocument& doc);

// The highlight's id: same book, position and text, same id.
void highlightId(const HighlightInput& input, char (&id)[16]);
// `at` 0 writes null.
bool writeHighlight(const HighlightInput& input, uint32_t day, int64_t at, char (&id)[16]);
// Blanks every highlights.jsonl line with this id in place (offsets stay valid) and
// appends {"id","deleted":true,"day","at"} so a host that pulled it learns of the deletion.
bool writeHighlightDeletion(const char* id, uint32_t day, int64_t at);
// True for a deletion record line of highlights.jsonl.
bool isHighlightDeletion(const char* line, size_t length);
// `correct` below 0: the reader graded it. `pick`: the option chosen, below 0 when there was none.
bool writeReview(const char* deckId, const char* cardId, CardType type, fsrs::Grade grade, int8_t correct,
                 int8_t pick, uint32_t ms, uint32_t day, int64_t at);
bool writeSeen(const char* itemId, uint32_t day, int64_t at);
// An article's seen line: whether it was opened, and what its questions were answered. A pick
// below 0: not asked or not answered.
struct ArticleSeen {
  bool opened = false;
  int8_t pick = -1;  // the predict guess
  bool correct = false;
  uint32_t ms = 0;
  int8_t link = -1;  // the link's pick
  bool linkCorrect = false;
  uint32_t linkMs = 0;
};
bool writeSeenArticle(const char* itemId, const ArticleSeen& seen, uint32_t day, int64_t at);
// A forecast's seen line: the row picked, in percent.
// `readId`: the piece it followed, when it followed one.
bool writeSeenForecast(const char* itemId, uint8_t percent, uint32_t ms, uint32_t day, int64_t at,
                       const char* readId = nullptr);
// A book puzzle's seen line, with its result.
bool writeSeenResult(const char* itemId, bool correct, int8_t pick, uint32_t ms, uint32_t day, int64_t at);
// One /x4/read-log.jsonl line. `book` is a book key.
bool writeReadLog(const char* book, float progress, bool finished, uint32_t ms, uint32_t day, int64_t at);
// One /x4/lookups.jsonl line: a word looked up in the dictionary while reading `book` (a book
// key). `word` is cut to 64 bytes, `title` to 200.
bool writeLookup(const char* word, const char* book, const char* title, uint32_t day, int64_t at);
// One /x4/visits.jsonl line: `ms` on the screen of `section` in the issue of `issue` (empty: none).
bool writeVisit(const char* section, const char* issue, uint32_t ms, uint32_t day, int64_t at);
// Rewrites /x4/stats.json. `books` and `play` fill those objects (either may be null).
bool writeStats(const LearnState& state, void (*books)(JsonObject out), void (*play)(JsonObject out) = nullptr);

}  // namespace x4
