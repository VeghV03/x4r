#include "Puzzle.h"

#include "LearnTypes.h"

namespace x4 {

void orderShuffle(const char* id, const uint8_t count, uint8_t* out) {
  for (uint8_t i = 0; i < count; ++i) out[i] = i;
  if (count < 2) return;
  uint64_t state = itemIdHash(id) | 1u;
  // Fisher-Yates driven by xorshift64.
  for (uint8_t i = count - 1; i > 0; --i) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    const auto j = static_cast<uint8_t>(state % (i + 1u));
    const uint8_t swap = out[i];
    out[i] = out[j];
    out[j] = swap;
  }
  if (orderSolved(out, count)) orderMove(out, count, 0, count - 1);
}

void orderMove(uint8_t* rows, const uint8_t count, const uint8_t from, const uint8_t to) {
  if (from >= count || to >= count || from == to) return;
  const uint8_t moved = rows[from];
  if (from < to) {
    for (uint8_t i = from; i < to; ++i) rows[i] = rows[i + 1];
  } else {
    for (uint8_t i = from; i > to; --i) rows[i] = rows[i - 1];
  }
  rows[to] = moved;
}

bool orderSolved(const uint8_t* rows, const uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (rows[i] != i) return false;
  }
  return true;
}

}  // namespace x4
