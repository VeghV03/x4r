#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>

#include <cstdint>

namespace freeink {
struct Icon;
}

// Drawing helpers shared by the Today home, the daily stream and the sleep card.
class MappedInputManager;

namespace x4::ui {

// True when a button went down this frame: a press on the screen, for its visit (Visits).
bool anyPress(const MappedInputManager& input);

// Called once per wrapped line. `line` is empty for a paragraph gap.
using LineFn = void (*)(void* ctx, const char* line, int index);

// Word-wraps `text` to `width` pixels. A single newline breaks the line, a blank
// line leaves one empty line. Words wider than a line break at a character.
// Returns the number of lines; `fn` may be null to only count.
int flowText(const GfxRenderer& renderer, int fontId, EpdFontFamily::Style style, const char* text, int width,
             LineFn fn, void* ctx);

// A word of a wrapped line: text bytes [source, source + length), drawn from x
// `left` to `right` past the line's start.
struct Span {
  uint16_t source;
  uint16_t length;
  int16_t left;
  int16_t right;
};
// Called once per wrapped line with its first 40 words. `line` is empty for a paragraph gap.
using SpanFn = void (*)(void* ctx, const char* line, int index, const Span* spans, int count);
// flowText that also reports each line's words.
int flowSpans(const GfxRenderer& renderer, int fontId, EpdFontFamily::Style style, const char* text, int width,
              SpanFn fn, void* ctx);

// Highlighted byte ranges of a text, drawn on a light gray ground as the reader shades its highlights.
struct Shade {
  uint16_t start;
  uint16_t end;
};
struct Shading {
  const Shade* shades = nullptr;
  uint8_t count = 0;
  uint16_t base = 0;  // where the drawn text starts in the shades' offsets
};
bool isShaded(const Shading& shading, uint16_t start, uint16_t end);
// The ground under one line's shaded words, the line drawn at (x, y). BW pass only, before the text.
void drawShading(const GfxRenderer& renderer, int x, int y, int height, const Span* spans, int count,
                 const Shading& shading);

// A text block: lines [firstLine, endLine) drawn while they fit above `bottom`.
struct TextBox {
  int fontId;
  EpdFontFamily::Style style;
  int x;
  int y;
  int width;
  int lineHeight;
  int bottom;
  bool black = true;
};
// Draws the block and returns the y below its last drawn line. A block never
// starts with a paragraph gap. `next` gets the first line not drawn. `shading` is optional.
int drawTextBox(const GfxRenderer& renderer, const TextBox& box, const char* text, int firstLine = 0,
                int endLine = 1 << 30, int* next = nullptr, const Shading* shading = nullptr);

// Page starts for text that may span screens. The first page starts at
// `firstTop` (below a title), the others at `top`.
struct Pages {
  static constexpr int MAX = 32;
  uint16_t start[MAX + 1] = {};
  uint8_t count = 0;
};
void paginate(const GfxRenderer& renderer, const TextBox& box, int firstTop, int top, const char* text, Pages& out);

// Screen frame with the generous x4r margins.
struct Frame {
  int left;
  int top;
  int width;
  int bottom;  // above the progress dots and button hints
  int dotsY;   // centre line of the progress dots
};
Frame frame(const GfxRenderer& renderer);

// One dot per step: done filled, the current one ringed, the rest hollow.
// Falls back to a thin bar when the dots would not fit.
void drawProgressDots(const GfxRenderer& renderer, int done, int total, int centreY);

// A bare number drawn large with a built-in font, left at `x`, baseline at `baseline`.
// Returns the drawn width.
int drawBigNumber(const GfxRenderer& renderer, uint32_t value, int x, int baseline, int scale256);
// The face a big glyph is drawn from. `scale256` is relative to the largest built-in face; past 1.5x
// of it a digit or sign comes from the digits face scaled down, so its edges stay crisp.
struct BigFace {
  int fontId;
  int scale256;
};
BigFace bigFace(const GfxRenderer& renderer, uint32_t cp, int scale256);
int bigNumberWidth(const GfxRenderer& renderer, uint32_t value, int scale256);
// Height of the digits above the baseline.
int bigNumberHeight(const GfxRenderer& renderer, int scale256);

// A check mark inside a size x size square at (x, y).
void drawCheck(const GfxRenderer& renderer, int x, int y, int size);
// A cross inside a size x size square at (x, y).
void drawCross(const GfxRenderer& renderer, int x, int y, int size);

// An ending screen (the finished set): a check or a cross alone, a little above the middle.
void drawOutcome(const GfxRenderer& renderer, const Frame& f, bool good);

// Menu rows (Today, Play): a column of 32 px icons at the frame's left edge,
// labels after them, and a right column for a bare number or a check.
inline constexpr int ROW_PAD = 14;   // above and below a row's content
inline constexpr int MARK_GAP = 16;  // the selection bar sits this far left of the frame
inline constexpr int ICON_SIZE = 32;
inline constexpr int ICON_GAP = 16;  // icon to label
inline constexpr int CHECK_SIZE = 24;
inline constexpr int CHECK_GAP = 16;  // a value in front of the check column
inline constexpr int TAG_GAP = 18;    // a tag in front of the value

struct MenuRow {
  const freeink::Icon* icon = nullptr;
  const char* label = "";
  EpdFontFamily::Style style = EpdFontFamily::BOLD;
  const char* value = "";    // a bare number, right-aligned
  const char* tag = "";      // a short code in front of the value
  bool check = false;        // in the right column, instead of the value
  bool checkColumn = false;  // the value stands in front of the check column, checked or not
  bool plain = false;        // no icon column: the label starts at the frame's left, as in the Library
};

// The x of a row's label, after the icon column.
int rowTextX(const Frame& f);
// Top of a text line within a row at `top` whose content is `lineHeight` tall.
int rowLineTop(int top, int lineHeight);
// The y on which icons and checks centre for a text line at `lineTop`: the middle of its capitals.
int rowMiddle(const GfxRenderer& renderer, int fontId, int lineTop);
// The icon at the frame's left edge, centred on `middle`.
void drawRowIcon(const GfxRenderer& renderer, const Frame& f, const freeink::Icon& icon, int middle);
// The selection bar beside a row spanning [top, bottom), padding included.
void drawRowSelection(const GfxRenderer& renderer, const Frame& f, int top, int bottom);
// Tab names where Today has its date line, the active one bold and underlined. One name is a title.
void drawTabs(const GfxRenderer& renderer, const Frame& f, const char* const* names, int count, int active);
// One single-line row at `top` in `fontId`, whose lines are `lineHeight` apart. Returns the y below it.
int drawMenuRow(const GfxRenderer& renderer, const Frame& f, int fontId, int lineHeight, int top, const MenuRow& row,
                bool selected);

// Displays the BW frame already drawn and, when text anti-aliasing is on, adds
// the gray pass drawn by `drawGray(ctx)`. drawGray must draw only text on white.
// Uses FAST refreshes with a HALF refresh every N, like the reader.
using GrayFn = void (*)(void* ctx);
// Whether displayWithAntiAliasing() will run a gray pass, so a screen can leave its grays to it.
bool grayPassRuns(const GfxRenderer& renderer);
void displayWithAntiAliasing(GfxRenderer& renderer, int& pagesUntilFullRefresh, GrayFn drawGray, void* ctx);

}  // namespace x4::ui
