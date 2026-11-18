// Logic grid: the classic triangle of category pairs, the clues under it.
// The columns are the positions and the categories up to the second last,
// headed by initials; the rows are the last category down to the first after
// the positions, named in full.
// Arrows move across the grid; Confirm cycles a cell through no, yes and blank.
// Down past a column's end goes to the clues, where the arrows pick one and
// Confirm crosses it off; Up past the first clue or Back returns to the grid.
// The puzzle is won when the yes marks, taken together, place every item
// correctly: a yes between two items joins them, and every item must be joined
// to its position.
#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "Einstein.h"
#include "fontIds.h"
#include "play/Game.h"
#include "play/Play.h"
#include "play/ui/PlayUi.h"

namespace x4::play {

namespace {

using einstein::catOf;
using einstein::CATS_MAX;
using einstein::Clue;
using einstein::itemOf;
using einstein::ITEMS_MAX;
using einstein::POSITION;
using einstein::Ref;

constexpr uint8_t STATE_VERSION = 1;
constexpr int SIDE_MAX = (CATS_MAX - 1) * ITEMS_MAX;
constexpr int NAME_SIZE = 24;
constexpr int CLUE_SIZE = 96;

// Grid labels: the largest of these whose lines fit the cells.
constexpr int LABEL_FONTS[] = {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID};
constexpr int LABEL_OVERLAP = 4;  // a label line may be this much taller than its cell
constexpr int CLUE_FONT = NOTOSANS_12_FONT_ID;
constexpr int MARGIN = 20;
constexpr int CELL_MAX = 40;
constexpr int LABEL_GAP = 8;
constexpr int HEAD_GAP = 6;  // column heads to the grid
constexpr int GAP = 18;
constexpr int CLUE_PAD = 10;  // inside a clue row, around its text

enum Mark : uint8_t { Blank, No, Yes };

// The saved game, as-is.
struct State {
  uint8_t version;
  uint8_t row;  // grid cursor
  uint8_t col;
  uint8_t onClues;  // the arrows move through the clues, not the grid
  uint8_t clue;
  uint8_t failed;  // no puzzle could be made
  uint8_t reserved[2];
  uint32_t crossed;  // clues crossed off, bit per clue
  einstein::Puzzle puzzle;
  uint8_t marks[SIDE_MAX][SIDE_MAX];
};
static_assert(sizeof(State) <= SAVE_MAX, "State fits a saved game");
static_assert(einstein::CLUES_MAX <= 32, "crossed holds a bit per clue");

// The name sets as comma lists, in the order of einstein's set index.
constexpr StrId NAME_SETS[einstein::SETS] = {StrId::STR_EINSTEIN_COLORS, StrId::STR_EINSTEIN_PETS,
                                             StrId::STR_EINSTEIN_DRINKS, StrId::STR_EINSTEIN_PEOPLE};

class EinsteinGame final : public Game {
 public:
  void start(const uint32_t seed, const uint8_t level) override {
    s = State{};
    s.version = STATE_VERSION;
    auto work = makeUniqueNoThrow<einstein::Work>();
    if (!work) {
      LOG_ERR("PLAY", "OOM: logic grid generator (%u bytes)", static_cast<unsigned>(sizeof(einstein::Work)));
      s.failed = 1;
      return;
    }
    const uint32_t began = millis();
    einstein::generate(seed, level, s.puzzle, *work, [] { delay(1); });
    LOG_INF("PLAY", "Logic grid level %u: %ux%u, tier %d, %u steps, %u clues, %lu ms", level, s.puzzle.cats,
            s.puzzle.items, s.puzzle.tier, s.puzzle.steps, s.puzzle.clueCount,
            static_cast<unsigned long>(millis() - began));
    if (s.puzzle.tier == einstein::Unsolved) {
      LOG_ERR("PLAY", "No logic grid for seed %lu", static_cast<unsigned long>(seed));
      s.failed = 1;
    }
  }

  size_t save(uint8_t* out) const override {
    std::memcpy(out, &s, sizeof(s));
    return sizeof(s);
  }

  bool restore(const uint8_t* in, const size_t size) override {
    if (size != sizeof(State)) return false;
    std::memcpy(&s, in, sizeof(s));
    const einstein::Puzzle& p = s.puzzle;
    if (s.version != STATE_VERSION || p.cats < 3 || p.cats > CATS_MAX || p.items < 3 || p.items > ITEMS_MAX ||
        p.clueCount == 0 || p.clueCount > einstein::CLUES_MAX || s.clue >= p.clueCount) {
      return false;
    }
    for (int c = 0; c < p.cats; ++c) {
      if (p.set[c] >= einstein::SETS) return false;
      for (int i = 0; i < p.items; ++i) {
        if (p.at[c][i] >= p.items || p.name[c][i] >= ITEMS_MAX) return false;
      }
    }
    for (int k = 0; k < p.clueCount; ++k) {
      const Clue& c = p.clues[k];
      if (c.kind >= einstein::KINDS || !valid(c.a) || !valid(c.b) || (c.kind == einstein::Either && !valid(c.c))) {
        return false;
      }
    }
    for (const auto& line : s.marks) {
      for (const uint8_t m : line) {
        if (m > Yes) return false;
      }
    }
    return cell(s.row, s.col);
  }

  bool input(const Keys& keys, uint32_t) override {
    const int side = this->side();
    if (side == 0) return false;
    if (s.onClues) return clueInput(keys);
    if (keys.up) {
      if (s.row == 0) return false;
      --s.row;
      return true;
    }
    if (keys.down) {
      if (cell(s.row + 1, s.col)) {
        ++s.row;
      } else {
        s.onClues = 1;
      }
      return true;
    }
    if (keys.left || keys.right) {
      int end = 0;  // the row's last column + 1
      while (end < side && cell(s.row, end)) ++end;
      s.col = static_cast<uint8_t>((s.col + (keys.right ? 1 : end - 1)) % end);
      return true;
    }
    if (!keys.confirm) return false;
    uint8_t& m = s.marks[s.row][s.col];
    m = m == Blank ? No : m == No ? Yes : Blank;
    return true;
  }

  bool back() override {
    if (!s.onClues) return false;
    s.onClues = 0;
    return true;
  }

  void draw(const GfxRenderer& r, const x4::ui::Frame& f) const override {
    const einstein::Puzzle& p = s.puzzle;
    const int n = p.items;
    const int side = this->side();
    if (side == 0) return;  // no puzzle: the game is already lost
    const int width = r.getScreenWidth();

    // Row labels hold the names in full, right-aligned before the grid; the
    // cells take the rest of the width.
    char text[CLUE_SIZE];
    int font = SMALL_FONT_ID;
    int labelWidth = 0;
    int cellSize = 0;
    for (const int candidate : LABEL_FONTS) {
      font = candidate;
      labelWidth = LABEL_GAP;
      for (int c = 1; c < p.cats; ++c) {
        for (int i = 0; i < n; ++i) {
          name(c, i, text, sizeof(text));
          labelWidth = std::max(labelWidth, r.getTextWidth(font, text, EpdFontFamily::BOLD) + LABEL_GAP);
        }
      }
      cellSize = std::min(CELL_MAX, (width - 2 * MARGIN - labelWidth) / side);
      if (r.getLineHeight(font) <= cellSize + LABEL_OVERLAP) break;
    }
    const int gridSize = cellSize * side;
    const int x0 = (width - labelWidth - gridSize) / 2 + labelWidth;
    const int labelHeight = r.getLineHeight(font);
    const int ascent = r.getFontAscenderSize(font);
    // Column heads hold the names in full too, reading upwards to just above the grid.
    int headHeight = labelHeight;
    for (int k = 0; k < side; ++k) {
      name(k / n, k % n, text, sizeof(text));
      headHeight = std::max(headHeight, r.getTextWidth(font, text, EpdFontFamily::BOLD));
    }
    const int y0 = f.top - 20 + headHeight + HEAD_GAP;
    const bool onGrid = !s.onClues && outcome() == Outcome::Playing;

    // The cursor's column head and row label are bold and underlined.
    for (int k = 0; k < side; ++k) {
      // Column heads, turned to read upwards; the mark runs beside the name.
      name(k / n, k % n, text, sizeof(text));
      bool mark = onGrid && k == s.col;
      auto style = mark ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      int textWidth = r.getTextWidth(font, text, style);
      int x = x0 + k * cellSize + (cellSize - labelHeight) / 2;
      int y = y0 - HEAD_GAP;
      r.drawTextRotated90CW(font, x, y, text, true, style);
      if (mark) r.fillRect(x + labelHeight - ascent - 4, y - textWidth, 2, textWidth, true);
      // Row labels.
      name(p.cats - 1 - k / n, k % n, text, sizeof(text));
      mark = onGrid && k == s.row;
      style = mark ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      textWidth = r.getTextWidth(font, text, style);
      x = x0 - LABEL_GAP - textWidth;
      y = y0 + k * cellSize + (cellSize - labelHeight) / 2;
      r.drawText(font, x, y, text, true, style);
      if (mark) r.fillRect(x, y + ascent + 2, textWidth, 2, true);
    }

    // Blocks: thin lines inside, thick around.
    const int block = cellSize * n;
    for (int br = 0; br < p.cats - 1; ++br) {
      for (int bc = 0; bc + br < p.cats - 1; ++bc) {
        const int bx = x0 + bc * block;
        const int by = y0 + br * block;
        for (int k = 1; k < n; ++k) {
          r.fillRect(bx + k * cellSize, by, 1, block, true);
          r.fillRect(bx, by + k * cellSize, block, 1, true);
        }
        r.drawRect(bx - 1, by - 1, block + 2, block + 2, 2, true);
      }
    }

    // Marks. Once the yes marks claim a whole solution, the wrong ones are underlined.
    const bool claimed = verdict() == einstein::Verdict::Wrong;
    const int dot = std::max(4, cellSize * 2 / 5);
    const int cross = std::max(4, cellSize * 2 / 5);
    const int pen = cellSize >= 28 ? 2 : 1;
    for (int row = 0; row < side; ++row) {
      for (int col = 0; col < side && cell(row, col); ++col) {
        const int cx = x0 + col * cellSize;
        const int cy = y0 + row * cellSize;
        const uint8_t m = s.marks[row][col];
        if (m == Yes) {
          r.fillRoundedRect(cx + (cellSize - dot) / 2, cy + (cellSize - dot) / 2, dot, dot, dot / 2, Color::Black);
          if (claimed && !truePair(row, col)) {
            r.fillRect(cx + cellSize / 5, cy + cellSize - cellSize / 5, cellSize * 3 / 5, 2, true);
          }
        } else if (m == No) {
          const int ox = cx + (cellSize - cross) / 2;
          const int oy = cy + (cellSize - cross) / 2;
          r.drawLine(ox, oy, ox + cross, oy + cross, pen, true);
          r.drawLine(ox + cross, oy, ox, oy + cross, pen, true);
        }
      }
    }
    if (outcome() == Outcome::Playing) {
      const int cursorPen = s.onClues ? ui::REST_PEN : std::min(ui::FOCUS_PEN, std::max(2, cellSize / 7));
      ui::drawCursor(r, x0 + s.col * cellSize - 1, y0 + s.row * cellSize - 1, cellSize + 2, cellSize + 2, cursorPen);
    }

    drawClues(r, f, y0 + gridSize + GAP);
  }

  bool playable() const override { return !s.failed; }

  Outcome outcome() const override {
    return verdict() == einstein::Verdict::Right ? Outcome::Won : Outcome::Playing;
  }

 private:
  State s{};

  int side() const { return (s.puzzle.cats - 1) * s.puzzle.items; }

  // Whether (row, col) is a cell of the triangle: row block b holds category
  // cats-1-b, column block holds category col/items, and a block exists when
  // its column category is the lower.
  bool cell(const int row, const int col) const {
    const int n = s.puzzle.items;
    const int side = this->side();
    return row >= 0 && col >= 0 && row < side && col < side && row / n + col / n < s.puzzle.cats - 1;
  }

  Ref rowItem(const int row) const {
    return einstein::ref(s.puzzle.cats - 1 - row / s.puzzle.items, row % s.puzzle.items);
  }
  Ref colItem(const int col) const { return einstein::ref(col / s.puzzle.items, col % s.puzzle.items); }

  bool valid(const Ref x) const { return catOf(x) < s.puzzle.cats && itemOf(x) < s.puzzle.items; }

  uint8_t position(const Ref x) const { return s.puzzle.at[catOf(x)][itemOf(x)]; }

  bool truePair(const int row, const int col) const { return position(rowItem(row)) == position(colItem(col)); }

  einstein::Verdict verdict() const {
    einstein::Joins joins(s.puzzle);
    const int side = this->side();
    for (int row = 0; row < side; ++row) {
      for (int col = 0; col < side && cell(row, col); ++col) {
        if (s.marks[row][col] == Yes) joins.join(rowItem(row), colItem(col));
      }
    }
    return joins.verdict();
  }

  bool clueInput(const Keys& keys) {
    if (keys.up || keys.left) {
      if (s.clue == 0) {
        s.onClues = 0;
      } else {
        --s.clue;
      }
      return true;
    }
    if (keys.down || keys.right) {
      if (s.clue + 1 >= s.puzzle.clueCount) return false;
      ++s.clue;
      return true;
    }
    if (!keys.confirm) return false;
    s.crossed ^= 1u << s.clue;
    return true;
  }

  // Item i of category c, as shown: positions are numbers from 1.
  void name(const int c, const int i, char* out, const size_t size) const {
    if (c == POSITION) {
      snprintf(out, size, "%d", i + 1);
      return;
    }
    const char* list = I18n::getInstance().get(NAME_SETS[s.puzzle.set[c]]);
    for (int skip = s.puzzle.name[c][i]; skip > 0 && *list; ++list) {
      if (*list == ',') --skip;
    }
    size_t len = 0;
    while (list[len] && list[len] != ',' && len + 1 < size) ++len;
    std::memcpy(out, list, len);
    out[len] = '\0';
  }

  void clueText(const Clue& c, char* out, const size_t size) const {
    char a[NAME_SIZE];
    char b[NAME_SIZE];
    char third[NAME_SIZE];
    name(catOf(c.a), itemOf(c.a), a, sizeof(a));
    name(catOf(c.b), itemOf(c.b), b, sizeof(b));
    const bool at = catOf(c.b) == POSITION;
    switch (c.kind) {
      case einstein::Same:
        snprintf(out, size, at ? tr(STR_EINSTEIN_AT) : tr(STR_EINSTEIN_SAME), a, b);
        break;
      case einstein::Not:
        snprintf(out, size, at ? tr(STR_EINSTEIN_NOT_AT) : tr(STR_EINSTEIN_NOT), a, b);
        break;
      case einstein::NextTo:
        snprintf(out, size, tr(STR_EINSTEIN_NEXT), a, b);
        break;
      case einstein::LeftOf:
        snprintf(out, size, tr(STR_EINSTEIN_LEFT), a, b);
        break;
      case einstein::Before:
        snprintf(out, size, tr(STR_EINSTEIN_BEFORE), a, b);
        break;
      default:
        name(catOf(c.c), itemOf(c.c), third, sizeof(third));
        snprintf(out, size, at ? tr(STR_EINSTEIN_EITHER_AT) : tr(STR_EINSTEIN_EITHER), a, b, third);
        break;
    }
  }

  // The clues a page at a time, the page that holds the focused one; crossed-off ones in gray.
  // A clue wraps when it is wider than the screen.
  void drawClues(const GfxRenderer& r, const x4::ui::Frame& f, const int top) const {
    const einstein::Puzzle& p = s.puzzle;
    const int rowWidth = r.getScreenWidth() - 2 * MARGIN;
    const int textWidth = rowWidth - 2 * CLUE_PAD;
    const int lineHeight = r.getLineHeight(CLUE_FONT);
    char text[CLUE_SIZE];
    uint8_t lines[einstein::CLUES_MAX];
    uint8_t pageOf[einstein::CLUES_MAX];
    int pages = 1;
    int used = 0;
    for (int k = 0; k < p.clueCount; ++k) {
      clueText(p.clues[k], text, sizeof(text));
      lines[k] = static_cast<uint8_t>(
          std::max(1, x4::ui::flowText(r, CLUE_FONT, EpdFontFamily::REGULAR, text, textWidth, nullptr, nullptr)));
      const int height = lines[k] * lineHeight + CLUE_PAD;
      if (used > 0 && top + used + height > f.bottom) {
        ++pages;
        used = 0;
      }
      pageOf[k] = static_cast<uint8_t>(pages - 1);
      used += height;
    }
    const int page = pageOf[s.clue];
    int y = top;
    for (int k = 0; k < p.clueCount; ++k) {
      if (pageOf[k] != page) continue;
      const int height = lines[k] * lineHeight + CLUE_PAD;
      clueText(p.clues[k], text, sizeof(text));
      const x4::ui::TextBox box{
          CLUE_FONT, EpdFontFamily::REGULAR, MARGIN + CLUE_PAD, y + CLUE_PAD / 2, textWidth, lineHeight, y + height};
      x4::ui::drawTextBox(r, box, text);
      if (s.crossed & (1u << k)) ui::lighten(r, MARGIN, y, rowWidth, height);
      if (k == s.clue && outcome() == Outcome::Playing) {
        ui::drawCursor(r, MARGIN, y, rowWidth, height, s.onClues ? ui::FOCUS_PEN : ui::REST_PEN);
      }
      y += height;
    }
    if (pages > 1) x4::ui::drawProgressDots(r, page, pages, f.dotsY);
  }
};

}  // namespace

std::unique_ptr<Game> makeEinstein() { return std::unique_ptr<Game>(new (std::nothrow) EinsteinGame()); }

}  // namespace x4::play
