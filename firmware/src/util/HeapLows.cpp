#include "HeapLows.h"

#include <HalMemory.h>
#include <freertos/FreeRTOS.h>

#include <cstdint>
#include <cstring>

namespace x4::heap {

namespace {

constexpr uint8_t kKept = 8;  // the newest lows; a reply line holds no more

struct Low {
  const char* stage;
  uint32_t min;
};

Low lows[kKept];
uint8_t count = 0;
uint32_t lowest = UINT32_MAX;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

}  // namespace

void mark(const char* stage) {
  const uint32_t now = static_cast<uint32_t>(HalMemory::getInternalHeap().minFreeBytes);
  portENTER_CRITICAL(&lock);
  if (now < lowest) {
    lowest = now;
    if (count == kKept) {
      std::memmove(lows, lows + 1, (kKept - 1) * sizeof(Low));
      --count;
    }
    lows[count++] = {stage, now};
  }
  portEXIT_CRITICAL(&lock);
}

void report(JsonObject out) {
  const HalMemory::HeapStats heap = HalMemory::getInternalHeap();
  out["free"] = heap.freeBytes;
  out["min"] = heap.minFreeBytes;
  out["block"] = heap.largestBlockBytes;
  Low copy[kKept];
  portENTER_CRITICAL(&lock);
  const uint8_t n = count;
  std::memcpy(copy, lows, n * sizeof(Low));
  portEXIT_CRITICAL(&lock);
  JsonArray low = out["low"].to<JsonArray>();
  for (uint8_t i = 0; i < n; ++i) {
    JsonObject o = low.add<JsonObject>();
    o["at"] = copy[i].stage;
    o["min"] = copy[i].min;
  }
}

}  // namespace x4::heap
