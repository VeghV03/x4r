// A test card drawn by the firmware's renderer and saved as a picture:
//   x4draw --fonts build/fonts --out card.pgm [--gray]
#include <cstdio>
#include <cstring>
#include <string>

#include "HostGfx.h"
#include "src/fontIds.h"

namespace {

constexpr int MARGIN = 24;
constexpr char MEASURED[] = "Who supervises the Sky Eye?";

struct Card {
  int body = 0;      // Literata 12
  int headline = 0;  // Literata 14
};

// `gray` as in a screen's drawScreen(gray): the shapes and the white label belong to the
// black-and-white pass only.
void drawCard(const GfxRenderer& renderer, const Card& card, const bool gray) {
  const int width = renderer.getScreenWidth() - 2 * MARGIN;
  int y = 40;

  renderer.drawText(UI_10_FONT_ID, MARGIN, y, "HUNGARY", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 6;

  // A 3 px rule with a hairline 4 px under it.
  if (!gray) {
    renderer.fillRect(MARGIN, y, width, 3);
    renderer.drawLine(MARGIN, y + 3 + 4, MARGIN + width - 1, y + 3 + 4);
  }
  y += 3 + 4 + 1 + 14;

  renderer.drawText(card.headline, MARGIN, y, "Ki felügyeli az Égi Szemet?", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(card.headline) + 10;

  const int bodyLine = renderer.getLineHeight(card.body);
  for (const char* line : {"Orbán Áron-ügy: letartóztatás", "Árvíztűrő tükörfúrógép", MEASURED}) {
    renderer.drawText(card.body, MARGIN, y, line);
    y += bodyLine;
  }
  // As long as the renderer says the last line is.
  if (!gray) renderer.drawLine(MARGIN, y + 2, MARGIN + renderer.getTextWidth(card.body, MEASURED) - 1, y + 2);
  y += 20;

  renderer.drawText(card.body, MARGIN, y, "Italic: árvíztűrő tükörfúrógép", true, EpdFontFamily::ITALIC);
  y += bodyLine + 16;

  if (gray) return;
  const char* label = "X4";
  const int boxHeight = renderer.getLineHeight(UI_12_FONT_ID) + 16;
  const int boxWidth = renderer.getTextWidth(UI_12_FONT_ID, label, EpdFontFamily::BOLD) + 32;
  renderer.fillRect(MARGIN, y, boxWidth, boxHeight);
  renderer.drawText(UI_12_FONT_ID, MARGIN + 16, y + 8, label, false, EpdFontFamily::BOLD);
}

int usage() {
  fprintf(stderr, "usage: x4draw --fonts <folder> --out <file.pgm> [--gray]\n");
  return 2;
}

}  // namespace

int main(const int argc, char** argv) {
  std::string fonts;
  std::string out;
  bool gray = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--fonts") && i + 1 < argc) {
      fonts = argv[++i];
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      out = argv[++i];
    } else if (!strcmp(argv[i], "--gray")) {
      gray = true;
    } else {
      return usage();
    }
  }
  if (fonts.empty() || out.empty()) return usage();

  HostGfx gfx;
  GfxRenderer& renderer = gfx.renderer();
  Card card;
  card.body = gfx.loadSdFamily(fonts, "Literata", 12);
  card.headline = gfx.loadSdFamily(fonts, "Literata", 14);
  if (!card.body || !card.headline) return 1;

  // Before any render the glyphs load one by one without kerning; after it they are resident.
  const int coldWidth = renderer.getTextWidth(card.body, MEASURED);
  gfx.render([&](const bool grayPass) { drawCard(renderer, card, grayPass); }, gray);
  if (!gfx.savePgm(out)) {
    fprintf(stderr, "x4draw: cannot write %s\n", out.c_str());
    return 1;
  }

  printf("screen %d x %d\n", renderer.getScreenWidth(), renderer.getScreenHeight());
  printf("line height: UI_10 %d, Literata 12 %d, Literata 14 %d\n", renderer.getLineHeight(UI_10_FONT_ID),
         renderer.getLineHeight(card.body), renderer.getLineHeight(card.headline));
  printf("\"%s\" in Literata 12: ink width %d (before the first render: %d), pen advance %d\n", MEASURED,
         renderer.getTextWidth(card.body, MEASURED), coldWidth,
         renderer.getTextAdvanceX(card.body, MEASURED, EpdFontFamily::REGULAR, 0, BidiUtils::BidiBaseDir::AUTO,
                                  GfxRenderer::TextMeasureMode::Rendered));
  printf("wrote %s%s\n", out.c_str(), gray ? " (with the gray pass)" : "");
  return 0;
}
