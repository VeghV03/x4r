#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "JsonlReader.h"

// The Read list (/x4/read.jsonl) and its close log (/x4/read-log.jsonl).
// Main-task only.
namespace x4::readlist {

// Most items the list shows; the rest wait until earlier ones are finished.
inline constexpr uint8_t MAX = 32;
// Bytes of a title kept for the list (cut on a UTF-8 boundary).
inline constexpr size_t TITLE_BYTES = 160;
inline constexpr size_t BOOK_BYTES = 96;
// Finished articles remembered on the device; the oldest goes first.
inline constexpr uint16_t DONE_MAX = 256;
inline constexpr float FINISHED_AT = 0.95f;

struct Entry {
  char title[TITLE_BYTES + 1] = {};
  char book[BOOK_BYTES + 1] = {};  // book key
  uint16_t minutes = 0;
};

// Parses one read.jsonl line. False for an invalid item (no id, title or book).
bool parseEntry(const char* line, size_t length, Entry& out);

// Unfinished items whose book is on the card, in file order. `out` may be null
// to only count. Returns how many (at most `max`).
uint8_t load(LineBuffer& buffer, Entry* out, uint8_t max);
// Every item whose book is on the card: the unfinished ones first, then the
// finished, each in file order. `unfinished` gets how many lead.
uint8_t loadAll(LineBuffer& buffer, Entry* out, uint8_t max, uint8_t& unfinished);

// The Read articles finished, loaded once for a pass over the list. Empty when memory is short.
struct Done {
  std::unique_ptr<uint64_t[]> hashes;
  uint16_t count = 0;
  bool has(const char* bookKey) const;
};
Done loadFinished();

// Copies at most `limit` bytes of `s` without splitting a UTF-8 sequence.
void copyCut(char* out, const char* s, size_t limit);

// True for a book under /books/read/.
bool isReadBook(const char* path);
bool isFinished(const char* bookKey);
bool finished(float progress, bool atEnd);

// The reader closed a book: appends its read-log line, and remembers a Read article
// when finished. `progress` is 0..1.
void onBookClosed(const char* bookPath, float progress, bool atEnd, uint32_t ms);

}  // namespace x4::readlist
