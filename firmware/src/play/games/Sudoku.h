#pragma once

#include <cstdint>

// Sudoku generation and grading. Pure C++, no allocation: every buffer lives in
// the caller's Work (about 1 KB).
namespace x4::play::sudoku {

inline constexpr int CELLS = 81;

// The hardest technique a puzzle needs, easiest first.
enum Tier : int8_t {
  NakedSingle = 0,
  HiddenSingle = 1,
  LockedCandidates = 2,  // pointing and claiming
  Pair = 3,              // naked and hidden pairs
  Triple = 4,            // naked and hidden triples
  XWing = 5,
  Advanced = 6,          // swordfish, XY-wing
  Trial = 7,             // needs trial and error; still one solution
  Unsolved = -1,         // beyond these techniques, or no solution
};

struct Puzzle {
  uint8_t givens[CELLS];    // 0 = empty
  uint8_t solution[CELLS];
  int8_t tier;
  uint8_t clues;
};

// What a level asks of the generator.
struct LevelSpec {
  int8_t minTier;
  int8_t maxTier;
  uint8_t minClues;
};
LevelSpec levelSpec(uint8_t level);

// Scratch for the solvers (kept off the stack).
struct Work {
  uint8_t grid[CELLS];
  uint16_t cand[CELLS];
  uint8_t stackCell[CELLS];
  uint16_t stackLeft[CELLS];
  uint8_t order[CELLS];
  Puzzle trial;
};

// Counts solutions of `grid` up to `limit` by backtracking. Copies the first
// one to `first` when set. Gives up after `maxNodes` placements and returns -1.
int countSolutions(const uint8_t (&grid)[CELLS], int limit, Work& work, uint8_t* first = nullptr,
                   uint32_t maxNodes = 2000000);

// Solves with human techniques up to `maxTier`, easiest first. Returns the
// hardest one needed, or Unsolved. A solved grid has exactly one solution,
// because every technique only removes impossible candidates. With maxTier
// Trial, a grid the techniques cannot finish is Trial when it has one solution.
int8_t grade(const uint8_t (&grid)[CELLS], int8_t maxTier, Work& work);

// A puzzle for `level` (1-10) from `seed`. Deterministic. `between` runs
// between attempts (the device yields there, so the idle task is not starved).
void generate(uint32_t seed, uint8_t level, Puzzle& out, Work& work, void (*between)() = nullptr);

}  // namespace x4::play::sudoku
