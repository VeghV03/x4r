#pragma once

#include <cstddef>
#include <cstdint>

#include "FeedText.h"
#include "Journal.h"

// Highlights made on a step. Like the reader's, each goes to
// /x4/highlights.jsonl for the host; a mark in /x4/state/marks.bin keeps its
// range so the step shows it shaded when it comes back.
namespace x4::feed {

// The step a highlight comes from.
struct Source {
  const char* key = "";        // stream item id, or "<deck>/<card id>"
  const char* kind = "";       // item kind or card type
  const char* title = "";      // its title, or prompt
  const char* book = "";       // its book key; "" names the step itself: "stream/<key>"
  const char* bookTitle = "";  // the book's title when known
};

// The marks saved on step `key`, in the order made. Returns the count.
uint8_t loadMarks(const char* key, Mark* out, uint8_t max);
// Logs the highlight of joined offsets [start, end) and saves its mark in `out`.
bool save(const Source& source, uint16_t start, uint16_t end, const char* text, HighlightKind kind, Mark& out);
// Deletes every highlight of step `key` overlapping [start, end) and logs each
// deletion. False when none was removed.
bool removeOverlapping(const char* key, uint16_t start, uint16_t end);
// The `book` of a deck's header line, or false.
bool deckBook(const char* deckId, char* out, size_t capacity);

}  // namespace x4::feed
