#pragma once

#include <cstdint>

// Pure helpers for the `order` book puzzle, host-tested.
namespace x4 {

// The rows an `order` item first shows: out[i] is the solved index shown at
// position i. A shuffle seeded by the item id, so the same item always opens
// the same way, and never already solved.
void orderShuffle(const char* id, uint8_t count, uint8_t* out);

// Moves the row at `from` to `to`, shifting the rows between.
void orderMove(uint8_t* rows, uint8_t count, uint8_t from, uint8_t to);

// True when every row is in its solved place.
bool orderSolved(const uint8_t* rows, uint8_t count);

}  // namespace x4
