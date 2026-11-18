#pragma once

#include <cstdint>

// A small deterministic generator: the same seed gives the same puzzle on the
// device and on the host.
namespace x4::play {

// Mixes 32 bits (the murmur3 finalizer).
inline uint32_t mix32(uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

class Rng {
 public:
  explicit Rng(const uint32_t seed) : state(mix32(seed) | 1u) {}

  uint32_t next() {  // xorshift32
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }

  // Uniform enough in [0, n) for n far below 2^32.
  uint32_t below(const uint32_t n) { return n ? next() % n : 0; }

  template <typename T>
  void shuffle(T* items, const uint32_t count) {
    for (uint32_t i = count; i > 1; --i) {
      const uint32_t j = below(i);
      const T tmp = items[i - 1];
      items[i - 1] = items[j];
      items[j] = tmp;
    }
  }

 private:
  uint32_t state;
};

}  // namespace x4::play
