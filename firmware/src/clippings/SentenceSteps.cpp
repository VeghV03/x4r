#include "SentenceSteps.h"

namespace sentences {

bool Steps::endsSentence(const int index) const {
  return endsAt(at, ctx, static_cast<size_t>(count), static_cast<size_t>(index));
}

int Steps::sentenceStart(int index) const {
  while (index > 0 && !endsSentence(index - 1)) --index;
  return index;
}

int Steps::sentenceEnd(int index) const {
  while (index + 1 < count && !endsSentence(index)) ++index;
  return index;
}

int Steps::paragraphEnd(int index) const {
  while (index + 1 < count && !at(ctx, static_cast<size_t>(index + 1)).paragraphStart) ++index;
  return index;
}

size_t Steps::bytes(const int first, const int last) const {
  size_t total = 0;
  for (int i = first; i <= last; ++i) total += at(ctx, static_cast<size_t>(i)).text.size() + 1;
  return total;
}

bool Steps::next(Range& range) const {
  const int start = sentenceEnd(range.last) + 1;
  if (start >= count) return false;
  range = {start, sentenceEnd(start), start};
  return true;
}

bool Steps::previous(Range& range) const {
  if (range.first <= 0) return false;
  const int start = sentenceStart(range.first - 1);
  range = {start, range.first - 1, start};
  return true;
}

bool Steps::grow(Range& range) const {
  if (range.last + 1 >= count) return false;
  const int end = sentenceEnd(range.last + 1);
  if (!fits(range.first, end)) return false;
  range.last = range.focus = end;
  return true;
}

bool Steps::growParagraph(Range& range) const {
  if (range.last + 1 >= count) return false;
  const int target = paragraphEnd(range.last + 1);
  size_t used = bytes(range.first, range.last);
  int end = -1;
  for (int i = range.last + 1; i <= target; ++i) {
    used += at(ctx, static_cast<size_t>(i)).text.size() + 1;
    if (used > limit) break;
    if (i == target || endsSentence(i)) end = i;
  }
  if (end < 0) return false;
  range.last = range.focus = end;
  return true;
}

bool Steps::shrink(Range& range) const {
  const int end = sentenceStart(range.last) - 1;
  if (end < range.first) return false;
  range.last = range.focus = end;
  return true;
}

bool Steps::shrinkWord(Range& range) const {
  if (range.last <= range.first) return false;
  range.last = range.focus = range.last - 1;
  return true;
}

bool wantsPage(const size_t loaded, const size_t bytesAfterFirst, const size_t maxPages, const size_t limit) {
  return loaded < maxPages && (loaded == 0 || bytesAfterFirst < limit);
}

}  // namespace sentences
