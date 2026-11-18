#pragma once

#include <cstddef>
#include <cstdint>

// Deleting highlights of the open book: from the reader's clipping store, which
// shades them, and from /x4/highlights.jsonl, which the host pulls.
namespace x4::highlights {

// Removes clipping `index` of the loaded book and logs the deletion for the host.
bool remove(size_t index);

// Removes every clipping of `spine` whose text overlaps [start, end). False when none was removed.
bool removeOverlapping(uint16_t spine, uint32_t start, uint32_t end);

}  // namespace x4::highlights
