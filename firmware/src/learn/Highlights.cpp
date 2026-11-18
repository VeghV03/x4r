#include "Highlights.h"

#include <Logging.h>

#include <string>

#include "ClippingStore.h"
#include "Journal.h"
#include "Learn.h"

namespace x4::highlights {

bool remove(const size_t index) {
  const Clipping* clipping = CLIPPINGS.clippingAt(index);
  if (!clipping) return false;
  // Rebuild the id the reader gave the highlight when it saved the clipping.
  std::string text;
  const bool named = CLIPPINGS.readClippingText(index, text);
  HighlightInput input;
  input.book = CLIPPINGS.filePath().c_str();
  input.text = text.c_str();
  input.spine = clipping->spineIndex;
  if (clipping->startOffset != UINT32_MAX && clipping->endOffset != UINT32_MAX) {
    input.start = clipping->startOffset;
    input.end = clipping->endOffset;
  }
  char id[16];
  highlightId(input, id);
  if (!CLIPPINGS.removeClippingAt(index)) {
    LOG_ERR("X4", "Failed to delete clipping %u", static_cast<unsigned>(index));
    return false;
  }
  if (!named) {
    LOG_ERR("X4", "Deleted clipping %u without its text; the host keeps its highlight", static_cast<unsigned>(index));
    return true;
  }
  if (!learn::forgetHighlight(id)) LOG_ERR("X4", "Failed to log deletion of %s", id);
  return true;
}

bool removeOverlapping(const uint16_t spine, const uint32_t start, const uint32_t end) {
  if (start == UINT32_MAX || end == UINT32_MAX) return false;
  bool removed = false;
  for (size_t i = CLIPPINGS.clippingCount(); i-- > 0;) {
    const Clipping& clipping = *CLIPPINGS.clippingAt(i);
    if (clipping.spineIndex != spine || clipping.startOffset == UINT32_MAX || clipping.endOffset == UINT32_MAX ||
        clipping.startOffset >= end || start >= clipping.endOffset) {
      continue;
    }
    if (!remove(i)) return false;
    removed = true;
  }
  return removed;
}

}  // namespace x4::highlights
