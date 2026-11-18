#pragma once

#include <cstddef>
#include <cstdint>

// Times each button press to the panel updates it causes, so `x4 lag` can show where a slow
// press spends its time. An update starting within kChainGapMs of the previous one returning
// belongs to the same press. A ring of the last kCount presses, 28 bytes each.
namespace presslag {

struct Entry {
  char activity[12];
  uint32_t idleMs;     // since the input before this press
  uint16_t upMs;       // press to release; 0 when still held at the panel update
  uint16_t panelMs;    // press to the first panel update starting
  uint16_t doneMs;     // press to that update returning
  uint16_t lastMs;     // press to the last update of its chain returning (the gray pass)
  uint8_t mode;        // HalDisplay::RefreshMode, or HalDisplay::GRAY_UPDATE
  uint8_t updates;     // panel updates in the chain
  bool lowClock;       // the CPU was at its idle clock when the press came
};

constexpr size_t kCount = 16;
constexpr uint32_t kChainGapMs = 1500;

// Main task: a button went down / came up.
void pressed(const char* activity, uint32_t idleMs, bool lowClock);
void released();
// setup(): a wake is a reset, timed from app start like a press. Lost when the device sleeps.
void woke();
// HalDisplay::refreshHook.
void onRefresh(uint8_t mode, bool done);

// A named point in the work of the latest press, timed from its button-down. Marks after the
// last of kMarks are dropped; a new press starts over.
constexpr size_t kMarks = 16;
struct Mark {
  char tag[10];
  uint16_t ms;
};
void mark(const char* tag);
size_t marks(Mark* out, size_t cap);

// Completed presses, oldest first. Copies under the lock; returns how many.
size_t snapshot(Entry* out, size_t cap);

}  // namespace presslag
