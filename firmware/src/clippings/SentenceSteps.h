#pragma once

#include <cstddef>

#include "Sentences.h"

// Sentence-step selection over a run of laid-out words that may span pages.
// The selection is [first, last]; `focus` is the word the screen follows, so the
// page turns when it crosses a page break. Pure C++: no allocation.
namespace sentences {

// Selected text never exceeds this many bytes: the highlight text limit in docs/DATA.md.
inline constexpr size_t SELECTION_TEXT_MAX = 4000;

struct Range {
  int first = 0;
  int last = 0;
  int focus = 0;
};

class Steps {
 public:
  Steps(WordAt word, const void* ctx, size_t count, size_t limit = SELECTION_TEXT_MAX)
      : at(word), ctx(ctx), count(static_cast<int>(count)), limit(limit) {}

  int sentenceStart(int index) const;
  int sentenceEnd(int index) const;
  // Last word of the paragraph that holds `index`.
  int paragraphEnd(int index) const;
  // Upper bound of the text bytes of words [first, last], one separator per word.
  size_t bytes(int first, int last) const;

  // Each returns false and leaves `range` alone when it cannot move.
  bool next(Range& range) const;          // the sentence after the selection
  bool previous(Range& range) const;      // the sentence before it
  bool grow(Range& range) const;          // end at the next sentence end
  bool growParagraph(Range& range) const; // end at the next paragraph end, or the last sentence that fits
  bool shrink(Range& range) const;        // end at the previous sentence end, never before the first
  bool shrinkWord(Range& range) const;    // end one word earlier

 private:
  bool endsSentence(int index) const;
  bool fits(int first, int last) const { return bytes(first, last) <= limit; }

  WordAt at;  // not `word`: Arduino.h defines a word() macro
  const void* ctx;
  int count;
  size_t limit;
};

// While loading the pages a selection may span: true when page `loaded` (0-based)
// should be loaded too. The pages after the first must hold a full selection, so
// one can start anywhere on the first page and still reach the text limit.
bool wantsPage(size_t loaded, size_t bytesAfterFirst, size_t maxPages, size_t limit = SELECTION_TEXT_MAX);

}  // namespace sentences
