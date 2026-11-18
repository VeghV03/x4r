#pragma once

#include <cstddef>
#include <string_view>

// Sentence boundaries over laid-out words, for sentence-step highlighting.
// Pure C++: no allocation, no Arduino dependencies.
namespace sentences {

struct Word {
  std::string_view text;  // one whitespace-separated token, punctuation attached
  bool paragraphStart = false;
};

// Word `index` of a run of `count` words.
using WordAt = Word (*)(const void* ctx, size_t index);

// True when word `index` is the last word of a sentence. A sentence ends at
// . ! ? or … (closing quotes and brackets may follow), at a paragraph end and at
// the end of the run. It does not end at common English and Hungarian
// abbreviations, initials, ordinals, list markers, or before a lower-case word.
bool endsAt(WordAt word, const void* ctx, size_t count, size_t index);

// True when `text` can open a sentence: its first letter is not lower case.
bool canStart(std::string_view text);

}  // namespace sentences
