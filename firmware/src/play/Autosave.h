#pragma once

#include <cstdint>

// When a game in progress is written to the card while it is played, so a
// power cut, crash or reboot loses at most the last few seconds. A change is
// saved once input pauses for IDLE_MS, or after BUSY_MS of steady play, and
// only while the game says its state is resumable (not mid-round). Saves land
// in pauses, never between two quick presses, and at most one per IDLE_MS.
namespace x4::play {

class Autosave {
 public:
  static constexpr uint32_t IDLE_MS = 2000;
  static constexpr uint32_t BUSY_MS = 15000;

  void changed(const uint32_t now) {
    if (!dirty) since = now;
    dirty = true;
    last = now;
  }

  bool due(const uint32_t now, const bool resumable) const {
    return dirty && resumable && (now - last >= IDLE_MS || now - since >= BUSY_MS);
  }

  void saved() { dirty = false; }

 private:
  bool dirty = false;
  uint32_t since = 0;  // the first change not yet saved
  uint32_t last = 0;   // the latest change
};

}  // namespace x4::play
