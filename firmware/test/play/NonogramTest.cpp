#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <vector>

#include "play/games/Nonogram.h"

namespace nonogram = x4::play::nonogram;
using nonogram::Clues;
using nonogram::Empty;
using nonogram::Filled;
using nonogram::Unknown;

namespace {

struct NonogramTest : ::testing::Test {
  std::unique_ptr<nonogram::Work> work = std::make_unique<nonogram::Work>();
  nonogram::Puzzle puzzle{};
};

std::vector<uint8_t> runsOf(const uint32_t mask, const int n) {
  std::vector<uint8_t> runs;
  uint8_t run = 0;
  for (int k = 0; k <= n; ++k) {
    if (k < n && (mask >> k & 1)) {
      ++run;
    } else if (run) {
      runs.push_back(run);
      run = 0;
    }
  }
  return runs;
}

// Column `c` so far (rows 0..r) can still become `runs`: the finished runs
// match its first ones and the open one is no longer than the next.
bool prefixFits(const std::vector<uint32_t>& rows, const int r, const int c, const uint8_t* runs, const int count) {
  int at = 0;
  int open = 0;
  for (int k = 0; k <= r; ++k) {
    if (rows[k] >> c & 1) {
      ++open;
    } else if (open) {
      if (at >= count || runs[at] != open) return false;
      ++at;
      open = 0;
    }
  }
  if (open) return at < count && open <= runs[at];
  return true;
}

// Solutions of `clues`, counted up to `limit` by plain backtracking over the
// row placements: independent of the line solver.
int countSolutions(const Clues& clues, const int n, const int limit) {
  std::vector<std::vector<uint32_t>> options(n);
  for (uint32_t mask = 0; mask < (1u << n); ++mask) {
    const std::vector<uint8_t> runs = runsOf(mask, n);
    for (int r = 0; r < n; ++r) {
      if (runs.size() == clues.count[r] && std::equal(runs.begin(), runs.end(), clues.runs[r])) {
        options[r].push_back(mask);
      }
    }
  }
  std::vector<uint32_t> rows(n);
  int found = 0;
  const auto place = [&](const auto& self, const int r) -> void {
    if (found >= limit) return;
    if (r == n) {
      for (int c = 0; c < n; ++c) {
        uint32_t column = 0;
        for (int k = 0; k < n; ++k) column |= (rows[k] >> c & 1) << k;
        const std::vector<uint8_t> runs = runsOf(column, n);
        if (runs.size() != clues.count[n + c] || !std::equal(runs.begin(), runs.end(), clues.runs[n + c])) return;
      }
      ++found;
      return;
    }
    for (const uint32_t mask : options[r]) {
      rows[r] = mask;
      bool ok = true;
      for (int c = 0; c < n && ok; ++c) ok = prefixFits(rows, r, c, clues.runs[n + c], clues.count[n + c]);
      if (ok) self(self, r + 1);
    }
  };
  place(place, 0);
  return found;
}

}  // namespace

TEST(Nonogram, GridGrowsFromFiveToFifteen) {
  EXPECT_EQ(nonogram::sizeFor(1), 5);
  EXPECT_EQ(nonogram::sizeFor(10), 15);
  for (uint8_t level = 2; level <= 10; ++level) EXPECT_GT(nonogram::sizeFor(level), nonogram::sizeFor(level - 1));
  EXPECT_EQ(nonogram::sizeFor(0), 5);
  EXPECT_EQ(nonogram::sizeFor(11), 15);
}

TEST(Nonogram, CluesAreTheRuns) {
  // ##.#.
  // .....
  // #####
  // ...##
  // #.#.#
  const uint8_t cells[25] = {1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 1};
  Clues clues{};
  nonogram::cluesOf(cells, 5, clues);
  EXPECT_EQ(clues.count[0], 2);
  EXPECT_EQ(clues.runs[0][0], 2);
  EXPECT_EQ(clues.runs[0][1], 1);
  EXPECT_EQ(clues.count[1], 0);
  EXPECT_EQ(clues.count[2], 1);
  EXPECT_EQ(clues.runs[2][0], 5);
  EXPECT_EQ(clues.count[4], 3);
  // Column 0 top to bottom: # . # . # -> 1 1 1.
  EXPECT_EQ(clues.count[5], 3);
  // Column 4: . . # # # -> 3.
  EXPECT_EQ(clues.count[9], 1);
  EXPECT_EQ(clues.runs[9][0], 3);
}

TEST(Nonogram, LineSolverFindsTheOverlap) {
  uint8_t line[10] = {};
  const uint8_t runs[] = {8};
  ASSERT_TRUE(nonogram::solveLine(line, 10, runs, 1));
  for (int i = 0; i < 10; ++i) EXPECT_EQ(line[i], i >= 2 && i < 8 ? Filled : Unknown) << i;
}

TEST(Nonogram, LineSolverUsesKnownCells) {
  // 3 in 7 with cell 1 filled: the run covers 1, so 4..6 are empty and 2 is filled.
  uint8_t line[7] = {Unknown, Filled, Unknown, Unknown, Unknown, Unknown, Unknown};
  const uint8_t runs[] = {3};
  ASSERT_TRUE(nonogram::solveLine(line, 7, runs, 1));
  const uint8_t want[7] = {Unknown, Filled, Filled, Unknown, Empty, Empty, Empty};
  for (int i = 0; i < 7; ++i) EXPECT_EQ(line[i], want[i]) << i;
}

TEST(Nonogram, LineSolverFillsAnExactFit) {
  uint8_t line[7] = {};
  const uint8_t runs[] = {2, 1, 2};
  ASSERT_TRUE(nonogram::solveLine(line, 7, runs, 3));
  const uint8_t want[7] = {Filled, Filled, Empty, Filled, Empty, Filled, Filled};
  for (int i = 0; i < 7; ++i) EXPECT_EQ(line[i], want[i]) << i;
}

TEST(Nonogram, BlankLineIsAllEmpty) {
  uint8_t line[6] = {};
  ASSERT_TRUE(nonogram::solveLine(line, 6, nullptr, 0));
  for (const uint8_t cell : line) EXPECT_EQ(cell, Empty);
  uint8_t marked[6] = {Unknown, Unknown, Filled, Unknown, Unknown, Unknown};
  EXPECT_FALSE(nonogram::solveLine(marked, 6, nullptr, 0));
}

TEST(Nonogram, LineSolverRejectsAContradiction) {
  uint8_t line[5] = {Filled, Empty, Filled, Empty, Filled};
  const uint8_t runs[] = {2};
  EXPECT_FALSE(nonogram::solveLine(line, 5, runs, 1));
  uint8_t full[5] = {Unknown, Unknown, Empty, Unknown, Unknown};
  const uint8_t three[] = {3};
  EXPECT_FALSE(nonogram::solveLine(full, 5, three, 1));
}

TEST(Nonogram, LineSolverAgreesWithEveryPlacement) {
  // Against brute force: every 8-cell line state and clue.
  constexpr int N = 8;
  for (uint32_t truth = 0; truth < (1u << N); truth += 7) {
    const std::vector<uint8_t> runs = runsOf(truth, N);
    for (uint32_t known = 0; known < (1u << N); known += 11) {
      uint8_t line[N];
      for (int i = 0; i < N; ++i) line[i] = (known >> i & 1) ? ((truth >> i & 1) ? Filled : Empty) : Unknown;
      uint32_t canFill = 0;
      uint32_t canEmpty = 0;
      for (uint32_t mask = 0; mask < (1u << N); ++mask) {
        if (runsOf(mask, N) != runs || ((mask ^ truth) & known)) continue;
        canFill |= mask;
        canEmpty |= ~mask;
      }
      ASSERT_TRUE(nonogram::solveLine(line, N, runs.data(), static_cast<int>(runs.size())));
      for (int i = 0; i < N; ++i) {
        const bool fill = canFill >> i & 1;
        const bool empty = canEmpty >> i & 1;
        const uint8_t want = fill && empty ? Unknown : fill ? Filled : Empty;
        EXPECT_EQ(line[i], want) << "truth " << truth << " known " << known << " cell " << i;
      }
    }
  }
}

TEST(Nonogram, AmbiguousPuzzleIsLeftUnsolved) {
  // A diagonal: its mirror image has the same clues.
  const uint8_t cells[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  Clues clues{};
  nonogram::cluesOf(cells, 3, clues);
  uint8_t state[9] = {};
  EXPECT_EQ(nonogram::solve(clues, 3, state), 9);
  EXPECT_EQ(countSolutions(clues, 3, 10), 6);  // every permutation matrix
}

TEST_F(NonogramTest, EveryLevelSolvesByLineLogicToTheSolution) {
  for (uint8_t level = 1; level <= 10; ++level) {
    for (uint32_t seed = 1; seed <= 300; ++seed) {
      nonogram::generate(seed * 2654435761u + level, level, puzzle, *work);
      const int n = puzzle.size;
      ASSERT_EQ(n, nonogram::sizeFor(level));
      ASSERT_LT(puzzle.attempts, 32) << "fell back to a full grid, level " << int(level) << " seed " << seed;
      Clues clues{};
      nonogram::cluesOf(puzzle.solution, n, clues);
      int filled = 0;
      for (int line = 0; line < 2 * n; ++line) EXPECT_LE(clues.count[line], nonogram::RUNS_SHOWN);
      for (int i = 0; i < n * n; ++i) filled += puzzle.solution[i] ? 1 : 0;
      EXPECT_GE(filled * 100, n * n * 35);
      EXPECT_LE(filled * 100, n * n * 75);
      uint8_t state[nonogram::CELLS_MAX] = {};
      int sweeps = 0;
      ASSERT_EQ(nonogram::solve(clues, n, state, &sweeps), 0) << "level " << int(level) << " seed " << seed;
      EXPECT_EQ(sweeps, puzzle.sweeps);
      for (int i = 0; i < n * n; ++i) ASSERT_EQ(state[i], puzzle.solution[i] ? Filled : Empty);
    }
  }
}

TEST_F(NonogramTest, GeneratedPuzzlesHaveOneSolution) {
  // Brute force, independent of the line solver.
  for (uint8_t level = 1; level <= 10; ++level) {
    for (uint32_t seed = 1; seed <= 40; ++seed) {
      nonogram::generate(seed * 7919u + level, level, puzzle, *work);
      Clues clues{};
      nonogram::cluesOf(puzzle.solution, puzzle.size, clues);
      EXPECT_EQ(countSolutions(clues, puzzle.size, 2), 1) << "level " << int(level) << " seed " << seed;
    }
  }
}

TEST_F(NonogramTest, SameSeedSamePuzzle) {
  nonogram::Puzzle other{};
  nonogram::generate(99, 7, puzzle, *work);
  nonogram::generate(99, 7, other, *work);
  EXPECT_EQ(std::memcmp(puzzle.solution, other.solution, nonogram::CELLS_MAX), 0);
  nonogram::generate(100, 7, other, *work);
  EXPECT_NE(std::memcmp(puzzle.solution, other.solution, nonogram::CELLS_MAX), 0);
}

TEST_F(NonogramTest, HarderLevelsTakeMoreSweeps) {
  double previous = 0;
  for (uint8_t level = 1; level <= 10; level += 3) {
    int total = 0;
    for (uint32_t seed = 1; seed <= 100; ++seed) {
      nonogram::generate(seed * 31u, level, puzzle, *work);
      total += puzzle.sweeps;
    }
    const double mean = total / 100.0;
    EXPECT_GT(mean, previous) << "level " << int(level);
    previous = mean;
  }
}

TEST_F(NonogramTest, LargestGridIsQuick) {
  // The X4 runs this kind of code roughly 50-100x slower than a desktop core.
  double worst = 0;
  double total = 0;
  for (uint32_t seed = 1; seed <= 500; ++seed) {
    const auto began = std::chrono::steady_clock::now();
    nonogram::generate(seed, 10, puzzle, *work);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
    total += ms;
    worst = std::max(worst, ms);
  }
  std::printf("level 10: %.3f ms average, %.3f ms worst\n", total / 500, worst);
  EXPECT_LT(total / 500, 5.0);
}
