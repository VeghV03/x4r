#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include <cstdint>

namespace freeink {
struct Icon;
}

// Drawing shared by the games: large glyphs from the built-in face, cursors and gray.
namespace x4::play::ui {

// Height above the baseline of a digit at `scale256`.
int glyphHeight(const GfxRenderer& renderer, int scale256);
// The scale at which a digit is `height` pixels tall.
int scaleForHeight(const GfxRenderer& renderer, int height);
// Width of a UTF-8 string drawn large.
int bigTextWidth(const GfxRenderer& renderer, const char* text, int scale256,
                 EpdFontFamily::Style style = EpdFontFamily::BOLD);
// Draws a UTF-8 string large, centred on `centreX`, baseline at `baseline`.
void drawBigText(const GfxRenderer& renderer, const char* text, int centreX, int baseline, int scale256,
                 EpdFontFamily::Style style = EpdFontFamily::BOLD, bool black = true);

// The row icon of a game, by its id; games without their own get the Play icon.
const freeink::Icon& gameIcon(const char* id);
const freeink::Icon& playIcon();

// Cursor weights: the one the arrows move, and one they do not move now.
inline constexpr int FOCUS_PEN = 4;
inline constexpr int REST_PEN = 2;
// A thick frame, drawn inwards from (x, y).
void drawCursor(const GfxRenderer& renderer, int x, int y, int width, int height, int pen);
// Turns every other pixel of the area white: black there reads as gray.
void lighten(const GfxRenderer& renderer, int x, int y, int width, int height);

}  // namespace x4::play::ui
