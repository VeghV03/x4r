#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "../Paper.h"

// The newspaper's two screens as drawings (the front page and a section). No input, no
// state: the firmware's activities and the Mac's simulator both draw through these.
namespace x4::paper {

// Left and right of every paper page.
inline constexpr int PAGE_MARGIN = 36;

// The faces the paper is set in.
struct Faces {
  int label;  // section names and outlets: the UI sans, bold capitals
  int small;  // the masthead's weather and event, a row's value
  int head;   // front-page headlines and the two lines under a headline: the serif at 12
  int title;  // the masthead's date, a section's name and its headlines: the serif at 14
};

struct FrontView {
  const Front* front = nullptr;
  const char* date = "";     // "Wed 11 Nov"; empty when the clock is unknown
  const char* agenda = "";   // "6° rain · 14:00 Design review"
  const char* play = "";     // the first daily puzzle not yet played
  const char* book = "";     // the book in progress
  const char* percent = "";  // "9%"
  uint8_t selected = 0;      // a row of paper::rows()
  bool masthead = false;     // the selection is on the masthead instead
};

struct SectionView {
  const char* name = "";
  const Piece* pieces = nullptr;
  uint8_t count = 0;
  uint8_t selected = 0;
  // Football's tabs (D44), its pieces the first; with them the masthead holds the tabs instead
  // of the name.
  const char* const* tabs = nullptr;
  uint8_t tabCount = 0;
};

// Each clears nothing and displays nothing: the caller owns the frame. `gray` is false in the
// black-and-white pass, where the rules, the selection bar and a read piece are drawn; in the
// scan and gray passes only the text that is smoothed is.
void drawFront(const GfxRenderer& renderer, const Faces& faces, const FrontView& view, bool gray);
void drawSection(const GfxRenderer& renderer, const Faces& faces, const SectionView& view, bool gray);

// The masthead as a row of tabs, `active` bold, and its rule; Football's pages share it. Returns
// the y below the rule, where a page's first row goes.
int drawTabs(const GfxRenderer& renderer, const Faces& faces, const char* const* names, uint8_t count, uint8_t active,
             bool gray);
// The same y, without drawing.
int belowTabs(const GfxRenderer& renderer, const Faces& faces);

// `text` in capitals, for the Latin letters the reading fonts carry (Hungarian included).
void capitals(const char* text, char* out, size_t size);

}  // namespace x4::paper
