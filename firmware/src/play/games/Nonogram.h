#pragma once

#include <cstdint>

// Nonogram generation and the line solver. Pure C++, no allocation: every
// buffer lives in the caller's Work (about 1 KB).
namespace x4::play::nonogram {

inline constexpr int MAX = 15;
inline constexpr int CELLS_MAX = MAX * MAX;
inline constexpr int RUNS_MAX = (MAX + 1) / 2;  // 1 0 1 0 ... 1
inline constexpr int LINES_MAX = 2 * MAX;
// Generated pictures have at most this many runs in a line, so the clues fit beside the grid.
inline constexpr int RUNS_SHOWN = 5;

// A cell as the solver knows it.
enum Cell : uint8_t { Unknown = 0, Filled = 1, Empty = 2 };

// The side of the grid at `level` (1-10): 5 to 15.
uint8_t sizeFor(uint8_t level);

// Run lengths per line: rows 0..size-1, then columns.
struct Clues {
  uint8_t count[LINES_MAX];
  uint8_t runs[LINES_MAX][RUNS_MAX];
};

// The clues of a picture: `cells` holds size*size bytes, row by row, non-zero filled.
void cluesOf(const uint8_t* cells, int size, Clues& out);

// The cells of line `line` (a row below `size`, else a column), in order.
inline int cellOf(const int line, const int k, const int size) {
  return line < size ? line * size + k : k * size + (line - size);
}

// Deduces what each run-length placement agrees on, line by line, until
// nothing changes. `state` (size*size Cells) may hold known cells on entry.
// Returns the number of cells still Unknown, or -1 on a contradiction. Every
// deduction is forced, so a full solve proves the solution unique.
// `sweeps` counts the passes over the rows or over the columns: how deep the
// puzzle goes.
int solve(const Clues& clues, int size, uint8_t* state, int* sweeps = nullptr);

// One line: what every placement of `runs` over `line` agrees on is written
// back into `line`. False when no placement fits.
bool solveLine(uint8_t* line, int length, const uint8_t* runs, int count);

struct Puzzle {
  uint8_t size;
  uint8_t solution[CELLS_MAX];  // 1 = filled
  uint8_t sweeps;               // solve() sweeps from blank
  uint8_t attempts;             // pictures drawn up to this one
  uint8_t touches;              // cells changed to make it solve
};

// Scratch for the generator (kept off the stack).
struct Work {
  uint8_t state[CELLS_MAX];
  uint8_t picture[CELLS_MAX];
  uint8_t field[CELLS_MAX];
  Clues clues;
};

// A puzzle for `level` from `seed`, solvable by line logic alone, with one
// solution. Deterministic. `between` runs between attempts (the device yields
// there, so the idle task is not starved).
void generate(uint32_t seed, uint8_t level, Puzzle& out, Work& work, void (*between)() = nullptr);

}  // namespace x4::play::nonogram
