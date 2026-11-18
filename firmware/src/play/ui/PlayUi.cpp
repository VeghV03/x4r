#include "PlayUi.h"

#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "components/icons/listIcons.h"
#include "components/icons/menuIcons.h"
#include "fontIds.h"
#include "learn/ui/LearnUi.h"

namespace x4::play::ui {

namespace {

// The largest built-in face; outlines are scaled, so no larger font is loaded.
#ifndef OMIT_FONTS
constexpr int BIG_FONT_ID = NOTOSANS_18_FONT_ID;
#else
constexpr int BIG_FONT_ID = NOTOSANS_14_FONT_ID;
#endif

// ASCII stand-ins the face has proper glyphs for.
uint32_t display(const uint32_t cp) {
  switch (cp) {
    case '-':
      return 0x2212;  // minus
    case 'x':
      return 0x00D7;  // times
    case '/':
      return 0x00F7;  // divide
    default:
      return cp;
  }
}

template <typename Fn>
void eachCodepoint(const char* text, Fn fn) {
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) fn(display(utf8NextCodepoint(&p)));
}

int advanceOf(const GfxRenderer& renderer, const uint32_t cp, const int scale256, const EpdFontFamily::Style style) {
  const x4::ui::BigFace face = x4::ui::bigFace(renderer, cp, scale256);
  int32_t advance = 0;
  int top = 0;
  if (!renderer.getCodepointMetrics(face.fontId, cp, style, advance, top)) return 0;
  return static_cast<int>((advance * face.scale256 / 256 + 8) >> 4);
}

// Keyed by GameInfo::id, so a game added to Games.cpp finds its icon here.
struct GameIcon {
  const char* id;
  const freeink::Icon* icon;
};
constexpr GameIcon GAME_ICONS[] = {
    {"sudoku", &icon_sudoku_32},
    {"nonogram", &icon_nonogram_32},
    {"einstein", &icon_einstein_32},
    {"arith", &icon_arith_32},
    {"words", &icon_words_32},
    {"crossword", &icon_crossword_32},
    {"chess", &icon_chess_32},
    {"puzzles", &icon_book_32},
};

}  // namespace

const freeink::Icon& gameIcon(const char* id) {
  for (const GameIcon& entry : GAME_ICONS) {
    if (std::strcmp(entry.id, id) == 0) return *entry.icon;
  }
  return icon_play_32;
}

const freeink::Icon& playIcon() { return icon_play_32; }

int glyphHeight(const GfxRenderer& renderer, const int scale256) {
  int32_t advance = 0;
  int top = 0;
  if (!renderer.getCodepointMetrics(BIG_FONT_ID, '0', EpdFontFamily::BOLD, advance, top)) return 0;
  return top * scale256 / 256;
}

int scaleForHeight(const GfxRenderer& renderer, const int height) {
  const int base = glyphHeight(renderer, 256);
  return base > 0 ? height * 256 / base : 256;
}

int bigTextWidth(const GfxRenderer& renderer, const char* text, const int scale256, const EpdFontFamily::Style style) {
  int width = 0;
  eachCodepoint(text, [&](const uint32_t cp) { width += advanceOf(renderer, cp, scale256, style); });
  return width;
}

void drawBigText(const GfxRenderer& renderer, const char* text, const int centreX, const int baseline,
                 const int scale256, const EpdFontFamily::Style style, const bool black) {
  int x = centreX - bigTextWidth(renderer, text, scale256, style) / 2;
  eachCodepoint(text, [&](const uint32_t cp) {
    const x4::ui::BigFace face = x4::ui::bigFace(renderer, cp, scale256);
    x += renderer.drawScaledCodepoint(face.fontId, cp, style, x, baseline, face.scale256, black);
  });
}

void drawCursor(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
                const int pen) {
  for (int i = 0; i < pen; ++i) renderer.drawRect(x + i, y + i, width - 2 * i, height - 2 * i, true);
}

void lighten(const GfxRenderer& renderer, const int x, const int y, const int width, const int height) {
  for (int row = y; row < y + height; ++row) {
    for (int col = x + ((row ^ x) & 1); col < x + width; col += 2) renderer.drawPixel(col, row, false);
  }
}

}  // namespace x4::play::ui
