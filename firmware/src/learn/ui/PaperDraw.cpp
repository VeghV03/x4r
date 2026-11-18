#include "PaperDraw.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace x4::paper {

namespace {

constexpr int MARGIN = PAGE_MARGIN;
constexpr int BOTTOM = 24;
// A reading face's line box is roomy; a headline and the lines under it sit closer.
constexpr int HEAD_TIGHTER = 6;
constexpr int TITLE_TIGHTER = 7;
constexpr int MAST_TOP = 26;
constexpr int BAR_GAP = 14;  // the selection bar sits this far left of the text
constexpr int BAR_WIDTH = 4;
constexpr int8_t LABEL_TRACKING = 2;
constexpr int HEAD_LINES = 3;
constexpr int LEAD_LINES = 2;
constexpr EpdFontFamily::Style BOLD = EpdFontFamily::BOLD;

// A thick rule with a hairline under it, as under a newspaper's masthead. Returns the y below.
int doubleRule(const GfxRenderer& r, const int y) {
  const int width = r.getScreenWidth() - 2 * MARGIN;
  r.fillRect(MARGIN, y, width, 3);
  r.fillRect(MARGIN, y + 6, width, 1);
  return y + 7;
}

// A hairline of every other pixel: lighter than a rule on a panel without grays.
void hairline(const GfxRenderer& r, const int y) {
  for (int x = MARGIN; x < r.getScreenWidth() - MARGIN; x += 2) r.drawPixel(x, y);
}

void bar(const GfxRenderer& r, const int top, const int bottom) {
  r.fillRect(MARGIN - BAR_GAP, top, BAR_WIDTH, bottom - top);
}

// Takes every other pixel out of a band, so what was drawn there reads as gray.
void dim(const GfxRenderer& r, const int top, const int bottom) {
  for (int y = top; y < bottom; ++y) {
    for (int x = MARGIN + ((y + MARGIN) & 1); x < r.getScreenWidth() - MARGIN; x += 2) r.drawPixel(x, y, false);
  }
}

// The mark of a kept piece: a small ribbon at the right margin.
void bookmark(const GfxRenderer& r, const int y) {
  constexpr int W = 10;
  constexpr int H = 15;
  const int x = r.getScreenWidth() - MARGIN - W;
  r.fillRect(x, y, W, H - W / 2);
  for (int i = 0; i < W / 2; ++i) {
    // The notch: two legs that narrow to the tips.
    r.fillRect(x, y + H - W / 2 + i, W / 2 - i, 1);
    r.fillRect(x + W / 2 + i, y + H - W / 2 + i, W / 2 - i, 1);
  }
}

void label(const GfxRenderer& r, const Faces& f, const int y, const char* text) {
  char upper[64];
  capitals(text, upper, sizeof(upper));
  r.drawText(f.label, MARGIN, y, upper, true, BOLD, BidiUtils::BidiBaseDir::AUTO, LABEL_TRACKING);
}

void rightText(const GfxRenderer& r, const int font, const int y, const char* text) {
  if (!*text) return;
  r.drawText(font, r.getScreenWidth() - MARGIN - r.getTextWidth(font, text), y, text);
}

// The masthead line and its rule. Returns the y below the rule.
int masthead(const GfxRenderer& r, const Faces& f, const char* left, const char* right, const bool gray) {
  const int width = r.getScreenWidth() - 2 * MARGIN;
  int used = 0;
  if (*left) {
    r.drawText(f.title, MARGIN, MAST_TOP, left, true, BOLD);
    used = r.getTextWidth(f.title, left, BOLD) + BAR_GAP;
  }
  if (*right && used < width) {
    // On the date's baseline, cut to what the line has left.
    const int y = MAST_TOP + r.getFontAscenderSize(f.title) - r.getFontAscenderSize(f.small);
    rightText(r, f.small, y, r.truncatedText(f.small, right, width - used).c_str());
  }
  const int rule = MAST_TOP + r.getLineHeight(f.title) + 6;
  return gray ? rule + 7 : doubleRule(r, rule);
}

}  // namespace

void capitals(const char* text, char* out, const size_t size) {
  size_t n = 0;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p && n + 1 < size; ++p) {
    uint8_t c = *p;
    if (c >= 'a' && c <= 'z') {
      c -= 32;
    } else if (c == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7 && n + 2 < size) {
      out[n++] = static_cast<char>(c);  // à..þ to À..Þ
      c = *++p - 0x20;
    } else if (c == 0xC5 && (p[1] == 0x91 || p[1] == 0xB1) && n + 2 < size) {
      out[n++] = static_cast<char>(c);  // ő, ű
      c = *++p - 1;
    }
    out[n++] = static_cast<char>(c);
  }
  out[n] = '\0';
}

void drawFront(const GfxRenderer& r, const Faces& f, const FrontView& view, const bool gray) {
  if (!view.front) return;
  const Front& front = *view.front;
  const int width = r.getScreenWidth() - 2 * MARGIN;
  const int labelHeight = r.getLineHeight(f.label);
  const int headHeight = r.getLineHeight(f.head) - HEAD_TIGHTER;
  const uint8_t lined = front.count + 2;  // the sections, Play and Book: a name and a line each
  const int top = masthead(r, f, view.date, view.agenda, gray) + 4;
  if (view.masthead && !gray) bar(r, MAST_TOP + 4, MAST_TOP + r.getLineHeight(f.title));
  // The rows share what the page has, so seven sections and eight both fill it.
  const int room = r.getScreenHeight() - BOTTOM - top - labelHeight;
  const int pad = std::clamp((room - lined * (labelHeight + headHeight)) / (lined + 1), 2, 14);

  int y = top;
  const uint8_t count = rows(front);
  for (uint8_t i = 0; i < count; ++i) {
    const bool section = i < front.count;
    const Fixed fixed = section ? Fixed::COUNT : static_cast<Fixed>(i - front.count);
    const char* name = section ? front.sections[i].name : fixed == Fixed::Play ? "Play" : fixed == Fixed::Book ? "Book" : "Library";
    const char* line = section ? front.sections[i].head : fixed == Fixed::Play ? view.play : fixed == Fixed::Book ? view.book : nullptr;
    const int rowTop = y;
    y += pad / 2;
    label(r, f, y, name);
    if (fixed == Fixed::Book) rightText(r, f.small, y + (labelHeight - r.getLineHeight(f.small)) / 2, view.percent);
    y += labelHeight;
    if (line) {
      // An empty line keeps its place, so the rows below never move.
      if (*line) r.drawText(f.head, MARGIN, y - HEAD_TIGHTER / 2, r.truncatedText(f.head, line, width).c_str());
      y += headHeight;
    }
    y += pad - pad / 2;
    if (gray) continue;
    if (i == view.selected && !view.masthead) bar(r, rowTop + pad / 2 + 2, y - (pad - pad / 2));
    if (i + 1 < count) hairline(r, y);
  }
}

int drawTabs(const GfxRenderer& r, const Faces& f, const char* const* names, const uint8_t count, const uint8_t active,
             const bool gray) {
  constexpr int GAP = 22;      // between two names
  constexpr int MIN_GAP = 10;  // when the names would run past the margins
  const int width = r.getScreenWidth() - 2 * MARGIN;
  // Six names fit only a size below a section's name (D48), set on the same baseline.
  const int font = f.head;
  const int top = MAST_TOP + r.getFontAscenderSize(f.title) - r.getFontAscenderSize(font);
  // Measured bold, so the names stay put whichever is active.
  int namesWidth = 0;
  for (uint8_t t = 0; t < count; ++t) namesWidth += r.getTextWidth(font, names[t], BOLD);
  const int gap = count > 1 ? std::clamp((width - namesWidth) / (count - 1), MIN_GAP, GAP) : GAP;
  int x = MARGIN;
  for (uint8_t t = 0; t < count; ++t) {
    r.drawText(font, x, top, names[t], true, t == active ? BOLD : EpdFontFamily::REGULAR);
    x += r.getTextWidth(font, names[t], BOLD) + gap;
  }
  const int rule = MAST_TOP + r.getLineHeight(f.title) + 6;
  return (gray ? rule + 7 : doubleRule(r, rule)) + 8;
}

int belowTabs(const GfxRenderer& r, const Faces& f) { return MAST_TOP + r.getLineHeight(f.title) + 6 + 7 + 8; }

void drawSection(const GfxRenderer& r, const Faces& f, const SectionView& view, const bool gray) {
  const int width = r.getScreenWidth() - 2 * MARGIN;
  const int kickHeight = r.getLineHeight(f.label);
  const int titleHeight = r.getLineHeight(f.title) - TITLE_TIGHTER;
  const int leadHeight = r.getLineHeight(f.head) - HEAD_TIGHTER;
  int y = view.tabCount ? drawTabs(r, f, view.tabs, view.tabCount, 0, gray) : masthead(r, f, view.name, "", gray) + 8;
  for (uint8_t i = 0; i < view.count; ++i) {
    const Piece& piece = view.pieces[i];
    const int top = y;
    // A read piece is drawn once, in black and white, and thinned to gray: no smoothing over it.
    const bool ink = !(piece.read && gray);
    if (ink) label(r, f, y, piece.source);
    if (piece.kept && !gray) bookmark(r, y + 4);
    y += kickHeight;
    for (const std::string& line : r.wrappedText(f.title, piece.title, width, HEAD_LINES, BOLD)) {
      if (ink) r.drawText(f.title, MARGIN, y - TITLE_TIGHTER / 2, line.c_str(), true, BOLD);
      y += titleHeight;
    }
    y += 2;
    for (const std::string& line : r.wrappedText(f.head, piece.lead, width, LEAD_LINES)) {
      if (ink) r.drawText(f.head, MARGIN, y - HEAD_TIGHTER / 2, line.c_str());
      y += leadHeight;
    }
    y += 4;
    if (!gray) {
      if (piece.read) dim(r, top, y);
      if (i == view.selected) bar(r, top + 4, y);
    }
    y += 8;
    if (!gray && i + 1 < view.count) hairline(r, y);
    y += 8;
  }
}

}  // namespace x4::paper
