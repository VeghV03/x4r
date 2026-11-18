#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "Journal.h"
#include "JsonlReader.h"
#include "play/Autosave.h"
#include "play/Play.h"
#include "play/PlayPaths.h"
#include "play/games/Einstein.h"
#include "play/games/MemoryGames.h"
#include "play/games/Sudoku.h"

using namespace x4;
namespace sudoku = x4::play::sudoku;
namespace memory = x4::play::memory;
namespace einstein = x4::play::einstein;

namespace {

std::string contents(const std::string& path) {
  const auto it = fake::files.find(path);
  if (it == fake::files.end()) return {};
  return {it->second->bytes.begin(), it->second->bytes.end()};
}

std::vector<std::string> lines(const std::string& path) {
  std::vector<std::string> out;
  const std::string all = contents(path);
  size_t start = 0;
  while (start < all.size()) {
    const size_t end = all.find('\n', start);
    out.push_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

struct Fresh : ::testing::Test {
  void SetUp() override { fake::reset(); }
};

struct SudokuTest : ::testing::Test {
  std::unique_ptr<sudoku::Work> work = std::make_unique<sudoku::Work>();
  sudoku::Puzzle puzzle{};
};

}  // namespace

// ---- Sudoku -------------------------------------------------------------------

TEST_F(SudokuTest, EveryLevelHasOneSolutionMatchingTheGivens) {
  for (uint8_t level = 1; level <= 10; ++level) {
    for (uint32_t seed = 1; seed <= 6; ++seed) {
      sudoku::generate(seed * 7919u + level, level, puzzle, *work);
      uint8_t solved[sudoku::CELLS] = {};
      ASSERT_EQ(sudoku::countSolutions(puzzle.givens, 2, *work, solved), 1) << "level " << int(level);
      EXPECT_EQ(std::memcmp(solved, puzzle.solution, sudoku::CELLS), 0);
      int clues = 0;
      for (int i = 0; i < sudoku::CELLS; ++i) {
        if (puzzle.givens[i]) {
          ++clues;
          EXPECT_EQ(puzzle.givens[i], puzzle.solution[i]);
        }
      }
      EXPECT_EQ(clues, puzzle.clues);
      EXPECT_GE(clues, sudoku::levelSpec(level).minClues);
    }
  }
}

TEST_F(SudokuTest, SolutionIsAValidGrid) {
  sudoku::generate(42, 5, puzzle, *work);
  for (int unit = 0; unit < 9; ++unit) {
    uint16_t row = 0, col = 0, box = 0;
    for (int k = 0; k < 9; ++k) {
      row |= 1u << puzzle.solution[unit * 9 + k];
      col |= 1u << puzzle.solution[k * 9 + unit];
      box |= 1u << puzzle.solution[((unit / 3) * 3 + k / 3) * 9 + (unit % 3) * 3 + k % 3];
    }
    EXPECT_EQ(row, 0x3FE);
    EXPECT_EQ(col, 0x3FE);
    EXPECT_EQ(box, 0x3FE);
  }
}

TEST_F(SudokuTest, SameSeedSamePuzzle) {
  sudoku::Puzzle again{};
  sudoku::generate(1234, 6, puzzle, *work);
  sudoku::generate(1234, 6, again, *work);
  EXPECT_EQ(std::memcmp(puzzle.givens, again.givens, sudoku::CELLS), 0);
  sudoku::generate(1235, 6, again, *work);
  EXPECT_NE(std::memcmp(puzzle.givens, again.givens, sudoku::CELLS), 0);
}

TEST_F(SudokuTest, GradeMatchesTheGeneratorsTier) {
  for (uint8_t level = 1; level <= 10; ++level) {
    sudoku::generate(99u + level, level, puzzle, *work);
    EXPECT_EQ(sudoku::grade(puzzle.givens, sudoku::Trial, *work), puzzle.tier) << "level " << int(level);
    // The hardest technique is needed: one tier less does not solve it.
    if (puzzle.tier > sudoku::NakedSingle && puzzle.tier < sudoku::Trial) {
      EXPECT_EQ(sudoku::grade(puzzle.givens, static_cast<int8_t>(puzzle.tier - 1), *work), sudoku::Unsolved);
    }
  }
}

TEST_F(SudokuTest, LevelsHitTheirTechniqueBand) {
  for (uint8_t level = 1; level <= 10; ++level) {
    const sudoku::LevelSpec spec = sudoku::levelSpec(level);
    int inBand = 0;
    constexpr int SEEDS = 20;
    for (uint32_t seed = 1; seed <= SEEDS; ++seed) {
      sudoku::generate(seed * 2654435761u, level, puzzle, *work);
      EXPECT_LE(puzzle.tier, spec.maxTier) << "level " << int(level);
      if (puzzle.tier >= spec.minTier) ++inBand;
    }
    EXPECT_GE(inBand, SEEDS * 7 / 10) << "level " << int(level);
  }
}

TEST_F(SudokuTest, HarderLevelsNeedHarderTechniques) {
  double previous = -1.0;
  for (uint8_t level = 1; level <= 10; level += 3) {
    double sum = 0.0;
    for (uint32_t seed = 1; seed <= 10; ++seed) {
      sudoku::generate(seed * 31u + level, level, puzzle, *work);
      sum += puzzle.tier;
    }
    EXPECT_GT(sum / 10.0, previous) << "level " << int(level);
    previous = sum / 10.0;
  }
}

TEST_F(SudokuTest, AmbiguousGridIsNotGraded) {
  sudoku::generate(7, 1, puzzle, *work);
  uint8_t grid[sudoku::CELLS] = {};
  std::memcpy(grid, puzzle.solution, sudoku::CELLS);
  // Clear a whole band of rows: its rows can be permuted, so many solutions.
  for (int i = 0; i < 27; ++i) grid[i] = 0;
  EXPECT_EQ(sudoku::countSolutions(grid, 2, *work), 2);
  EXPECT_EQ(sudoku::grade(grid, sudoku::Trial, *work), sudoku::Unsolved);
}

TEST_F(SudokuTest, ContradictionHasNoSolution) {
  uint8_t grid[sudoku::CELLS] = {};
  grid[0] = 5;
  grid[1] = 5;
  EXPECT_EQ(sudoku::countSolutions(grid, 2, *work), 0);
  EXPECT_EQ(sudoku::grade(grid, sudoku::Advanced, *work), sudoku::Unsolved);
}

TEST_F(SudokuTest, NakedSinglesSolveAGridMissingOneDigitPerRow) {
  sudoku::generate(3, 1, puzzle, *work);
  uint8_t grid[sudoku::CELLS];
  std::memcpy(grid, puzzle.solution, sudoku::CELLS);
  for (int r = 0; r < 9; ++r) grid[r * 9 + r] = 0;
  EXPECT_EQ(sudoku::grade(grid, sudoku::NakedSingle, *work), sudoku::NakedSingle);
}

// ---- logic grid --------------------------------------------------------------------

namespace {

struct EinsteinTest : ::testing::Test {
  std::unique_ptr<einstein::Work> work = std::make_unique<einstein::Work>();
  einstein::Puzzle puzzle{};
  einstein::Grid grid{};
};

uint32_t einsteinSeed(const uint32_t seed, const uint8_t level) { return seed * 2654435761u + level; }

}  // namespace

TEST_F(EinsteinTest, EveryLevelHasExactlyOneSolutionTheCluesAgreeWith) {
  for (uint8_t level = 1; level <= 10; ++level) {
    const einstein::LevelSpec spec = einstein::levelSpec(level);
    for (uint32_t seed = 1; seed <= 40; ++seed) {
      einstein::generate(einsteinSeed(seed, level), level, puzzle, *work);
      SCOPED_TRACE("level " + std::to_string(level) + " seed " + std::to_string(seed));
      ASSERT_EQ(puzzle.cats, spec.cats);
      ASSERT_EQ(puzzle.items, spec.items);
      ASSERT_NE(puzzle.tier, einstein::Unsolved);
      ASSERT_GT(puzzle.clueCount, 0);
      ASSERT_LE(puzzle.clueCount, einstein::CLUES_MAX);
      for (int k = 0; k < puzzle.clueCount; ++k) EXPECT_TRUE(einstein::holds(puzzle.clues[k], puzzle.at));
      EXPECT_EQ(einstein::countSolutions(puzzle.cats, puzzle.items, puzzle.clues, puzzle.clueCount, 2), 1);
    }
  }
}

TEST_F(EinsteinTest, SolutionPlacesEveryItemOnce) {
  einstein::generate(7, 10, puzzle, *work);
  for (int c = 0; c < puzzle.cats; ++c) {
    uint8_t seen = 0;
    for (int i = 0; i < puzzle.items; ++i) {
      ASSERT_LT(puzzle.at[c][i], puzzle.items);
      seen |= static_cast<uint8_t>(1u << puzzle.at[c][i]);
      if (i > 0 && c != einstein::POSITION) EXPECT_LT(puzzle.name[c][i - 1], puzzle.name[c][i]);
    }
    EXPECT_EQ(seen, (1u << puzzle.items) - 1);
    if (c == einstein::POSITION) {
      for (int i = 0; i < puzzle.items; ++i) EXPECT_EQ(puzzle.at[c][i], i);
    }
  }
  // Each other category has its own name set.
  for (int a = 1; a < puzzle.cats; ++a) {
    for (int b = a + 1; b < puzzle.cats; ++b) EXPECT_NE(puzzle.set[a], puzzle.set[b]);
  }
}

TEST_F(EinsteinTest, SolverReachesTheSolution) {
  for (uint8_t level = 1; level <= 10; ++level) {
    einstein::generate(einsteinSeed(3, level), level, puzzle, *work);
    const int8_t maxTier = einstein::levelSpec(level).maxTier;
    ASSERT_EQ(einstein::solve(puzzle.cats, puzzle.items, puzzle.clues, puzzle.clueCount, maxTier, grid), puzzle.tier);
    for (int c = 1; c < puzzle.cats; ++c) {
      for (int i = 0; i < puzzle.items; ++i) {
        EXPECT_EQ(grid.poss[c][einstein::POSITION][i], 1u << puzzle.at[c][i]) << "level " << int(level);
      }
    }
  }
}

TEST_F(EinsteinTest, SolverNeverDropsTheSolution) {
  // With clues missing the solver stops early, but every pair of the solution stays open.
  for (uint32_t seed = 1; seed <= 60; ++seed) {
    const uint8_t level = static_cast<uint8_t>(1 + seed % 10);
    einstein::generate(einsteinSeed(seed, level), level, puzzle, *work);
    const int count = puzzle.clueCount / 2;
    const int8_t tier = einstein::solve(puzzle.cats, puzzle.items, puzzle.clues, count, einstein::Chain, grid);
    for (int a = 0; a < puzzle.cats; ++a) {
      for (int b = 0; b < puzzle.cats; ++b) {
        if (a == b) continue;
        for (int i = 0; i < puzzle.items; ++i) {
          int j = 0;
          while (puzzle.at[b][j] != puzzle.at[a][i]) ++j;
          EXPECT_TRUE(grid.poss[a][b][i] & (1u << j));
        }
      }
    }
    if (tier != einstein::Unsolved) {
      EXPECT_EQ(einstein::countSolutions(puzzle.cats, puzzle.items, puzzle.clues, count, 2), 1);
    }
  }
}

TEST_F(EinsteinTest, MinimalCluesAreAllNeeded) {
  einstein::generate(11, 8, puzzle, *work);
  const int8_t maxTier = einstein::levelSpec(8).maxTier;
  einstein::Clue fewer[einstein::CLUES_MAX];
  for (int drop = 0; drop < puzzle.clueCount; ++drop) {
    int m = 0;
    for (int k = 0; k < puzzle.clueCount; ++k) {
      if (k != drop) fewer[m++] = puzzle.clues[k];
    }
    EXPECT_EQ(einstein::solve(puzzle.cats, puzzle.items, fewer, m, maxTier, grid), einstein::Unsolved) << drop;
  }
}

TEST_F(EinsteinTest, CluesReadAsWorded) {
  // A position is only ever the second item of "is", "is not" and either-or,
  // and either-or offers two items of one other category.
  for (uint8_t level = 1; level <= 10; ++level) {
    for (uint32_t seed = 1; seed <= 10; ++seed) {
      einstein::generate(einsteinSeed(seed, level), level, puzzle, *work);
      for (int k = 0; k < puzzle.clueCount; ++k) {
        const einstein::Clue& c = puzzle.clues[k];
        EXPECT_NE(einstein::catOf(c.a), einstein::POSITION);
        EXPECT_NE(c.a, c.b);
        const bool positional = c.kind == einstein::NextTo || c.kind == einstein::LeftOf || c.kind == einstein::Before;
        if (positional) EXPECT_NE(einstein::catOf(c.b), einstein::POSITION);
        if (c.kind == einstein::Either) {
          EXPECT_EQ(einstein::catOf(c.b), einstein::catOf(c.c));
          EXPECT_NE(einstein::catOf(c.b), einstein::catOf(c.a));
          EXPECT_NE(c.b, c.c);
        }
        if (c.kind == einstein::Same || c.kind == einstein::Not) {
          EXPECT_NE(einstein::catOf(c.a), einstein::catOf(c.b));
        }
      }
    }
  }
}

TEST_F(EinsteinTest, SameSeedSamePuzzle) {
  einstein::Puzzle again{};
  einstein::generate(99, 6, puzzle, *work);
  einstein::generate(99, 6, again, *work);
  EXPECT_EQ(std::memcmp(&puzzle, &again, sizeof(puzzle)), 0);
  einstein::generate(100, 6, again, *work);
  EXPECT_NE(std::memcmp(&puzzle, &again, sizeof(puzzle)), 0);
}

TEST_F(EinsteinTest, SizesGrowWithTheLevel) {
  int cells = 0;
  for (uint8_t level = 1; level <= 10; ++level) {
    const einstein::LevelSpec spec = einstein::levelSpec(level);
    EXPECT_GE(spec.cats, 3);
    EXPECT_LE(spec.cats, einstein::CATS_MAX);
    EXPECT_GE(spec.items, 3);
    EXPECT_LE(spec.items, einstein::ITEMS_MAX);
    const int now = spec.cats * (spec.cats - 1) / 2 * spec.items * spec.items;
    EXPECT_GE(now, cells) << "level " << int(level);
    cells = now;
  }
  EXPECT_EQ(einstein::levelSpec(1).cats, 3);
  EXPECT_EQ(einstein::levelSpec(1).items, 3);
  EXPECT_EQ(einstein::levelSpec(10).cats, 5);
  EXPECT_EQ(einstein::levelSpec(10).items, 5);
}

TEST_F(EinsteinTest, HarderLevelsNeedChainedDeductions) {
  double previous = -1;
  for (uint8_t level = 1; level <= 10; level += 3) {
    int chained = 0;
    int steps = 0;
    for (uint32_t seed = 1; seed <= 30; ++seed) {
      einstein::generate(einsteinSeed(seed, level), level, puzzle, *work);
      chained += puzzle.tier == einstein::Chain;
      steps += puzzle.steps;
    }
    const double mean = steps / 30.0;
    EXPECT_GT(mean, previous) << "level " << int(level);
    previous = mean;
    if (level >= 4) EXPECT_GE(chained, 27) << "level " << int(level);
  }
  // Level 1 is "is" and "is not" alone, solved without the position clues.
  einstein::generate(5, 1, puzzle, *work);
  EXPECT_EQ(puzzle.tier, einstein::Direct);
  for (int k = 0; k < puzzle.clueCount; ++k) EXPECT_LE(puzzle.clues[k].kind, einstein::Not);
}

TEST_F(EinsteinTest, ClueKindsHoldAsWorded) {
  uint8_t at[einstein::CATS_MAX][einstein::ITEMS_MAX] = {{0, 1, 2}, {2, 0, 1}, {1, 2, 0}};
  using einstein::ref;
  // Category 1: item 0 at 2, item 1 at 0, item 2 at 1. Category 2: item 0 at 1, item 1 at 2, item 2 at 0.
  EXPECT_TRUE(einstein::holds({einstein::Same, ref(1, 0), ref(2, 1), 0}, at));
  EXPECT_TRUE(einstein::holds({einstein::Same, ref(1, 0), ref(0, 2), 0}, at));
  EXPECT_FALSE(einstein::holds({einstein::Same, ref(1, 0), ref(2, 0), 0}, at));
  EXPECT_TRUE(einstein::holds({einstein::Not, ref(1, 0), ref(2, 0), 0}, at));
  EXPECT_TRUE(einstein::holds({einstein::NextTo, ref(1, 1), ref(2, 0), 0}, at));
  EXPECT_FALSE(einstein::holds({einstein::NextTo, ref(1, 1), ref(2, 1), 0}, at));
  EXPECT_TRUE(einstein::holds({einstein::LeftOf, ref(1, 1), ref(1, 2), 0}, at));
  EXPECT_FALSE(einstein::holds({einstein::LeftOf, ref(1, 2), ref(1, 1), 0}, at));
  EXPECT_TRUE(einstein::holds({einstein::Before, ref(1, 1), ref(1, 0), 0}, at));
  EXPECT_FALSE(einstein::holds({einstein::Before, ref(1, 0), ref(1, 1), 0}, at));
  EXPECT_TRUE(einstein::holds({einstein::Either, ref(1, 0), ref(2, 0), ref(2, 1)}, at));
  EXPECT_FALSE(einstein::holds({einstein::Either, ref(1, 0), ref(2, 0), ref(2, 2)}, at));
}

namespace {

// The item of category `cat` at position `pos`.
einstein::Ref itemAt(const einstein::Puzzle& p, const int cat, const int pos) {
  int i = 0;
  while (p.at[cat][i] != pos) ++i;
  return einstein::ref(cat, i);
}

}  // namespace

TEST_F(EinsteinTest, YesMarksAlongAChainSolveIt) {
  einstein::generate(21, 8, puzzle, *work);
  einstein::Joins joins(puzzle);
  // Each category joined only to the one before it, the first to the position.
  for (int pos = 0; pos < puzzle.items; ++pos) {
    for (int c = 1; c < puzzle.cats; ++c) {
      EXPECT_EQ(joins.verdict(), einstein::Verdict::Open);
      joins.join(itemAt(puzzle, c, pos), itemAt(puzzle, c - 1, pos));
    }
  }
  EXPECT_EQ(joins.verdict(), einstein::Verdict::Right);
}

TEST_F(EinsteinTest, AWholeWrongSolutionIsWrong) {
  einstein::generate(21, 3, puzzle, *work);
  einstein::Joins joins(puzzle);
  // The last category shifted by one position: a whole solution, but not this one.
  for (int pos = 0; pos < puzzle.items; ++pos) {
    for (int c = 1; c < puzzle.cats; ++c) {
      const int to = c == puzzle.cats - 1 ? (pos + 1) % puzzle.items : pos;
      joins.join(itemAt(puzzle, c, to), einstein::ref(einstein::POSITION, pos));
    }
  }
  EXPECT_EQ(joins.verdict(), einstein::Verdict::Wrong);
}

TEST_F(EinsteinTest, AStrayYesKeepsItOpenOrWrong) {
  einstein::generate(4, 1, puzzle, *work);
  einstein::Joins joins(puzzle);
  for (int pos = 0; pos < puzzle.items; ++pos) {
    for (int c = 1; c < puzzle.cats; ++c) joins.join(itemAt(puzzle, c, pos), einstein::ref(einstein::POSITION, pos));
  }
  ASSERT_EQ(joins.verdict(), einstein::Verdict::Right);
  // A false yes merges two positions' groups: no longer one item per category.
  joins.join(itemAt(puzzle, 1, 0), itemAt(puzzle, 2, 1));
  EXPECT_EQ(joins.verdict(), einstein::Verdict::Open);
}

TEST_F(EinsteinTest, GenerationIsQuickAtTheTopLevel) {
  const auto began = std::chrono::steady_clock::now();
  double worst = 0;
  for (uint32_t seed = 1; seed <= 50; ++seed) {
    const auto t = std::chrono::steady_clock::now();
    einstein::generate(einsteinSeed(seed, 10), 10, puzzle, *work);
    worst = std::max(worst, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count());
  }
  const double mean = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / 50;
  std::printf("[ logic grid ] level 10 on the host: %.2f ms mean, %.2f ms worst\n", mean, worst);
  EXPECT_LT(worst, 200.0);
}

// ---- seeds and levels -------------------------------------------------------------

TEST_F(Fresh, DailySeedIsStablePerGameAndSet) {
  EXPECT_EQ(play::dailySeed("sudoku", 12), play::dailySeed("sudoku", 12));
  EXPECT_NE(play::dailySeed("sudoku", 12), play::dailySeed("sudoku", 13));
  EXPECT_NE(play::dailySeed("sudoku", 12), play::dailySeed("arith", 12));
}

TEST_F(Fresh, PracticeSeedsDifferAndSurviveAReboot) {
  std::set<uint32_t> seeds;
  for (int i = 0; i < 5; ++i) seeds.insert(play::nextPracticeSeed("sudoku", 3));
  EXPECT_EQ(seeds.size(), 5u);
  EXPECT_EQ(play::standing("sudoku").practice, 5u);
  EXPECT_EQ(seeds.count(play::dailySeed("sudoku", 3)), 0u);
  // The counter lives on the card, so the sixth seed is new too.
  EXPECT_EQ(seeds.count(play::nextPracticeSeed("sudoku", 3)), 0u);
}

TEST(PlayLevels, AdaptMovesOneStepWithinBounds) {
  EXPECT_EQ(play::adapt(4, true), 5);
  EXPECT_EQ(play::adapt(4, false), 3);
  EXPECT_EQ(play::adapt(play::LEVEL_MAX, true), play::LEVEL_MAX);
  EXPECT_EQ(play::adapt(play::LEVEL_MIN, false), play::LEVEL_MIN);
}

TEST_F(Fresh, NewGameStartsAtTheStartLevel) {
  const play::Standing s = play::standing("arith");
  EXPECT_EQ(s.level, play::LEVEL_START);
  EXPECT_EQ(s.played, 0u);
  EXPECT_FALSE(play::dailyDone("arith", 1));
}

TEST_F(Fresh, DailyDayIsTodayElseTheLastDailyPlayed) {
  EXPECT_EQ(play::dailyDay(20405), 20405u);
  EXPECT_EQ(play::dailyDay(0), 0u);
  play::Result r;
  r.game = "sudoku";
  r.mode = play::Mode::Daily;
  r.daily = true;
  play::finish(r, 20404, 0);
  r.game = "arith";
  play::finish(r, 20403, 0);
  // With the clock unknown, today's daily stays the one last played.
  EXPECT_EQ(play::dailyDay(0), 20404u);
  EXPECT_TRUE(play::dailyDone("sudoku", play::dailyDay(0)));
  EXPECT_FALSE(play::dailyDone("arith", play::dailyDay(0)));
}

TEST_F(Fresh, FinishAdaptsCountsAndMarksTheDaily) {
  play::Result r;
  r.game = "sudoku";
  r.mode = play::Mode::Daily;
  r.daily = true;
  r.level = 3;
  r.won = true;
  EXPECT_EQ(play::finish(r, 7, 0), 4);
  EXPECT_TRUE(play::dailyDone("sudoku", 7));
  EXPECT_FALSE(play::dailyDone("sudoku", 8));

  // Practice adapts the level but does not count.
  r.mode = play::Mode::Practice;
  r.daily = false;
  r.level = 4;
  r.won = false;
  EXPECT_EQ(play::finish(r, 7, 0), 3);
  // A set game counts.
  r.mode = play::Mode::Set;
  r.level = 3;
  r.won = true;
  play::finish(r, 7, 0);

  const play::Standing s = play::standing("sudoku");
  EXPECT_EQ(s.level, 4);
  EXPECT_EQ(s.played, 2u);
  EXPECT_EQ(s.won, 2u);
  EXPECT_EQ(s.dailyDay, 7u);
  // Other games keep their own standing.
  EXPECT_EQ(play::standing("arith").level, play::LEVEL_START);
}

TEST_F(Fresh, DamagedRecordStartsOver) {
  play::Result r;
  r.game = "arith";
  r.level = 5;
  r.won = true;
  play::finish(r, 1, 0);
  fake::files[paths::PLAY_STATE]->bytes[20] ^= 0xFF;  // inside `played`
  EXPECT_EQ(play::standing("arith").level, play::LEVEL_START);
  play::finish(r, 1, 0);  // rewritten in the same slot
  EXPECT_EQ(play::standing("arith").level, 6);
  EXPECT_EQ(contents(paths::PLAY_STATE).size(), 32u);
}

TEST_F(Fresh, StandingsMatchOneByOne) {
  play::Result r;
  r.game = "sudoku";
  r.level = 4;
  r.won = true;
  r.daily = true;
  play::finish(r, 7, 0);
  r.game = "arith";
  play::finish(r, 7, 0);
  fake::files[paths::PLAY_STATE]->bytes[32 + 20] ^= 0xFF;  // arith's record, inside `played`
  const char* ids[] = {"arith", "chess", "sudoku"};
  play::Standing all[3];
  play::standings(ids, 3, all);
  for (int i = 0; i < 3; ++i) {
    const play::Standing one = play::standing(ids[i]);
    EXPECT_EQ(all[i].level, one.level) << ids[i];
    EXPECT_EQ(all[i].dailyDay, one.dailyDay) << ids[i];
    EXPECT_EQ(all[i].lang, one.lang) << ids[i];
  }
  EXPECT_EQ(all[2].dailyDay, 7u);
  EXPECT_EQ(all[0].level, play::LEVEL_START);  // damaged: starts over
}

// ---- play.jsonl and stats.json -----------------------------------------------------

TEST_F(Fresh, PlayLineFollowsDataMd) {
  play::Result r;
  r.game = "arith";
  r.mode = play::Mode::Set;
  r.level = 4;
  r.won = true;
  r.score = 85;
  r.ms = 61000;
  play::finish(r, 20405, 0);
  play::Result w;
  w.game = "words";
  w.lang = "hu";
  w.mode = play::Mode::Practice;
  w.level = 3;
  w.won = false;
  w.tries = 6;
  w.ms = 95000;
  play::finish(w, 12, 1793714400);
  const auto out = lines(paths::PLAY_LOG);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], R"({"game":"arith","mode":"set","level":4,"won":true,"score":85,"ms":61000,"day":20405,"at":null})");
  EXPECT_EQ(
      out[1],
      R"({"game":"words","lang":"hu","mode":"practice","level":3,"won":false,"tries":6,"ms":95000,"day":12,"at":1793714400})");
}

TEST_F(Fresh, PlayLogIsAppendedAfterThePullRenamedIt) {
  play::Result r;
  r.game = "arith";
  play::finish(r, 1, 0);
  ASSERT_TRUE(Storage.rename(paths::PLAY_LOG, "/x4/play.pulling"));
  play::finish(r, 1, 0);
  EXPECT_EQ(lines(paths::PLAY_LOG).size(), 1u);
  EXPECT_EQ(lines("/x4/play.pulling").size(), 1u);
}

TEST_F(Fresh, StatsCarryPlay) {
  play::Result r;
  r.game = "sudoku";
  r.mode = play::Mode::Daily;
  r.daily = true;
  r.level = 4;
  r.won = true;
  play::finish(r, 2, 0);
  LearnState state{};
  state.retention = -1.0f;
  ASSERT_TRUE(writeStats(state, nullptr, &play::addStats));
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, contents(paths::STATS)));
  EXPECT_EQ(doc["play"]["sudoku"]["level"], 5);
  EXPECT_EQ(doc["play"]["sudoku"]["played"], 1);
  EXPECT_EQ(doc["play"]["sudoku"]["won"], 1);
}

// ---- the game in progress ----------------------------------------------------------

TEST_F(Fresh, SavedGameRoundTrips) {
  play::Saved saved;
  saved.mode = play::Mode::Daily;
  saved.daily = true;
  saved.level = 6;
  saved.day = 9;
  saved.ms = 1234;
  saved.size = 5;
  const uint8_t state[5] = {1, 2, 3, 4, 5};
  ASSERT_TRUE(play::saveGame("sudoku", saved, state));
  play::Saved back;
  uint8_t buffer[play::SAVE_MAX] = {};
  ASSERT_TRUE(play::loadGame("sudoku", back, buffer));
  EXPECT_EQ(back.mode, play::Mode::Daily);
  EXPECT_TRUE(back.daily);
  EXPECT_EQ(back.level, 6);
  EXPECT_EQ(back.day, 9u);
  EXPECT_EQ(back.ms, 1234u);
  EXPECT_EQ(back.size, 5);
  EXPECT_EQ(std::memcmp(buffer, state, 5), 0);
  EXPECT_FALSE(play::loadGame("arith", back, buffer));

  play::clearGame("sudoku");
  EXPECT_FALSE(play::loadGame("sudoku", back, buffer));
}

TEST_F(Fresh, DamagedSavedGameIsIgnored) {
  play::Saved saved;
  saved.size = 3;
  const uint8_t state[3] = {7, 8, 9};
  ASSERT_TRUE(play::saveGame("arith", saved, state));
  fake::files["/x4/state/play-arith.bin"]->bytes.back() ^= 0x01;
  play::Saved back;
  uint8_t buffer[play::SAVE_MAX] = {};
  EXPECT_FALSE(play::loadGame("arith", back, buffer));
}

TEST_F(Fresh, SaveCutBeforeTheRenameIsRecovered) {
  play::Saved saved;
  saved.size = 2;
  const uint8_t first[2] = {1, 1};
  ASSERT_TRUE(play::saveGame("words", saved, first));
  // replaceFile() removed the old save, then the power went before the rename.
  const uint8_t second[2] = {2, 2};
  fake::failRename = 0;
  EXPECT_FALSE(play::saveGame("words", saved, second));
  EXPECT_FALSE(Storage.exists("/x4/state/play-words.bin"));
  play::Saved back;
  uint8_t buffer[play::SAVE_MAX] = {};
  ASSERT_TRUE(play::loadGame("words", back, buffer));
  EXPECT_EQ(buffer[0], 2);

  // The next save replaces both; a finished game leaves neither.
  const uint8_t third[2] = {3, 3};
  ASSERT_TRUE(play::saveGame("words", saved, third));
  ASSERT_TRUE(play::loadGame("words", back, buffer));
  EXPECT_EQ(buffer[0], 3);
  play::clearGame("words");
  EXPECT_FALSE(play::loadGame("words", back, buffer));
}

TEST_F(Fresh, TornSaveKeepsTheLastOne) {
  play::Saved saved;
  saved.size = 2;
  const uint8_t first[2] = {1, 1};
  ASSERT_TRUE(play::saveGame("sudoku", saved, first));
  const uint8_t second[2] = {2, 2};
  fake::shortWrite = 1;  // the state after the header
  EXPECT_FALSE(play::saveGame("sudoku", saved, second));
  play::Saved back;
  uint8_t buffer[play::SAVE_MAX] = {};
  ASSERT_TRUE(play::loadGame("sudoku", back, buffer));
  EXPECT_EQ(buffer[0], 1);

  // The power went mid-write: half a .tmp next to the last save.
  auto torn = std::make_shared<fake::Node>(*fake::files["/x4/state/play-sudoku.bin"]);
  torn->bytes.resize(torn->bytes.size() - 1);
  fake::files["/x4/state/play-sudoku.bin.tmp"] = torn;
  ASSERT_TRUE(play::loadGame("sudoku", back, buffer));
  EXPECT_EQ(buffer[0], 1);
}

// ---- saving while playing ----------------------------------------------------------

TEST(Autosave, WaitsForAPause) {
  play::Autosave a;
  EXPECT_FALSE(a.due(10000, true));
  a.changed(1000);
  EXPECT_FALSE(a.due(1000 + play::Autosave::IDLE_MS - 1, true));
  a.changed(2500);  // another move restarts the wait
  EXPECT_FALSE(a.due(1000 + play::Autosave::IDLE_MS, true));
  EXPECT_TRUE(a.due(2500 + play::Autosave::IDLE_MS, true));
  a.saved();
  EXPECT_FALSE(a.due(60000, true));
}

TEST(Autosave, SteadyPlayStillSaves) {
  play::Autosave a;
  uint32_t now = 0;
  for (; now < play::Autosave::BUSY_MS; now += 500) {
    a.changed(now);
    EXPECT_FALSE(a.due(now, true));
  }
  a.changed(now);
  EXPECT_TRUE(a.due(now, true));
}

TEST(Autosave, WaitsForTheRoundToEnd) {
  play::Autosave a;
  a.changed(0);
  EXPECT_FALSE(a.due(play::Autosave::BUSY_MS * 2, false));
  EXPECT_TRUE(a.due(play::Autosave::BUSY_MS * 2, true));
}

TEST(Autosave, SurvivesTheClockWrapping) {
  play::Autosave a;
  a.changed(0xFFFFFF00u);
  EXPECT_FALSE(a.due(0x100u, true));
  EXPECT_TRUE(a.due(0xFFFFFF00u + play::Autosave::IDLE_MS, true));
}

// ---- arithmetic ----------------------------------------------------------------

TEST(MemoryGames, ArithChainIsConsistent) {
  for (uint8_t level = 1; level <= 10; ++level) {
    const memory::ArithLevel& spec = memory::arithLevel(level);
    std::set<char> ops;
    int undone = 0;  // steps that cancel the last one; only when nothing else fits
    for (uint32_t seed = 1; seed <= 500; ++seed) {
      memory::ArithChain chain;
      memory::arithMake(seed * 101u + level, level, chain);
      ASSERT_EQ(chain.count, spec.count);
      EXPECT_GE(chain.start, 100);
      EXPECT_LE(chain.start, 999);
      int16_t value = chain.start;
      for (uint8_t i = 0; i < chain.count; ++i) {
        const memory::ArithStep& step = chain.steps[i];
        ops.insert(step.op);
        if (i > 0) {
          const memory::ArithStep& last = chain.steps[i - 1];
          EXPECT_NE(step.op, last.op);
          const std::string pair{last.op, step.op};
          if (step.value == last.value && (pair == "+-" || pair == "-+" || pair == "x/" || pair == "/x")) ++undone;
        }
        if (step.op == '+' || step.op == '-') {
          EXPECT_GE(step.value, spec.addMin);
          EXPECT_LE(step.value, spec.addMax);
        } else {
          ASSERT_TRUE(step.op == 'x' || step.op == '/');
          EXPECT_GE(step.value, spec.mulMin);
          EXPECT_LE(step.value, spec.mulMax);
          if (step.op == '/') EXPECT_EQ(value % step.value, 0);
        }
        value = memory::arithApply(value, step);
        EXPECT_GE(value, 1);
        EXPECT_LE(value, 999);
      }
      EXPECT_EQ(value, chain.result);
    }
    EXPECT_EQ(ops.size(), 4u) << "level " << int(level);
    EXPECT_LE(undone, 5) << "level " << int(level);
  }
  EXPECT_EQ(memory::arithLevel(1).count, 6);
  EXPECT_EQ(memory::arithLevel(10).count, memory::CHAIN_MAX);
  EXPECT_EQ(memory::CHAIN_MAX, 12);
  for (uint8_t level = 2; level <= 10; ++level) {
    const memory::ArithLevel& easier = memory::arithLevel(level - 1);
    const memory::ArithLevel& harder = memory::arithLevel(level);
    EXPECT_GE(harder.count, easier.count);
    EXPECT_GE(harder.addMax, easier.addMax);
    EXPECT_GE(harder.mulMax, easier.mulMax);
    EXPECT_GT(harder.count + harder.addMax + harder.mulMax + harder.addMin + harder.mulMin,
              easier.count + easier.addMax + easier.mulMax + easier.addMin + easier.mulMin);
  }
}

TEST(MemoryGames, SameSeedSameChain) {
  memory::ArithChain a;
  memory::ArithChain b;
  memory::arithMake(77, 6, a);
  memory::arithMake(77, 6, b);
  EXPECT_EQ(a.start, b.start);
  EXPECT_EQ(a.result, b.result);
  for (uint8_t i = 0; i < a.count; ++i) {
    EXPECT_EQ(a.steps[i].op, b.steps[i].op);
    EXPECT_EQ(a.steps[i].value, b.steps[i].value);
  }
}

