#pragma once

#include <cstdint>

// The X4's board profile, cut down to what the renderer and HalDisplay.h read.
namespace BoardConfig {

enum class DisplayController : uint8_t { SSD1677 = 0 };

// Bezel-covered edges of the panel; the SDK's defaults, which the X4 profile keeps.
struct ViewableInsets {
  uint8_t top = 9;
  uint8_t right = 3;
  uint8_t bottom = 3;
  uint8_t left = 3;
};

struct BoardProfile {
  DisplayController displayController = DisplayController::SSD1677;
  uint16_t displayWidth = 800;
  uint16_t displayHeight = 480;
  ViewableInsets viewableInsets = {};
};

inline BoardProfile ACTIVE = {};

}  // namespace BoardConfig
