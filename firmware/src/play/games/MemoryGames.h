#pragma once

#include <cstdint>

// Rules of the arithmetic chains.
// Pure C++, so the host tests cover them apart from the screens.
namespace x4::play::memory {

// ---- arithmetic chains ----------------------------------------------------------

inline constexpr uint8_t CHAIN_MAX = 12;
inline constexpr int16_t VALUE_MAX = 999;

struct ArithStep {
  char op;  // '+', '-', 'x' (times) or '/' (exact division)
  uint16_t value;
};

struct ArithChain {
  int16_t start = 0;
  ArithStep steps[CHAIN_MAX] = {};
  uint8_t count = 0;
  int16_t result = 0;
};

// What a level asks for.
struct ArithLevel {
  uint8_t count;    // steps after the start number
  uint16_t addMin;  // range of a number added or taken away
  uint16_t addMax;
  uint8_t mulMin;  // range of a factor or divisor
  uint8_t mulMax;
};
const ArithLevel& arithLevel(uint8_t level);

// A three-digit start, then steps that keep every running value within 1-999.
// Division is exact and the same operation never comes twice in a row.
void arithMake(uint32_t seed, uint8_t level, ArithChain& out);
int16_t arithApply(int16_t value, const ArithStep& step);

}  // namespace x4::play::memory
