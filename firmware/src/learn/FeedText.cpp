#include "FeedText.h"

namespace x4::feed {

namespace {

void add(Fields& f, const Slice slice, uint16_t& length) {
  if (slice.length == 0 || f.count >= FIELDS_MAX) return;
  f.slice[f.count] = slice;
  f.base[f.count] = length;
  length = static_cast<uint16_t>(length + slice.length + 1);
  ++f.count;
}

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

Fields fieldsFor(const Item& item, const StreamPhase phase) {
  Fields f;
  uint16_t length = 0;
  switch (kindOf(item.shape).layout) {
    case Layout::Text:
    case Layout::Quote:
      if (phase == StreamPhase::Prompt) add(f, item.body, length);
      break;
    case Layout::Answer:
      if (phase == StreamPhase::Revealed) {
        add(f, item.body, length);
        add(f, item.why, length);
      }
      break;
    case Layout::Points:
      if (phase == StreamPhase::Revealed) {
        for (uint8_t i = 0; i < item.optionCount; ++i) add(f, item.options[i], length);
        add(f, item.why, length);
      }
      break;
    case Layout::Options:
    case Layout::Order:
      if (phase == StreamPhase::Answered) add(f, item.why, length);
      break;
  }
  return f;
}

int fieldOf(const Fields& fields, const Slice slice) {
  if (slice.length == 0) return -1;
  for (uint8_t i = 0; i < fields.count; ++i) {
    if (fields.slice[i].offset == slice.offset && fields.slice[i].length == slice.length) return i;
  }
  return -1;
}

size_t splitWords(const char* arena, const Fields& fields, Word* out, const size_t max) {
  size_t count = 0;
  for (uint8_t f = 0; f < fields.count; ++f) {
    const char* text = arena + fields.slice[f].offset;
    const uint16_t length = fields.slice[f].length;
    bool paragraph = true;
    for (uint16_t i = 0; i < length;) {
      if (isSpace(text[i])) {
        if (text[i] == '\n') paragraph = true;
        ++i;
        continue;
      }
      const uint16_t start = i;
      while (i < length && !isSpace(text[i])) ++i;
      if (out && count < max) {
        out[count] = {static_cast<uint16_t>(fields.base[f] + start), static_cast<uint16_t>(i - start), SCREEN_UNKNOWN,
                      paragraph};
      }
      paragraph = false;
      ++count;
    }
  }
  return out && count > max ? max : count;
}

const char* wordText(const char* arena, const Fields& fields, const Word& word) {
  uint8_t f = 0;
  while (f + 1 < fields.count && fields.base[f + 1] <= word.start) ++f;
  return arena + fields.slice[f].offset + (word.start - fields.base[f]);
}

int findWord(const Word* words, const size_t count, const uint16_t offset) {
  size_t lo = 0;
  size_t hi = count;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (words[mid].start + words[mid].length <= offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < count && words[lo].start <= offset ? static_cast<int>(lo) : -1;
}

sentences::Word Run::at(const void* run, const size_t index) {
  const auto& r = *static_cast<const Run*>(run);
  const Word& w = r.words[index];
  return {{wordText(r.arena, *r.fields, w), w.length}, w.paragraphStart};
}

sentences::Range firstSentence(const Run& run, const uint8_t screen) {
  const auto steps = run.steps();
  const int count = static_cast<int>(run.count);
  int first = -1;
  int later = -1;
  for (int i = 0; i < count && first < 0; ++i) {
    const uint8_t at = run.words[i].screen;
    if (at == screen) first = i;
    if (later < 0 && at != SCREEN_UNKNOWN && at > screen) later = i;
  }
  if (first < 0) first = later >= 0 ? later : 0;
  // A screen that opens mid-sentence starts at its first full sentence; the fragment stays one Up away.
  const Word& w = run.words[first];
  if (!w.paragraphStart && !sentences::canStart({wordText(run.arena, *run.fields, w), w.length})) {
    const int next = steps.sentenceEnd(first) + 1;
    if (next < count && run.words[next].screen == w.screen) first = next;
  }
  return {first, steps.sentenceEnd(first), first};
}

std::string selectedText(const Run& run, const int first, const int last) {
  std::string out;
  for (int i = first; i <= last; ++i) {
    const Word& w = run.words[i];
    if (i > first) out += w.paragraphStart ? "\n\n" : " ";
    out.append(wordText(run.arena, *run.fields, w), w.length);
  }
  return out;
}

bool overlaps(const Mark* marks, const uint8_t count, const uint16_t start, const uint16_t end) {
  for (uint8_t i = 0; i < count; ++i) {
    if (marks[i].start < end && start < marks[i].end) return true;
  }
  return false;
}

}  // namespace x4::feed
