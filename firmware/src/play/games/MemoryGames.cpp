#include "MemoryGames.h"

#include "play/Rng.h"

namespace x4::play::memory {

namespace {

uint8_t clampLevel(const uint8_t level) { return level < 1 ? 1 : level > 10 ? 10 : level; }

// The chain grows from 6 to 12 steps, and the numbers with it.
constexpr ArithLevel ARITH[10] = {
    {6, 11, 199, 2, 12},  {7, 11, 199, 2, 12},  {7, 21, 299, 2, 13}, {8, 21, 299, 3, 15},
    {8, 31, 399, 3, 15},  {9, 31, 399, 3, 17},  {9, 41, 499, 3, 17}, {10, 41, 499, 4, 19},
    {11, 51, 499, 4, 19}, {12, 51, 499, 4, 19},
};

constexpr int16_t START_MIN = 100;
constexpr char OPS[] = {'+', '-', 'x', '/'};
constexpr uint8_t DIVISORS_MAX = 20;

int smaller(const int a, const int b) { return a < b ? a : b; }

char inverse(const char op) {
  switch (op) {
    case '+':
      return '-';
    case '-':
      return '+';
    case 'x':
      return '/';
    default:
      return 'x';
  }
}

// A number in [lo, hi] other than `skip` (0: none); false when there is none.
bool pick(Rng& rng, const int lo, const int hi, const int skip, uint16_t& out) {
  const bool skipping = skip >= lo && skip <= hi;
  const int span = hi - lo + 1 - (skipping ? 1 : 0);
  if (span <= 0) return false;
  int value = lo + static_cast<int>(rng.below(static_cast<uint32_t>(span)));
  if (skipping && value >= skip) ++value;
  out = static_cast<uint16_t>(value);
  return true;
}

// A step of kind `op` from `value` within the level's ranges, with any
// number but `skip`; false when none fits.
bool makeStep(Rng& rng, const ArithLevel& spec, const int16_t value, const char op, const int skip,
              ArithStep& out) {
  out.op = op;
  switch (op) {
    case '+':
      return pick(rng, spec.addMin, smaller(spec.addMax, VALUE_MAX - value), skip, out.value);
    case '-':
      return pick(rng, spec.addMin, smaller(spec.addMax, value - 1), skip, out.value);
    case 'x':
      return pick(rng, spec.mulMin, smaller(spec.mulMax, VALUE_MAX / value), skip, out.value);
    default: {
      // Exact, and never down to 1.
      uint8_t divisors[DIVISORS_MAX];
      uint8_t n = 0;
      for (int d = spec.mulMin; d <= spec.mulMax && n < DIVISORS_MAX; ++d) {
        if (d != skip && value % d == 0 && value / d > 1) divisors[n++] = static_cast<uint8_t>(d);
      }
      if (n == 0) return false;
      out.value = divisors[rng.below(n)];
      return true;
    }
  }
}

}  // namespace

const ArithLevel& arithLevel(const uint8_t level) { return ARITH[clampLevel(level) - 1]; }

int16_t arithApply(const int16_t value, const ArithStep& step) {
  switch (step.op) {
    case '+':
      return static_cast<int16_t>(value + step.value);
    case '-':
      return static_cast<int16_t>(value - step.value);
    case 'x':
      return static_cast<int16_t>(value * step.value);
    case '/':
      return step.value ? static_cast<int16_t>(value / step.value) : value;
    default:
      return value;
  }
}

void arithMake(const uint32_t seed, const uint8_t level, ArithChain& out) {
  const ArithLevel& spec = arithLevel(level);
  Rng rng(seed);
  out = ArithChain{};
  out.start = static_cast<int16_t>(START_MIN + rng.below(VALUE_MAX - START_MIN + 1));
  int16_t value = out.start;
  ArithStep previous{0, 0};
  for (uint8_t i = 0; i < spec.count; ++i) {
    // Every kind that fits, other than the last one, equally likely. A step
    // does not undo the last one (x 7 then / 7) unless nothing else fits.
    // One of + and - always fits: a + leaves room to go down, a - room to go
    // up, and addMin is far below half of VALUE_MAX.
    ArithStep fits[4] = {};
    uint8_t n = 0;
    for (int undo = 0; undo < 2 && n == 0; ++undo) {
      for (const char op : OPS) {
        const int skip = !undo && op == inverse(previous.op) ? previous.value : 0;
        if (op != previous.op && makeStep(rng, spec, value, op, skip, fits[n])) ++n;
      }
    }
    const ArithStep& step = fits[rng.below(n)];
    out.steps[i] = step;
    value = arithApply(value, step);
    previous = step;
  }
  out.count = spec.count;
  out.result = value;
}

}  // namespace x4::play::memory
