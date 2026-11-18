#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

// Reading totals and the last read per book, for stats.json `books`, the reader's pace and the
// recaps. Kept in /x4/state/books.bin: fixed records found by key hash, rewritten in place.
namespace x4::books {

// Books kept; past this the least recently read one makes room.
constexpr uint8_t MAX_BOOKS = 32;
constexpr size_t KEY_MAX = 479;  // book key bytes (no leading '/'); longer keys are not kept

// Adds a reading of `key` to its totals and makes it the book's last read, counted in `set`
// at `at` (Unix seconds, 0 when the clock is unknown). False when the key is too long or the
// card fails.
bool add(const char* key, uint32_t readMs, uint32_t pages, uint32_t day, int64_t at);
// The totals of `key`; false (and zeros) when it has none.
bool find(const char* key, uint32_t& readMs, uint32_t& pages);

struct LastRead {
  uint32_t day;
  int64_t at;
};
// When `key` was last read; false when it never was since its totals began.
bool lastRead(const char* key, LastRead& out);

// Sets `read_ms` and `pages` on books[key] for every kept book.
// Moves every book's last day by `delta` unless books.bin carries `seq` (DeckStore's
// shiftDays). True when it carries `seq` after, or there is no file.
bool shiftDays(int32_t delta, uint32_t seq);
void addTo(JsonObject books);

}  // namespace x4::books
