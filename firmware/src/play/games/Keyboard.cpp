#include "Keyboard.h"

#include <I18n.h>

#include <algorithm>
#include <cstdlib>

#include "learn/ui/LearnUi.h"
#include "play/ui/PlayUi.h"

namespace x4::play::words {

namespace {

constexpr int RADIUS = 6;
constexpr int KEY_GAP = 8;

void drawLetter(const GfxRenderer& r, const Box& box, const Letter letter, const bool black, const int heightPct) {
  if (!letter) return;
  char text[4];
  const Letter up = upper(letter);
  encode(&up, 1, text, sizeof(text));
  const int scale = ui::scaleForHeight(r, box.height * heightPct / 100);
  const int baseline = box.y + (box.height + ui::glyphHeight(r, scale)) / 2;
  ui::drawBigText(r, text, box.x + box.width / 2, baseline, scale, EpdFontFamily::BOLD, black);
}

void drawDeleteIcon(const GfxRenderer& r, const Box& b) {
  const int h = std::min(b.height, b.width) / 2;
  const int w = h * 3 / 2;
  const int x0 = b.x + (b.width - w) / 2;
  const int x1 = x0 + w;
  const int top = b.y + (b.height - h) / 2;
  const int bottom = top + h;
  const int cy = top + h / 2;
  const int tip = x0 + h / 2;
  r.drawLine(x0, cy, tip, top, 2, true);
  r.drawLine(x0, cy, tip, bottom, 2, true);
  r.drawLine(tip, top, x1, top, 2, true);
  r.drawLine(tip, bottom, x1, bottom, 2, true);
  r.drawLine(x1, top, x1, bottom, 2, true);
  const int cx = (tip + x1) / 2 + 1;
  const int arm = h / 5;
  r.drawLine(cx - arm, cy - arm, cx + arm, cy + arm, 2, true);
  r.drawLine(cx - arm, cy + arm, cx + arm, cy - arm, 2, true);
}

void drawNextIcon(const GfxRenderer& r, const Box& b) {
  const int w = std::min(b.height, b.width) / 2;
  const int x0 = b.x + (b.width - w) / 2;
  const int x1 = x0 + w;
  const int cy = b.y + b.height / 2;
  const int arm = w / 3;
  r.drawLine(x0, cy, x1, cy, 3, true);
  r.drawLine(x1 - arm, cy - arm, x1, cy, 3, true);
  r.drawLine(x1 - arm, cy + arm, x1, cy, 3, true);
}

}  // namespace

void drawTile(const GfxRenderer& r, const Box& b, const Letter letter, const Tile tile) {
  switch (tile) {
    case Tile::Empty:
      r.drawRoundedRect(b.x, b.y, b.width, b.height, 2, RADIUS, true);
      return;
    case Tile::Typed:
      r.drawRoundedRect(b.x, b.y, b.width, b.height, 2, RADIUS, true);
      drawLetter(r, b, letter, true, 48);
      return;
    case Tile::Absent:
      drawLetter(r, b, letter, true, 48);
      return;
    case Tile::Present:
      r.drawRoundedRect(b.x, b.y, b.width, b.height, 5, RADIUS, true);
      drawLetter(r, b, letter, true, 48);
      return;
    case Tile::Correct:
      r.fillRoundedRect(b.x, b.y, b.width, b.height, RADIUS, Color::Black);
      drawLetter(r, b, letter, false, 48);
      return;
    case Tile::Reveal:
      r.drawRoundedRect(b.x, b.y, b.width, b.height, 2, RADIUS, true);
      drawLetter(r, b, letter, true, 48);
      ui::lighten(r, b.x + 4, b.y + 4, b.width - 8, b.height - 8);
      return;
  }
}

void strikeThrough(const GfxRenderer& r, const Box& first, const int count, const int pitch) {
  const int y = first.y + first.height / 2;
  r.fillRect(first.x - 6, y - 2, pitch * (count - 1) + first.width + 12, 4, true);
}

void Keyboard::setLang(const Lang lang) {
  count = alphabet(lang, &letters);
  if (at >= total()) at = 0;
}

void Keyboard::place(const uint8_t key, int& row, int& col) const {
  if (key < count) {
    row = key / COLUMNS;
    col = key % COLUMNS;
    return;
  }
  const int last = (count - 1) / COLUMNS;
  const int used = count - last * COLUMNS;
  row = used <= COLUMNS - 2 ? last : last + 1;
  col = COLUMNS - (key == count ? 2 : 1);
}

uint8_t Keyboard::rows() const {
  int row = 0;
  int col = 0;
  place(static_cast<uint8_t>(count + 1), row, col);
  return static_cast<uint8_t>(row + 1);
}

bool Keyboard::move(const Keys& keys, const bool wrapUp) {
  if (keys.left || keys.right) {
    at = static_cast<uint8_t>((at + (keys.left ? total() - 1 : 1)) % total());
    return true;
  }
  if (!keys.up && !keys.down) return false;
  int row = 0;
  int col = 0;
  place(at, row, col);
  int target = row + (keys.up ? -1 : 1);
  if (target < 0) {
    if (!wrapUp) return false;
    target = rows() - 1;
  }
  if (target >= rows()) target = 0;
  int best = -1;
  int bestDistance = COLUMNS + 1;
  for (uint8_t k = 0; k < total(); ++k) {
    int r = 0;
    int c = 0;
    place(k, r, c);
    if (r == target && std::abs(c - col) < bestDistance) {
      best = k;
      bestDistance = std::abs(c - col);
    }
  }
  if (best >= 0) at = static_cast<uint8_t>(best);
  return true;
}

int Keyboard::key() const {
  if (at < count) return at;
  return at == count ? DELETE : ENTER;
}

int Keyboard::indexOf(const Letter letter) const {
  const Letter* end = letters + count;
  const Letter* found = std::find(letters, end, letter);
  return found == end ? -1 : static_cast<int>(found - letters);
}

Letter Keyboard::letter() const { return at < count ? letters[at] : 0; }

int Keyboard::height(const int width) const { return rows() * (width / COLUMNS * 6 / 5); }

void Keyboard::draw(const GfxRenderer& r, const Box& area, const Mark* marks, const int pen,
                    const EnterIcon enter) const {
  const int cellWidth = area.width / COLUMNS;
  const int cellHeight = cellWidth * 6 / 5;
  const int left = area.x + (area.width - cellWidth * COLUMNS) / 2;
  for (uint8_t k = 0; k < total(); ++k) {
    int row = 0;
    int col = 0;
    place(k, row, col);
    const Box b{left + col * cellWidth + KEY_GAP / 2, area.y + row * cellHeight + KEY_GAP / 2, cellWidth - KEY_GAP,
                cellHeight - KEY_GAP};
    if (k == count) {
      drawDeleteIcon(r, b);
    } else if (k == count + 1) {
      if (enter == EnterIcon::Check) {
        const int size = std::min(b.width, b.height) * 3 / 5;
        x4::ui::drawCheck(r, b.x + (b.width - size) / 2, b.y + (b.height - size) / 2, size);
      } else {
        drawNextIcon(r, b);
      }
    } else {
      const Mark mark = marks ? marks[k] : Mark::None;
      if (mark == Mark::Correct) {
        r.fillRoundedRect(b.x, b.y, b.width, b.height, RADIUS, Color::Black);
        drawLetter(r, b, letters[k], false, 40);
      } else {
        if (mark == Mark::Present) r.drawRoundedRect(b.x, b.y, b.width, b.height, 3, RADIUS, true);
        drawLetter(r, b, letters[k], true, 40);
        if (mark == Mark::Absent) ui::lighten(r, b.x, b.y, b.width, b.height);
      }
    }
    if (pen > 0 && k == at) ui::drawCursor(r, b.x - 4, b.y - 4, b.width + 8, b.height + 8, pen);
  }
}

}  // namespace x4::play::words
