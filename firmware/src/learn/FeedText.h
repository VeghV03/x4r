#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "Item.h"
#include "clippings/SentenceSteps.h"
#include "StreamInput.h"

// The text of a stream step that can be highlighted, as words for sentence
// steps. Offsets are into the step's fields joined as if by one byte each, so
// one range can name text across an explain card's points. Pure: no allocation but the
// selected text.
namespace x4::feed {

inline constexpr uint8_t FIELDS_MAX = LIST_MAX;
// Saved highlights shaded on one step.
inline constexpr uint8_t MARKS_MAX = 16;
inline constexpr uint8_t SCREEN_UNKNOWN = 0xFF;

struct Fields {
  Slice slice[FIELDS_MAX];
  uint16_t base[FIELDS_MAX] = {};
  uint8_t count = 0;
};

// What `item` shows as text to read in `phase`: a bite's or highlight's body; once revealed, a card's back or points
// and its why; once answered, a puzzle's why. Prompts and options are left out.
Fields fieldsFor(const Item& item, StreamPhase phase);
// The field holding `slice`, or -1.
int fieldOf(const Fields& fields, Slice slice);

struct Word {
  uint16_t start;  // joined offset
  uint16_t length;
  uint8_t screen;  // where the step's layout put it; SCREEN_UNKNOWN until laid out
  bool paragraphStart;
};

// Splits the fields into words at spaces, tabs and line ends, as ui::flowText
// wraps them. A field's first word and a word after a line end start a
// paragraph. Fills at most `max` (none when `out` is null); returns the count.
size_t splitWords(const char* arena, const Fields& fields, Word* out, size_t max);
const char* wordText(const char* arena, const Fields& fields, const Word& word);
// The word holding joined offset `offset`, or -1.
int findWord(const Word* words, size_t count, uint16_t offset);

// Sentence steps over the words.
struct Run {
  const char* arena;
  const Fields* fields;
  const Word* words;
  size_t count;

  sentences::Steps steps() const { return {&Run::at, this, count}; }
  static sentences::Word at(const void* run, size_t index);
};

// Where a selection starts on `screen`: its first sentence, or the fragment
// it opens with when no sentence starts there; past it, the first word laid
// out later, else the first word.
sentences::Range firstSentence(const Run& run, uint8_t screen);
// Words [first, last], a blank line before each paragraph after the first.
std::string selectedText(const Run& run, int first, int last);
// Joined offsets [start, end) the words cover.
inline uint16_t rangeStart(const Run& run, const int first) { return run.words[first].start; }
inline uint16_t rangeEnd(const Run& run, const int last) {
  return static_cast<uint16_t>(run.words[last].start + run.words[last].length);
}

// A highlight saved on a step: its joined offsets and highlights.jsonl id.
struct Mark {
  uint16_t start = 0;
  uint16_t end = 0;
  char id[12] = {};
};
bool overlaps(const Mark* marks, uint8_t count, uint16_t start, uint16_t end);

}  // namespace x4::feed
