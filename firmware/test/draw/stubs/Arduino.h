#pragma once

// Host stand-in for the Arduino core: only what the drawing code reaches for.
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define PROGMEM

unsigned long millis();
unsigned long micros();

// Heap figures the font code reads to decide what to keep resident. The Mac has no such limit,
// so these are fixed at a level the X4 has when idle: nothing is dropped for lack of memory.
struct HostEsp {
  size_t getFreeHeap() const { return 200 * 1024; }
  size_t getMaxAllocHeap() const { return 120 * 1024; }
  [[noreturn]] void restart() const { abort(); }
};
inline HostEsp ESP;
