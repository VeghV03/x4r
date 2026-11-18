#include "LearnUi.h"

#include <Icon.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace x4::ui {

bool anyPress(const MappedInputManager& input) {
  using Button = MappedInputManager::Button;
  for (const Button b : {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down}) {
    if (input.wasPressed(b)) return true;
  }
  return false;
}

namespace {

// Longest line kept while wrapping. A line is at most one screen wide, so this
// only clips pathological unbroken runs.
constexpr size_t LINE_BYTES = 480;

size_t utf8CharLength(const char* p) {
  const auto c = static_cast<unsigned char>(*p);
  size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
  for (size_t i = 1; i < n; ++i) {
    if ((static_cast<unsigned char>(p[i]) & 0xC0) != 0x80) return i;
  }
  return n;
}

// Words kept per line for a SpanFn; a line holding more leaves the rest out.
constexpr int SPANS_MAX = 40;

struct Wrapper {
  const GfxRenderer& renderer;
  int fontId;
  EpdFontFamily::Style style;
  int width;
  LineFn fn;
  void* ctx;
  char line[LINE_BYTES + 1];
  size_t length = 0;
  int count = 0;
  // With spanFn set: the current line's words and where in `origin` they came from.
  SpanFn spanFn = nullptr;
  const char* origin = nullptr;
  Span spans[SPANS_MAX];
  int spanCount = 0;
  int lastRight = 0;  // width of the line as last measured

  bool fits(const char* text) {
    const int w = renderer.getTextWidth(fontId, text, style);
    if (w > width) return false;
    lastRight = w;
    return true;
  }

  // Records [word, word + n) as ending at lastRight; `spaced` when a space precedes it.
  void addSpan(const char* word, const size_t n, const bool spaced) {
    if (!spanFn || spanCount >= SPANS_MAX) return;
    Span& s = spans[spanCount];
    s.source = static_cast<uint16_t>(word - origin);
    s.length = static_cast<uint16_t>(n);
    const int left = spanCount ? spans[spanCount - 1].right : 0;
    s.right = static_cast<int16_t>(lastRight);
    s.left = static_cast<int16_t>(std::min(lastRight, left + (spaced ? renderer.getSpaceWidth(fontId, style) : 0)));
    ++spanCount;
  }

  void emit() {
    line[length] = '\0';
    if (fn) fn(ctx, line, count);
    if (spanFn) spanFn(ctx, line, count, spans, spanCount);
    ++count;
    length = 0;
    spanCount = 0;
    lastRight = 0;
  }

  // Appends [word, word + n) with a leading space when the line is not empty.
  bool tryAppend(const char* word, const size_t n) {
    const size_t extra = (length ? 1 : 0) + n;
    if (length + extra > LINE_BYTES) return false;
    const size_t before = length;
    const int right = lastRight;
    if (length) line[length++] = ' ';
    std::memcpy(line + length, word, n);
    length += n;
    line[length] = '\0';
    if (fits(line)) {
      addSpan(word, n, before > 0);
      return true;
    }
    length = before;
    line[length] = '\0';
    lastRight = right;
    return false;
  }

  // A word wider than the line: as many characters as fit, then the rest.
  void breakWord(const char* word, size_t n) {
    while (n > 0) {
      size_t take = 0;
      const bool spaced = length > 0;
      while (take < n) {
        const size_t step = std::min(utf8CharLength(word + take), n - take);
        if (length + step > LINE_BYTES) break;
        std::memcpy(line + length, word + take, step);
        line[length + step] = '\0';
        if (!fits(line) && (length > 0 || take > 0)) break;
        length += step;
        take += step;
      }
      line[length] = '\0';
      if (take > 0) addSpan(word, take, spaced);
      word += take;
      n -= take;
      if (n > 0) emit();
    }
  }

  void addWord(const char* word, const size_t n) {
    if (tryAppend(word, n)) return;
    if (length) emit();
    if (!tryAppend(word, n)) breakWord(word, n);
  }

  void run(const char* text) {
    const char* p = text;
    while (*p) {
      // One paragraph line: up to the next newline.
      const char* end = std::strchr(p, '\n');
      if (!end) end = p + std::strlen(p);
      const char* q = p;
      bool any = false;
      while (q < end) {
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\r')) ++q;
        const char* w = q;
        while (q < end && *q != ' ' && *q != '\t' && *q != '\r') ++q;
        if (q > w) {
          addWord(w, static_cast<size_t>(q - w));
          any = true;
        }
      }
      if (any || length) emit();
      if (!*end) break;
      p = end + 1;
      // A blank line is a paragraph gap: one empty line, however many newlines.
      if (*p == '\n' || *p == '\r') {
        while (*p == '\n' || *p == '\r') ++p;
        if (*p) emit();
      }
    }
  }
};

struct BoxDraw {
  const GfxRenderer& renderer;
  const TextBox& box;
  int firstLine;
  int endLine;
  int y;
  int next;
  const Shading* shading = nullptr;
  bool started = false;
  bool full = false;
  // The last line, drawn once the next one shows whether text is cut after it: a line drawn
  // and then cleared for its ellipsis would take the descenders of the line above with it.
  char pending[256] = "";
  int pendingY = 0;
};

void flushPending(BoxDraw& d) {
  if (d.pending[0] == '\0') return;
  d.renderer.drawText(d.box.fontId, d.box.x, d.pendingY, d.pending, d.box.black, d.box.style);
  d.pending[0] = '\0';
}

// Text that does not fit ends the last line with an ellipsis, so a cut is never silent.
void endWithEllipsis(BoxDraw& d, const char* rest) {
  if (d.pending[0] == '\0') return;
  std::string joined = d.pending;
  joined += ' ';
  joined += rest;
  joined += "\xe2\x80\xa6";  // U+2026: shown even where the joined text happens to fit
  const std::string cut = d.renderer.truncatedText(d.box.fontId, joined.c_str(), d.box.width, d.box.style);
  d.renderer.drawText(d.box.fontId, d.box.x, d.pendingY, cut.c_str(), d.box.black, d.box.style);
  d.pending[0] = '\0';
}

void drawBoxLine(void* ctx, const char* line, const int index, const Span* spans, const int count) {
  auto& d = *static_cast<BoxDraw*>(ctx);
  if (index < d.firstLine || index >= d.endLine || d.full) return;
  if (*line == '\0' && !d.started) {
    d.next = index + 1;
    return;
  }
  if (d.y + d.box.lineHeight > d.box.bottom) {
    d.full = true;
    if (*line) endWithEllipsis(d, line);
    return;
  }
  flushPending(d);
  if (*line && d.shading) {
    // Shaded lines are paged, never cut: drawn at once over their ground.
    drawShading(d.renderer, d.box.x, d.y, d.box.lineHeight, spans, count, *d.shading);
    d.renderer.drawText(d.box.fontId, d.box.x, d.y, line, d.box.black, d.box.style);
  } else if (*line) {
    strncpy(d.pending, line, sizeof(d.pending) - 1);
    d.pendingY = d.y;
  }
  d.started = true;
  d.y += d.box.lineHeight;
  d.next = index + 1;
}

struct PageScan {
  const TextBox& box;
  int top;
  int y;
  Pages& pages;
  bool pageEmpty = true;
};

void scanPageLine(void* ctx, const char* line, const int index) {
  auto& s = *static_cast<PageScan*>(ctx);
  if (*line == '\0' && s.pageEmpty) return;  // skipped at a page start, as drawTextBox does
  if (s.y + s.box.lineHeight > s.box.bottom && !s.pageEmpty) {
    if (s.pages.count >= Pages::MAX) return;
    s.pages.start[s.pages.count++] = static_cast<uint16_t>(index);
    s.y = s.top;
    s.pageEmpty = true;
    if (*line == '\0') return;
  }
  s.y += s.box.lineHeight;
  s.pageEmpty = false;
}

}  // namespace

int flowText(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style, const char* text,
             const int width, LineFn fn, void* ctx) {
  if (!text || !*text) return 0;
  Wrapper w{renderer, fontId, style, width, fn, ctx, {}};
  w.run(text);
  return w.count;
}

int flowSpans(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style, const char* text,
              const int width, SpanFn fn, void* ctx) {
  if (!text || !*text) return 0;
  Wrapper w{renderer, fontId, style, width, nullptr, ctx, {}};
  w.spanFn = fn;
  w.origin = text;
  w.run(text);
  return w.count;
}

bool isShaded(const Shading& shading, const uint16_t start, const uint16_t end) {
  for (uint8_t i = 0; i < shading.count; ++i) {
    if (shading.shades[i].start < end && start < shading.shades[i].end) return true;
  }
  return false;
}

void drawShading(const GfxRenderer& renderer, const int x, const int y, const int height, const Span* spans,
                 const int count, const Shading& shading) {
  // Neighbouring shaded words share one ground, the space between them included.
  int runLeft = -1;
  int runRight = 0;
  for (int i = 0; i <= count; ++i) {
    if (i < count) {
      const uint16_t start = static_cast<uint16_t>(shading.base + spans[i].source);
      if (isShaded(shading, start, static_cast<uint16_t>(start + spans[i].length))) {
        if (runLeft < 0) runLeft = spans[i].left;
        runRight = spans[i].right;
        continue;
      }
    }
    if (runLeft >= 0 && runRight > runLeft) {
      renderer.fillRectDither(x + runLeft, y, runRight - runLeft, height, Color::LightGray);
    }
    runLeft = -1;
  }
}

int drawTextBox(const GfxRenderer& renderer, const TextBox& box, const char* text, const int firstLine,
                const int endLine, int* next, const Shading* shading) {
  if (renderer.isFontCacheScanning()) {
    // The scan only needs the text: wrapping it would measure every word on a cold glyph cache.
    if (text && *text) renderer.drawText(box.fontId, box.x, box.y, text, box.black, box.style);
    if (next) *next = firstLine;
    return box.y + box.lineHeight;
  }
  BoxDraw d{renderer, box, firstLine, endLine, box.y, firstLine};
  d.shading = shading && shading->count ? shading : nullptr;
  flowSpans(renderer, box.fontId, box.style, text, box.width, &drawBoxLine, &d);
  flushPending(d);
  if (next) *next = d.next;
  return d.y;
}

void paginate(const GfxRenderer& renderer, const TextBox& box, const int firstTop, const int top, const char* text,
              Pages& out) {
  out = Pages{};
  out.count = 1;  // start[0] = 0
  PageScan s{box, top, firstTop, out};
  const int lines = flowText(renderer, box.fontId, box.style, text, box.width, &scanPageLine, &s);
  out.start[out.count] = static_cast<uint16_t>(lines);
}

Frame frame(const GfxRenderer& renderer) {
  const Rect content = UITheme::getContentArea(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int side = std::max(36, content.width / 11);
  Frame f;
  f.left = content.x + side;
  f.width = content.width - 2 * side;
  f.top = content.y + std::max(48, content.height / 12);
  f.dotsY = content.y + content.height - metrics.buttonHintsHeight - 22;
  f.bottom = f.dotsY - 26;
  return f;
}

void drawProgressDots(const GfxRenderer& renderer, const int done, const int total, const int centreY) {
  if (total <= 0) return;
  const Rect content = UITheme::getContentArea(renderer);
  constexpr int DOT = 10;
  constexpr int STEP = 20;
  const int maxWidth = content.width - 2 * 36;
  const int width = (total - 1) * STEP + DOT;
  if (width > maxWidth) {
    // Too many steps for dots: a hairline with the done part thick.
    const int x = content.x + (content.width - maxWidth) / 2;
    renderer.drawLine(x, centreY, x + maxWidth, centreY, true);
    const int doneWidth = maxWidth * std::min(done, total) / total;
    if (doneWidth > 0) renderer.fillRect(x, centreY - 1, doneWidth, 3, true);
    return;
  }
  int x = content.x + (content.width - width) / 2;
  for (int i = 0; i < total; ++i, x += STEP) {
    const int y = centreY - DOT / 2;
    if (i < done) {
      renderer.fillRoundedRect(x, y, DOT, DOT, DOT / 2, Color::Black);
    } else if (i == done) {
      renderer.fillRoundedRect(x - 3, y - 3, DOT + 6, DOT + 6, (DOT + 6) / 2, Color::Black);
      renderer.fillRoundedRect(x, y, DOT, DOT, DOT / 2, Color::White);
    } else {
      renderer.drawRoundedRect(x, y, DOT, DOT, 1, DOT / 2, true);
    }
  }
}

namespace {
// The largest built-in face; the others are compiled out with OMIT_FONTS.
#ifndef OMIT_FONTS
constexpr int BIG_FONT_ID = NOTOSANS_18_FONT_ID;
#else
constexpr int BIG_FONT_ID = NOTOSANS_14_FONT_ID;
#endif
}  // namespace

BigFace bigFace(const GfxRenderer& renderer, const uint32_t cp, const int scale256) {
  constexpr int DIGITS_FROM = 384;  // 1.5x
  int32_t advance = 0;
  int top = 0;
  if (scale256 <= DIGITS_FROM ||
      !renderer.getCodepointMetrics(BIG_DIGITS_FONT_ID, cp, EpdFontFamily::BOLD, advance, top)) {
    return {BIG_FONT_ID, scale256};
  }
  // Both faces matched on the height of their zero.
  int baseTop = 0;
  int digitsTop = 0;
  if (!renderer.getCodepointMetrics(BIG_FONT_ID, '0', EpdFontFamily::BOLD, advance, baseTop) ||
      !renderer.getCodepointMetrics(BIG_DIGITS_FONT_ID, '0', EpdFontFamily::BOLD, advance, digitsTop) ||
      digitsTop <= 0) {
    return {BIG_FONT_ID, scale256};
  }
  return {BIG_DIGITS_FONT_ID, scale256 * baseTop / digitsTop};
}

int bigNumberWidth(const GfxRenderer& renderer, const uint32_t value, const int scale256) {
  char digits[12];
  snprintf(digits, sizeof(digits), "%u", static_cast<unsigned>(value));
  int width = 0;
  for (const char* p = digits; *p; ++p) {
    const BigFace face = bigFace(renderer, static_cast<uint32_t>(*p), scale256);
    int32_t advance = 0;
    int top = 0;
    if (renderer.getCodepointMetrics(face.fontId, static_cast<uint32_t>(*p), EpdFontFamily::BOLD, advance, top)) {
      width += static_cast<int>((advance * face.scale256 / 256 + 8) >> 4);
    }
  }
  return width;
}

int bigNumberHeight(const GfxRenderer& renderer, const int scale256) {
  int32_t advance = 0;
  int top = 0;
  if (!renderer.getCodepointMetrics(BIG_FONT_ID, '0', EpdFontFamily::BOLD, advance, top)) return 0;
  return top * scale256 / 256;
}

int drawBigNumber(const GfxRenderer& renderer, const uint32_t value, const int x, const int baseline,
                  const int scale256) {
  char digits[12];
  snprintf(digits, sizeof(digits), "%u", static_cast<unsigned>(value));
  int cx = x;
  for (const char* p = digits; *p; ++p) {
    const BigFace face = bigFace(renderer, static_cast<uint32_t>(*p), scale256);
    cx += renderer.drawScaledCodepoint(face.fontId, static_cast<uint32_t>(*p), EpdFontFamily::BOLD, cx, baseline,
                                       face.scale256);
  }
  return cx - x;
}

namespace {
// A line drawn with a square pen, so diagonals keep their weight
// (drawLine's width only thickens vertically).
void stroke(const GfxRenderer& renderer, const int x1, const int y1, const int x2, const int y2, const int pen) {
  for (int dy = 0; dy < pen; dy++) {
    for (int dx = 0; dx < pen; dx++) {
      renderer.drawLine(x1 + dx, y1 + dy, x2 + dx, y2 + dy, true);
    }
  }
}

int penFor(const int size) { return std::max(3, size / 7); }
}  // namespace

void drawCheck(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int pen = penFor(size);
  const int s = size - pen;  // the pen grows right and down
  stroke(renderer, x, y + s / 2, x + s * 3 / 8, y + s * 7 / 8, pen);
  stroke(renderer, x + s * 3 / 8, y + s * 7 / 8, x + s, y + s / 8, pen);
}

void drawCross(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int pen = penFor(size);
  const int s = size - pen;
  stroke(renderer, x + s / 8, y + s / 8, x + s * 7 / 8, y + s * 7 / 8, pen);
  stroke(renderer, x + s * 7 / 8, y + s / 8, x + s / 8, y + s * 7 / 8, pen);
}

void drawOutcome(const GfxRenderer& renderer, const Frame& f, const bool good) {
  constexpr int MARK = 56;
  const int top = f.top + (f.bottom - f.top - MARK) * 2 / 5;
  const int x = (renderer.getScreenWidth() - MARK) / 2;
  if (good) {
    drawCheck(renderer, x, top, MARK);
  } else {
    drawCross(renderer, x, top, MARK);
  }
}

int rowTextX(const Frame& f) { return f.left + ICON_SIZE + ICON_GAP; }

int rowLineTop(const int top, const int lineHeight) { return top + ROW_PAD + std::max(0, ICON_SIZE - lineHeight) / 2; }

int rowMiddle(const GfxRenderer& renderer, const int fontId, const int lineTop) {
  // Capitals are about 0.7 of the ascender; their middle sits 0.65 of it below the line top.
  return lineTop + renderer.getFontAscenderSize(fontId) * 65 / 100;
}

void drawRowIcon(const GfxRenderer& renderer, const Frame& f, const freeink::Icon& icon, const int middle) {
  const int top = middle - icon.h / 2;
  const int stride = (icon.w + 7) / 8;
  for (int y = 0; y < icon.h; ++y) {
    for (int x = 0; x < icon.w; ++x) {
      // Bit 0 is ink (freeink::Icon).
      if (!((icon.bits[y * stride + x / 8] >> (7 - x % 8)) & 1)) renderer.drawPixel(f.left + x, top + y, true);
    }
  }
}

void drawRowSelection(const GfxRenderer& renderer, const Frame& f, const int top, const int bottom) {
  renderer.fillRect(f.left - MARK_GAP, top + ROW_PAD / 2, 4, bottom - top - ROW_PAD, true);
}

void drawTabs(const GfxRenderer& renderer, const Frame& f, const char* const* names, const int count,
              const int active) {
  constexpr int GAP = 28;      // between two names
  constexpr int MIN_GAP = 12;  // when the names would run past the frame
  constexpr int RULE = 3;      // under the active name
  constexpr int ABOVE = 16;    // names to the first row, as Today's date line
  const int lh = renderer.getLineHeight(UI_12_FONT_ID);
  const int y = f.top - lh - ABOVE;
  // Measured bold, so the names stay put whichever is active.
  int namesWidth = 0;
  for (int t = 0; t < count; ++t) namesWidth += renderer.getTextWidth(UI_12_FONT_ID, names[t], EpdFontFamily::BOLD);
  const int gap = count > 1 ? std::clamp((f.width - namesWidth) / (count - 1), MIN_GAP, GAP) : GAP;
  int x = f.left;
  for (int t = 0; t < count; ++t) {
    const auto style = t == active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(UI_12_FONT_ID, x, y, names[t], true, style);
    const int width = renderer.getTextWidth(UI_12_FONT_ID, names[t], style);
    if (t == active) renderer.fillRect(x, y + lh, width, RULE, true);
    x += renderer.getTextWidth(UI_12_FONT_ID, names[t], EpdFontFamily::BOLD) + gap;
  }
}

int drawMenuRow(const GfxRenderer& renderer, const Frame& f, const int fontId, const int lineHeight, const int top,
                const MenuRow& row, const bool selected) {
  const int lineTop = row.plain ? top + ROW_PAD : rowLineTop(top, lineHeight);
  const int middle = rowMiddle(renderer, fontId, lineTop);
  const int right = f.left + f.width;
  if (row.icon && !row.plain) drawRowIcon(renderer, f, *row.icon, middle);
  // The label ends before whatever stands at the right, with an ellipsis if it must.
  int labelRight = row.check ? right - CHECK_SIZE - CHECK_GAP : right;
  if (*row.value && (row.checkColumn || !row.check)) {
    const int valueRight = row.checkColumn ? right - CHECK_SIZE - CHECK_GAP : right;
    const int valueX = valueRight - renderer.getTextWidth(fontId, row.value);
    renderer.drawText(fontId, valueX, lineTop, row.value);
    labelRight = valueX;
    if (*row.tag) {
      labelRight = valueX - TAG_GAP - renderer.getTextWidth(fontId, row.tag);
      renderer.drawText(fontId, labelRight, lineTop, row.tag);
    }
    labelRight -= TAG_GAP;
  }
  const int labelX = row.plain ? f.left : rowTextX(f);
  TextBox label{fontId, row.style, labelX, lineTop, std::max(1, labelRight - labelX), lineHeight, lineTop + lineHeight};
  drawTextBox(renderer, label, row.label);
  if (row.check) drawCheck(renderer, right - CHECK_SIZE, middle - CHECK_SIZE / 2, CHECK_SIZE);
  const int bottom = top + ROW_PAD + (row.plain ? lineHeight : std::max(lineHeight, ICON_SIZE)) + ROW_PAD;
  if (selected) drawRowSelection(renderer, f, top, bottom);
  return bottom;
}

bool grayPassRuns(const GfxRenderer& renderer) {
  return SETTINGS.textAntiAliasing && renderer.grayscaleCapabilities().supported();
}

void displayWithAntiAliasing(GfxRenderer& renderer, int& pagesUntilFullRefresh, GrayFn drawGray, void* ctx) {
  const auto grayscale = renderer.grayscaleCapabilities();
  if (!grayPassRuns(renderer) || !drawGray) {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    return;
  }
  const bool combined = grayscale.base == HalDisplay::GrayscaleBase::Combined;

  // The BW base, as the reader shows a text page.
  if (combined) {
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
  } else if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    renderer.preconditionGrayscale();
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh--;
  }

  if (!grayscale.stripUploads) {
    // Panels without strip uploads: the whole-frame pass (stores the BW frame).
    ReaderUtils::renderAntiAliased(renderer, [&] { drawGray(ctx); });
    return;
  }

  // Strip path: each plane goes to the panel in bands, so only one band of
  // scratch is held instead of a second framebuffer. 80 rows * 100 bytes = 8 KB
  // on the X4; freed before returning.
  constexpr int STRIP_ROWS = 80;
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();
  auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
  if (!scratch) {
    LOG_ERR("LEARN", "OOM: grayscale strip (%d bytes); card shown without AA", gwBytes * STRIP_ROWS);
    if (combined) renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }
  for (const bool lsb : {true, false}) {
    renderer.setRenderMode(lsb ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
    for (int y = 0; y < gh; y += STRIP_ROWS) {
      const int rows = std::min(STRIP_ROWS, gh - y);
      renderer.beginStripTarget(scratch.get(), y, rows);
      renderer.clearScreen(0x00);
      drawGray(ctx);
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsb, scratch.get(), y, rows);
    }
  }
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();
}

}  // namespace x4::ui
