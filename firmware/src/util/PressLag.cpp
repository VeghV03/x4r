#include "PressLag.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>

#include <algorithm>
#include <cstring>

namespace presslag {
namespace {

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Entry ring[kCount];
size_t head = 0;  // next slot to write
size_t filled = 0;

// The press waiting for its panel update; `pending` is cleared once it has one.
Entry current{};
uint32_t pressedAt = 0;
bool pending = false;
bool panelStarted = false;
// After the first update returns, later ones extend ring[chainSlot] until the next press.
bool chaining = false;
size_t chainSlot = 0;
uint32_t chainDoneAt = 0;

Mark markRing[kMarks];
size_t markCount = 0;

uint16_t since(const uint32_t at, const uint32_t now) {
  return static_cast<uint16_t>(std::min<uint32_t>(now - at, UINT16_MAX));
}

}  // namespace

void pressed(const char* activity, const uint32_t idleMs, const bool lowClock) {
  taskENTER_CRITICAL(&mux);
  current = Entry{};
  strncpy(current.activity, activity, sizeof(current.activity) - 1);
  current.idleMs = idleMs;
  current.lowClock = lowClock;
  pressedAt = millis();
  pending = true;
  panelStarted = false;
  chaining = false;
  markCount = 0;
  taskEXIT_CRITICAL(&mux);
}

void woke() {
  pressed("wake", 0, false);
  taskENTER_CRITICAL(&mux);
  pressedAt = 0;
  taskEXIT_CRITICAL(&mux);
}

void mark(const char* tag) {
  const uint32_t now = millis();
  taskENTER_CRITICAL(&mux);
  if (markCount < kMarks) {
    Mark& m = markRing[markCount++];
    strncpy(m.tag, tag, sizeof(m.tag) - 1);
    m.tag[sizeof(m.tag) - 1] = '\0';
    m.ms = since(pressedAt, now);
  }
  taskEXIT_CRITICAL(&mux);
}

size_t marks(Mark* out, const size_t cap) {
  taskENTER_CRITICAL(&mux);
  const size_t n = std::min(markCount, cap);
  for (size_t i = 0; i < n; ++i) out[i] = markRing[i];
  taskEXIT_CRITICAL(&mux);
  return n;
}

void released() {
  taskENTER_CRITICAL(&mux);
  if (pending && current.upMs == 0) current.upMs = std::max<uint16_t>(since(pressedAt, millis()), 1);
  taskEXIT_CRITICAL(&mux);
}

void onRefresh(const uint8_t mode, const bool done) {
  const uint32_t now = millis();
  taskENTER_CRITICAL(&mux);
  if (pending && !done && !panelStarted) {
    current.panelMs = since(pressedAt, now);
    current.mode = mode;
    panelStarted = true;
  } else if (pending && done && panelStarted) {
    current.doneMs = since(pressedAt, now);
    current.lastMs = current.doneMs;
    current.updates = 1;
    ring[head] = current;
    chainSlot = head;
    chainDoneAt = now;
    chaining = true;
    head = (head + 1) % kCount;
    filled = std::min(filled + 1, kCount);
    pending = false;
  } else if (chaining && !done && now - chainDoneAt > kChainGapMs) {
    chaining = false;
  } else if (chaining && done) {
    Entry& e = ring[chainSlot];
    e.lastMs = since(pressedAt, now);
    if (e.updates < UINT8_MAX) ++e.updates;
    chainDoneAt = now;
  }
  taskEXIT_CRITICAL(&mux);
}

size_t snapshot(Entry* out, const size_t cap) {
  taskENTER_CRITICAL(&mux);
  const size_t n = std::min(filled, cap);
  for (size_t i = 0; i < n; ++i) out[i] = ring[(head + kCount - n + i) % kCount];
  taskEXIT_CRITICAL(&mux);
  return n;
}

}  // namespace presslag
