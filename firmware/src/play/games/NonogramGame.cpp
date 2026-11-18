// Nonogram: arrows move, Confirm fills or clears a cell, holding Confirm marks
// it empty. A line whose fills match its clue has its clue grayed. Solved or
// given up, the picture stays on screen until Confirm.
#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "Nonogram.h"
#include "play/Game.h"
#include "play/Play.h"
#include "play/ui/PlayUi.h"

namespace x4::play {

namespace {

using nonogram::CELLS_MAX;
using nonogram::Clues;

constexpr uint8_t BOARD_VERSION = 1;

enum Mark : uint8_t { Blank = 0, Fill = 1, Cross = 2 };

// The saved game, as-is.
struct Board {
  uint8_t version;
  uint8_t size;
  uint8_t cursor;
  uint8_t done;    // solved; the picture is shown
  uint8_t unused;  // was "reviewed"; keeps older saves readable
  uint8_t failed;  // no puzzle could be made
  uint8_t reserved[2];
  uint8_t solution[CELLS_MAX];  // 1 = filled
  uint8_t cells[CELLS_MAX];     // the player's Marks
};
static_assert(sizeof(Board) <= SAVE_MAX, "Board fits a saved game");

// Clue digits never grow past this, nor the cells past CELL_MAX.
constexpr int CLUE_MAX = 30;
constexpr int CLUE_MIN = 10;
constexpr int CELL_MAX = 72;
constexpr int MARGIN = 12;

// Where everything goes, from the clue lengths and the room on screen.
struct Layout {
  int cell;
  int x0;  // grid
  int y0;
  int rowClues;  // width left of the grid
  int colClues;  // height above it
  int scale;     // clue digits
  int digit;     // their height
  int gap;       // between clues, and between the clues and the grid
};

class NonogramGame final : public Game {
 public:
  void start(const uint32_t seed, const uint8_t level) override {
    board = Board{};
    board.version = BOARD_VERSION;
    auto work = makeUniqueNoThrow<nonogram::Work>();
    auto puzzle = makeUniqueNoThrow<nonogram::Puzzle>();
    if (!work || !puzzle) {
      LOG_ERR("PLAY", "OOM: nonogram generator");
      board.size = nonogram::sizeFor(level);
      board.failed = 1;
      return;
    }
    const uint32_t began = millis();
    nonogram::generate(seed, level, *puzzle, *work, [] { delay(1); });
    LOG_INF("PLAY", "Nonogram level %u: %ux%u, %u sweeps, %u pictures, %lu ms", level, puzzle->size, puzzle->size,
            puzzle->sweeps, puzzle->attempts, static_cast<unsigned long>(millis() - began));
    board.size = puzzle->size;
    board.cursor = static_cast<uint8_t>((board.size / 2) * board.size + board.size / 2);
    std::memcpy(board.solution, puzzle->solution, CELLS_MAX);
    nonogram::cluesOf(board.solution, board.size, clues);
  }

  bool holdsConfirm() const override { return true; }

  size_t save(uint8_t* out) const override {
    std::memcpy(out, &board, sizeof(board));
    return sizeof(board);
  }

  bool restore(const uint8_t* in, const size_t size) override {
    if (size != sizeof(Board)) return false;
    std::memcpy(&board, in, sizeof(board));
    const int n = board.size;
    if (board.version != BOARD_VERSION || n < nonogram::sizeFor(1) || n > nonogram::MAX || board.cursor >= n * n) {
      return false;
    }
    for (int i = 0; i < n * n; ++i) {
      if (board.solution[i] > 1 || board.cells[i] > Cross) return false;
    }
    nonogram::cluesOf(board.solution, n, clues);
    return true;
  }

  bool input(const Keys& keys, uint32_t) override {
    if (board.done) return false;
    const int n = board.size;
    int row = board.cursor / n;
    int col = board.cursor % n;
    if (keys.up || keys.down || keys.left || keys.right) {
      if (keys.up) row = (row + n - 1) % n;
      if (keys.down) row = (row + 1) % n;
      if (keys.left) col = (col + n - 1) % n;
      if (keys.right) col = (col + 1) % n;
      board.cursor = static_cast<uint8_t>(row * n + col);
      return true;
    }
    uint8_t& cell = board.cells[board.cursor];
    if (keys.confirmHold) {
      cell = cell == Cross ? Blank : Cross;
    } else if (keys.confirm) {
      cell = cell == Fill ? Blank : Fill;
    } else {
      return false;
    }
    if (solved()) board.done = 1;
    return true;
  }

  void draw(const GfxRenderer& r, const x4::ui::Frame& f) const override {
    const int n = board.size;
    const Layout l = layout(r, f);
    const int size = l.cell * n;
    // The solution once given up; the player's fills otherwise.
    for (int i = 0; i < n * n; ++i) {
      const int cx = l.x0 + (i % n) * l.cell;
      const int cy = l.y0 + (i / n) * l.cell;
      const uint8_t mark = board.cells[i];
      if (mark == Fill) {
        r.fillRect(cx, cy, l.cell, l.cell, true);
      } else if (mark == Cross && !board.done) {
        const int arm = l.cell / 6;
        const int mx = cx + l.cell / 2;
        const int my = cy + l.cell / 2;
        r.drawLine(mx - arm, my - arm, mx + arm, my + arm - 1, 2, true);
        r.drawLine(mx - arm, my + arm - 1, mx + arm, my - arm, 2, true);
      }
    }
    // Every fifth line is heavier, for counting.
    for (int k = 0; k <= n; ++k) {
      const int pen = k % 5 == 0 || k == n ? 2 : 1;
      r.fillRect(l.x0 + k * l.cell - pen / 2, l.y0 - pen / 2, pen, size + pen, true);
      r.fillRect(l.x0 - pen / 2, l.y0 + k * l.cell - pen / 2, size + pen, pen, true);
    }

    char text[4];
    for (int line = 0; line < 2 * n; ++line) {
      const bool isRow = line < n;
      const int at = isRow ? line : line - n;
      const uint8_t count = clues.count[line];
      // A blank line's clue is 0.
      const int shown = count ? count : 1;
      if (isRow) {
        const int baseline = l.y0 + at * l.cell + (l.cell + l.digit) / 2;
        int right = l.x0 - l.gap;
        for (int k = shown - 1; k >= 0; --k) {
          snprintf(text, sizeof(text), "%u", count ? clues.runs[line][k] : 0u);
          const int w = ui::bigTextWidth(r, text, l.scale);
          ui::drawBigText(r, text, right - w / 2, baseline, l.scale);
          right -= w + l.gap;
        }
        if (lineDone(line)) ui::lighten(r, l.x0 - l.rowClues, l.y0 + at * l.cell, l.rowClues - 2, l.cell);
      } else {
        const int centre = l.x0 + at * l.cell + l.cell / 2;
        int baseline = l.y0 - l.gap;
        for (int k = shown - 1; k >= 0; --k) {
          snprintf(text, sizeof(text), "%u", count ? clues.runs[line][k] : 0u);
          ui::drawBigText(r, text, centre, baseline, l.scale);
          baseline -= l.digit + l.gap;
        }
        if (lineDone(line)) ui::lighten(r, l.x0 + at * l.cell, l.y0 - l.colClues, l.cell, l.colClues - 2);
      }
    }

    if (board.done) return;
    // A thick frame inside the cell, and a white ring inside that so it shows on a filled cell.
    const int cx = l.x0 + (board.cursor % n) * l.cell;
    const int cy = l.y0 + (board.cursor / n) * l.cell;
    const int pen = l.cell >= 40 ? ui::FOCUS_PEN : 3;
    ui::drawCursor(r, cx + 1, cy + 1, l.cell - 1, l.cell - 1, pen);
    if (board.cells[board.cursor] == Fill) {
      r.drawRect(cx + 1 + pen, cy + 1 + pen, l.cell - 1 - 2 * pen, l.cell - 1 - 2 * pen, 2, false);
    }
  }

  bool playable() const override { return !board.failed; }

  Outcome outcome() const override {
    return board.done ? Outcome::Won : Outcome::Playing;
  }

 private:
  Board board{};
  Clues clues{};  // from the solution

  bool solved() const {
    for (int i = 0; i < board.size * board.size; ++i) {
      if ((board.cells[i] == Fill) != (board.solution[i] != 0)) return false;
    }
    return true;
  }

  // The player's fills in `line` make exactly its clue.
  bool lineDone(const int line) const {
    const int n = board.size;
    uint8_t count = 0;
    uint8_t run = 0;
    for (int k = 0; k <= n; ++k) {
      const bool on = k < n && board.cells[nonogram::cellOf(line, k, n)] == Fill;
      if (on) {
        ++run;
        continue;
      }
      if (!run) continue;
      if (count >= clues.count[line] || clues.runs[line][count] != run) return false;
      ++count;
      run = 0;
    }
    return count == clues.count[line];
  }

  // The largest clue digits whose cells are still half again as tall, and wide
  // enough for a two-digit column clue.
  Layout layout(const GfxRenderer& r, const x4::ui::Frame& f) const {
    const int n = board.size;
    const int width = r.getScreenWidth() - 2 * MARGIN;
    const int top = f.top - 20;
    const int height = f.bottom - f.top + 40;
    int colCount = 1;
    bool wideColumn = false;
    for (int line = n; line < 2 * n; ++line) {
      colCount = std::max<int>(colCount, clues.count[line]);
      for (int k = 0; k < clues.count[line]; ++k) wideColumn = wideColumn || clues.runs[line][k] >= 10;
    }
    Layout l{};
    char text[4];
    for (int digit = CLUE_MAX; digit >= CLUE_MIN; --digit) {
      l.digit = digit;
      l.scale = ui::scaleForHeight(r, digit);
      l.gap = std::max(4, digit * 2 / 5);
      l.rowClues = 0;
      for (int line = 0; line < n; ++line) {
        int w = l.gap;
        for (int k = 0; k < std::max<int>(clues.count[line], 1); ++k) {
          snprintf(text, sizeof(text), "%u", clues.count[line] ? clues.runs[line][k] : 0u);
          w += ui::bigTextWidth(r, text, l.scale) + l.gap;
        }
        l.rowClues = std::max(l.rowClues, w);
      }
      l.colClues = colCount * (digit + l.gap) + l.gap;
      l.cell = std::min({(width - l.rowClues) / n, (height - l.colClues) / n, CELL_MAX});
      const int widest = wideColumn ? ui::bigTextWidth(r, "15", l.scale) + 4 : 0;
      if (l.cell >= digit * 3 / 2 && l.cell >= widest) break;
    }
    // The grid itself centred, as the other boards, where its clues leave room for it.
    const int grid = l.cell * n;
    l.x0 = std::max(MARGIN + l.rowClues, (r.getScreenWidth() - grid) / 2);
    l.y0 = std::max(top + l.colClues, top + (height - grid) / 2);
    return l;
  }
};

}  // namespace

std::unique_ptr<Game> makeNonogram() { return std::unique_ptr<Game>(new (std::nothrow) NonogramGame()); }

}  // namespace x4::play
