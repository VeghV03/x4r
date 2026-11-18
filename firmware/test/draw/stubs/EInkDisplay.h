#pragma once

#include <cstdint>

#include <freeink-sdk/libs/display/FreeInkDisplay/include/GrayscaleCapabilities.h>

// The X4's panel in memory. It keeps what the SSD1677 would hold: the frame the renderer draws
// into, the two gray planes, and `glass`, the picture a person would see after the last update.
class EInkDisplay {
 public:
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // One byte per pixel of `glass`.
  enum Level : uint8_t { Black = 0, DarkGray = 1, LightGray = 2, White = 3 };

  EInkDisplay();
  // The panel built last: HalDisplay keeps its panel private, and the host has to read the glass.
  static EInkDisplay* latest;

  uint8_t frameBuffer[BUFFER_SIZE];  // 1 bit per pixel, a set bit is white
  uint8_t lsbPlane[BUFFER_SIZE];     // gray planes: a set bit asks for gray
  uint8_t msbPlane[BUFFER_SIZE];
  uint8_t glass[DISPLAY_WIDTH * DISPLAY_HEIGHT];
  bool inverted = false;
  bool lent = false;

  // A black-and-white update from `frame`.
  void show(const uint8_t* frame);
  // The gray update: pixels marked in the planes turn gray, the rest stay.
  void showGray();
};
