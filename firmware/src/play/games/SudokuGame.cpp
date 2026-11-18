// Sudoku: arrows move, Confirm cycles the digit, holding Confirm writes the
// shown digit as a note (or clears the notes of an empty cell).
#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "Sudoku.h"
#include "play/Game.h"
#include "play/Play.h"
#include "play/ui/PlayUi.h"

namespace x4::play {

namespace {

using sudoku::CELLS;

constexpr uint8_t BOARD_VERSION = 1;

// The saved game, as-is.
struct Board {
  uint8_t version;
  uint8_t cursor;
  uint8_t failed;  // no puzzle could be made
  uint8_t reserved;
  uint8_t givens[CELLS];
  uint8_t solution[CELLS];
  uint8_t values[CELLS];  // the player's digits; givens are copied in
  uint16_t notes[CELLS];  // bits 1-9
};
static_assert(sizeof(Board) <= SAVE_MAX, "Board fits a saved game");

class SudokuGame final : public Game {
 public:
  void start(const uint32_t seed, const uint8_t level) override {
    board = Board{};
    board.version = BOARD_VERSION;
    board.cursor = 40;
    auto work = makeUniqueNoThrow<sudoku::Work>();
    auto puzzle = makeUniqueNoThrow<sudoku::Puzzle>();
    if (!work || !puzzle) {
      LOG_ERR("PLAY", "OOM: sudoku generator");
      board.failed = 1;
      return;
    }
    const uint32_t began = millis();
    sudoku::generate(seed, level, *puzzle, *work, [] { delay(1); });
    LOG_INF("PLAY", "Sudoku level %u: tier %d, %u clues, %lu ms", level, puzzle->tier, puzzle->clues,
            static_cast<unsigned long>(millis() - began));
    std::memcpy(board.givens, puzzle->givens, CELLS);
    std::memcpy(board.solution, puzzle->solution, CELLS);
    std::memcpy(board.values, puzzle->givens, CELLS);
  }

  bool holdsConfirm() const override { return true; }

  size_t save(uint8_t* out) const override {
    std::memcpy(out, &board, sizeof(board));
    return sizeof(board);
  }

  bool restore(const uint8_t* in, const size_t size) override {
    if (size != sizeof(Board)) return false;
    std::memcpy(&board, in, sizeof(board));
    if (board.version != BOARD_VERSION || board.cursor >= CELLS) return false;
    for (int i = 0; i < CELLS; ++i) {
      if (board.values[i] > 9 || board.solution[i] < 1 || board.solution[i] > 9) return false;
      if (board.givens[i] && board.givens[i] != board.values[i]) return false;
    }
    return true;
  }

  bool input(const Keys& keys, uint32_t) override {
    int row = board.cursor / 9;
    int col = board.cursor % 9;
    if (keys.up || keys.down || keys.left || keys.right) {
      if (keys.up) row = (row + 8) % 9;
      if (keys.down) row = (row + 1) % 9;
      if (keys.left) col = (col + 8) % 9;
      if (keys.right) col = (col + 1) % 9;
      board.cursor = static_cast<uint8_t>(row * 9 + col);
      return true;
    }
    const int i = board.cursor;
    if (board.givens[i] || !(keys.confirm || keys.confirmHold)) return false;
    if (keys.confirmHold) {
      if (board.values[i]) {
        board.notes[i] ^= static_cast<uint16_t>(1u << board.values[i]);
        board.values[i] = 0;
      } else if (board.notes[i]) {
        board.notes[i] = 0;
      } else {
        return false;
      }
      return true;
    }
    board.values[i] = static_cast<uint8_t>((board.values[i] + 1) % 10);
    return true;
  }

  void draw(const GfxRenderer& r, const x4::ui::Frame& f) const override {
    const int width = r.getScreenWidth();
    const int cell = std::min((width - 48) / 9, (f.bottom - f.top + 40) / 9);
    const int size = cell * 9;
    const int x0 = (width - size) / 2;
    const int y0 = f.top - 20 + (f.bottom - f.top + 40 - size) / 2;

    for (int k = 0; k <= 9; ++k) {
      const int pen = k % 3 == 0 ? 3 : 1;
      r.fillRect(x0 + k * cell - pen / 2, y0, pen, size, true);
      r.fillRect(x0, y0 + k * cell - pen / 2, size, pen, true);
    }

    const int scale = ui::scaleForHeight(r, cell * 11 / 20);
    const int digitHeight = ui::glyphHeight(r, scale);
    const bool full = std::none_of(board.values, board.values + CELLS, [](const uint8_t v) { return v == 0; });
    // Notes sit inside the cursor frame (inset 3, pen 3), so they never touch it.
    const int noteInset = 7;
    const int noteSize = (cell - 2 * noteInset) / 3;
    // Each note fills its third of the cell, about 10 px high on the X4: smaller does not read.
    const int noteScale = ui::scaleForHeight(r, noteSize - 1);
    const int noteHeight = ui::glyphHeight(r, noteScale);
    for (int i = 0; i < CELLS; ++i) {
      const int cx = x0 + (i % 9) * cell;
      const int cy = y0 + (i / 9) * cell;
      const uint8_t v = board.values[i];
      // Givens stand on a light tint, as in print, so the reader's own digits read apart from them.
      if (board.givens[i]) r.fillRectDither(cx + 2, cy + 2, cell - 3, cell - 3, Color::LightGray);
      if (v) {
        const char digit[2] = {static_cast<char>('0' + v), '\0'};
        const int baseline = cy + (cell + digitHeight) / 2;
        ui::drawBigText(r, digit, cx + cell / 2, baseline, scale,
                        board.givens[i] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        if (full && !board.givens[i] && conflicts(i)) {
          r.fillRect(cx + cell / 4, baseline + 4, cell / 2, 2, true);
        }
      } else if (board.notes[i]) {
        for (int d = 1; d <= 9; ++d) {
          if (!(board.notes[i] & (1u << d))) continue;
          const char note[2] = {static_cast<char>('0' + d), '\0'};
          const int nx = cx + noteInset + ((d - 1) % 3) * noteSize + noteSize / 2;
          const int ny = cy + noteInset + ((d - 1) / 3) * noteSize + (noteSize + noteHeight) / 2;
          ui::drawBigText(r, note, nx, ny, noteScale, EpdFontFamily::REGULAR);
        }
      }
    }
    if (outcome() == Outcome::Playing) {
      ui::drawCursor(r, x0 + (board.cursor % 9) * cell + 3, y0 + (board.cursor / 9) * cell + 3, cell - 5, cell - 5, ui::FOCUS_PEN);
    }
  }

  bool playable() const override { return !board.failed; }

  Outcome outcome() const override {
    return std::memcmp(board.values, board.solution, CELLS) == 0 ? Outcome::Won : Outcome::Playing;
  }

 private:
  Board board{};

  // The digit at `i` repeats in its row, column or box.
  bool conflicts(const int i) const {
    const int r = i / 9;
    const int c = i % 9;
    for (int j = 0; j < CELLS; ++j) {
      if (j == i || board.values[j] != board.values[i]) continue;
      const bool box = (j / 27 == i / 27) && ((j % 9) / 3 == c / 3);
      if (j / 9 == r || j % 9 == c || box) return true;
    }
    return false;
  }
};

}  // namespace

std::unique_ptr<Game> makeSudoku() { return std::unique_ptr<Game>(new (std::nothrow) SudokuGame()); }

}  // namespace x4::play
