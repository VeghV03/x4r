#include "Sudoku.h"

#include <cstring>

#include "play/Rng.h"

namespace x4::play::sudoku {

namespace {

constexpr uint16_t ALL = 0x3FE;  // bits 1..9

struct Tables {
  uint8_t unit[27][9];   // rows, columns, boxes
  uint8_t unitsOf[CELLS][3];
  uint8_t peers[CELLS][20];
};

constexpr Tables makeTables() {
  Tables t{};
  for (int i = 0; i < CELLS; ++i) {
    const int r = i / 9;
    const int c = i % 9;
    const int b = (r / 3) * 3 + c / 3;
    t.unitsOf[i][0] = static_cast<uint8_t>(r);
    t.unitsOf[i][1] = static_cast<uint8_t>(9 + c);
    t.unitsOf[i][2] = static_cast<uint8_t>(18 + b);
  }
  for (int u = 0; u < 9; ++u) {
    for (int k = 0; k < 9; ++k) {
      t.unit[u][k] = static_cast<uint8_t>(u * 9 + k);
      t.unit[9 + u][k] = static_cast<uint8_t>(k * 9 + u);
      t.unit[18 + u][k] = static_cast<uint8_t>(((u / 3) * 3 + k / 3) * 9 + (u % 3) * 3 + k % 3);
    }
  }
  for (int i = 0; i < CELLS; ++i) {
    int n = 0;
    for (int j = 0; j < CELLS; ++j) {
      const bool row = i / 9 == j / 9;
      const bool col = i % 9 == j % 9;
      const bool box = (i / 27) * 3 + (i % 9) / 3 == (j / 27) * 3 + (j % 9) / 3;
      if (i != j && (row || col || box)) t.peers[i][n++] = static_cast<uint8_t>(j);
    }
  }
  return t;
}

constexpr Tables T = makeTables();

inline int bits(uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}

inline int lowest(const uint32_t v) {
  int d = 0;
  while (!(v & (1u << d))) ++d;
  return d;
}

inline int rowOf(const int i) { return i / 9; }
inline int colOf(const int i) { return i % 9; }
inline int boxOf(const int i) { return (i / 27) * 3 + (i % 9) / 3; }
inline bool sees(const int a, const int b) {
  return a != b && (rowOf(a) == rowOf(b) || colOf(a) == colOf(b) || boxOf(a) == boxOf(b));
}

// ---- backtracking ------------------------------------------------------------

// Depth-first search with the most constrained cell first. Iterative, so the
// stack depth stays flat. Fills digits in random order when `rng` is set.
int search(uint8_t* g, const int limit, Work& w, uint8_t* first, const uint32_t maxNodes, Rng* rng) {
  uint16_t used[27] = {};
  for (int i = 0; i < CELLS; ++i) {
    if (!g[i]) continue;
    const uint16_t bit = 1u << g[i];
    for (const uint8_t u : T.unitsOf[i]) {
      if (used[u] & bit) return 0;
      used[u] |= bit;
    }
  }
  const auto freeOf = [&](const int i) {
    return static_cast<uint16_t>(ALL & ~(used[T.unitsOf[i][0]] | used[T.unitsOf[i][1]] | used[T.unitsOf[i][2]]));
  };
  // Picks the empty cell with the fewest options; false when none is empty.
  const auto pick = [&](uint8_t& cell, uint16_t& left) {
    int best = -1;
    int bestCount = 10;
    uint16_t bestMask = 0;
    for (int i = 0; i < CELLS && bestCount > 1; ++i) {
      if (g[i]) continue;
      const uint16_t m = freeOf(i);
      const int n = bits(m);
      if (n < bestCount) {
        best = i;
        bestCount = n;
        bestMask = m;
      }
    }
    if (best < 0) return false;
    cell = static_cast<uint8_t>(best);
    left = bestMask;
    return true;
  };
  const auto set = [&](const int i, const int d) {
    g[i] = static_cast<uint8_t>(d);
    for (const uint8_t u : T.unitsOf[i]) used[u] |= static_cast<uint16_t>(1u << d);
  };
  const auto clear = [&](const int i) {
    const uint16_t bit = static_cast<uint16_t>(1u << g[i]);
    for (const uint8_t u : T.unitsOf[i]) used[u] &= static_cast<uint16_t>(~bit);
    g[i] = 0;
  };

  int count = 0;
  if (!pick(w.stackCell[0], w.stackLeft[0])) {
    if (first) std::memcpy(first, g, CELLS);
    return 1;
  }
  int sp = 1;
  uint32_t nodes = 0;
  while (sp > 0) {
    const int top = sp - 1;
    const uint8_t cell = w.stackCell[top];
    if (g[cell]) clear(cell);
    uint16_t& left = w.stackLeft[top];
    if (!left) {
      --sp;
      continue;
    }
    int d = lowest(left);
    if (rng) {
      for (uint32_t skip = rng->below(static_cast<uint32_t>(bits(left))); skip > 0; --skip) {
        d = lowest(left & ~((2u << d) - 1));
      }
    }
    left &= static_cast<uint16_t>(~(1u << d));
    set(cell, d);
    if (++nodes > maxNodes) return -1;
    uint8_t nextCell = 0;
    uint16_t nextLeft = 0;
    if (!pick(nextCell, nextLeft)) {
      if (++count == 1 && first) std::memcpy(first, g, CELLS);
      if (count >= limit) return count;
      continue;
    }
    if (!nextLeft) continue;
    w.stackCell[sp] = nextCell;
    w.stackLeft[sp] = nextLeft;
    ++sp;
  }
  return count;
}

// ---- human techniques ----------------------------------------------------------

class Solver {
 public:
  explicit Solver(Work& w) : g(w.grid), cand(w.cand) {}

  // False on a contradiction in the givens.
  bool load(const uint8_t (&grid)[CELLS]) {
    std::memset(g, 0, CELLS);
    for (int i = 0; i < CELLS; ++i) cand[i] = ALL;
    for (int i = 0; i < CELLS; ++i) {
      if (grid[i] && !place(i, grid[i])) return false;
    }
    return true;
  }

  int8_t solve(const int8_t maxTier) {
    int8_t hardest = NakedSingle;
    for (;;) {
      bool done = true;
      for (int i = 0; i < CELLS; ++i) {
        if (g[i]) continue;
        if (!cand[i]) return Unsolved;
        done = false;
      }
      if (done) return hardest;
      int8_t used = Unsolved;
      if (nakedSingle()) {
        used = NakedSingle;
      } else if (maxTier >= HiddenSingle && hiddenSingle()) {
        used = HiddenSingle;
      } else if (maxTier >= LockedCandidates && lockedCandidates()) {
        used = LockedCandidates;
      } else if (maxTier >= Pair && (nakedSubset(2) || hiddenSubset(2))) {
        used = Pair;
      } else if (maxTier >= Triple && (nakedSubset(3) || hiddenSubset(3))) {
        used = Triple;
      } else if (maxTier >= XWing && fish(2)) {
        used = XWing;
      } else if (maxTier >= Advanced && (xyWing() || fish(3))) {
        used = Advanced;
      }
      if (used == Unsolved) return Unsolved;
      if (used > hardest) hardest = used;
    }
  }

 private:
  uint8_t* g;
  uint16_t* cand;

  bool place(const int i, const int d) {
    const uint16_t bit = static_cast<uint16_t>(1u << d);
    if (!(cand[i] & bit)) return false;
    g[i] = static_cast<uint8_t>(d);
    cand[i] = 0;
    for (const uint8_t u : T.unitsOf[i]) {
      for (const uint8_t j : T.unit[u]) cand[j] &= static_cast<uint16_t>(~bit);
    }
    return true;
  }

  bool eliminate(const int i, const uint16_t mask) {
    if (g[i] || !(cand[i] & mask)) return false;
    cand[i] &= static_cast<uint16_t>(~mask);
    return true;
  }

  bool nakedSingle() {
    for (int i = 0; i < CELLS; ++i) {
      if (!g[i] && bits(cand[i]) == 1) return place(i, lowest(cand[i]));
    }
    return false;
  }

  bool hiddenSingle() {
    for (const auto& unit : T.unit) {
      for (int d = 1; d <= 9; ++d) {
        int at = -1;
        int n = 0;
        for (const uint8_t i : unit) {
          if (cand[i] & (1u << d)) {
            at = i;
            ++n;
          }
        }
        if (n == 1) return place(at, d);
      }
    }
    return false;
  }

  bool lockedCandidates() {
    bool changed = false;
    for (int d = 1; d <= 9; ++d) {
      const uint16_t bit = static_cast<uint16_t>(1u << d);
      // Pointing: within a box the digit sits on one line.
      for (int b = 0; b < 9; ++b) {
        uint16_t rows = 0;
        uint16_t cols = 0;
        for (const uint8_t i : T.unit[18 + b]) {
          if (cand[i] & bit) {
            rows |= static_cast<uint16_t>(1u << rowOf(i));
            cols |= static_cast<uint16_t>(1u << colOf(i));
          }
        }
        if (bits(rows) == 1) {
          for (const uint8_t i : T.unit[lowest(rows)]) {
            if (boxOf(i) != b) changed |= eliminate(i, bit);
          }
        }
        if (bits(cols) == 1) {
          for (const uint8_t i : T.unit[9 + lowest(cols)]) {
            if (boxOf(i) != b) changed |= eliminate(i, bit);
          }
        }
        if (changed) return true;
      }
      // Claiming: within a line the digit sits in one box.
      for (int line = 0; line < 18; ++line) {
        uint16_t boxes = 0;
        for (const uint8_t i : T.unit[line]) {
          if (cand[i] & bit) boxes |= static_cast<uint16_t>(1u << boxOf(i));
        }
        if (bits(boxes) != 1) continue;
        for (const uint8_t i : T.unit[18 + lowest(boxes)]) {
          const bool onLine = line < 9 ? rowOf(i) == line : colOf(i) == line - 9;
          if (!onLine) changed |= eliminate(i, bit);
        }
        if (changed) return true;
      }
    }
    return false;
  }

  // k cells of a unit holding only k digits between them.
  bool nakedSubset(const int k) {
    for (const auto& unit : T.unit) {
      uint8_t cells[9];
      int n = 0;
      for (const uint8_t i : unit) {
        const int c = bits(cand[i]);
        if (!g[i] && c >= 2 && c <= k) cells[n++] = i;
      }
      if (n < k) continue;
      for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
          if (k == 2) {
            if (tryNaked(unit, cand[cells[a]] | cand[cells[b]], 2, cells[a], cells[b], 0xFF)) return true;
            continue;
          }
          for (int c = b + 1; c < n; ++c) {
            if (tryNaked(unit, cand[cells[a]] | cand[cells[b]] | cand[cells[c]], 3, cells[a], cells[b], cells[c])) {
              return true;
            }
          }
        }
      }
    }
    return false;
  }

  bool tryNaked(const uint8_t (&unit)[9], const uint16_t mask, const int k, const uint8_t a, const uint8_t b,
                const uint8_t c) {
    if (bits(mask) != k) return false;
    bool changed = false;
    for (const uint8_t i : unit) {
      if (i != a && i != b && i != c) changed |= eliminate(i, mask);
    }
    return changed;
  }

  // k digits of a unit confined to k cells.
  bool hiddenSubset(const int k) {
    for (const auto& unit : T.unit) {
      uint16_t where[10] = {};
      uint8_t digits[9];
      int n = 0;
      for (int d = 1; d <= 9; ++d) {
        for (int p = 0; p < 9; ++p) {
          if (cand[unit[p]] & (1u << d)) where[d] |= static_cast<uint16_t>(1u << p);
        }
        const int c = bits(where[d]);
        if (c >= 2 && c <= k) digits[n++] = static_cast<uint8_t>(d);
      }
      if (n < k) continue;
      for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
          if (k == 2) {
            const uint16_t keep = static_cast<uint16_t>((1u << digits[a]) | (1u << digits[b]));
            if (tryHidden(unit, where[digits[a]] | where[digits[b]], keep, 2)) return true;
            continue;
          }
          for (int c = b + 1; c < n; ++c) {
            const uint16_t keep =
                static_cast<uint16_t>((1u << digits[a]) | (1u << digits[b]) | (1u << digits[c]));
            if (tryHidden(unit, where[digits[a]] | where[digits[b]] | where[digits[c]], keep, 3)) return true;
          }
        }
      }
    }
    return false;
  }

  bool tryHidden(const uint8_t (&unit)[9], const uint16_t positions, const uint16_t keep, const int k) {
    if (bits(positions) != k) return false;
    bool changed = false;
    for (int p = 0; p < 9; ++p) {
      if (positions & (1u << p)) changed |= eliminate(unit[p], static_cast<uint16_t>(ALL & ~keep));
    }
    return changed;
  }

  // X-wing (k = 2) and swordfish (k = 3), on rows and on columns.
  bool fish(const int k) {
    for (int d = 1; d <= 9; ++d) {
      const uint16_t bit = static_cast<uint16_t>(1u << d);
      for (int byCol = 0; byCol < 2; ++byCol) {
        uint16_t spots[9];
        uint8_t lines[9];
        int n = 0;
        for (int line = 0; line < 9; ++line) {
          uint16_t m = 0;
          for (int p = 0; p < 9; ++p) {
            if (cand[byCol ? p * 9 + line : line * 9 + p] & bit) m |= static_cast<uint16_t>(1u << p);
          }
          const int c = bits(m);
          if (c >= 2 && c <= k) {
            spots[n] = m;
            lines[n++] = static_cast<uint8_t>(line);
          }
        }
        for (int a = 0; a < n; ++a) {
          for (int b = a + 1; b < n; ++b) {
            if (k == 2) {
              if (tryFish(byCol, bit, spots[a] | spots[b], 2, (1u << lines[a]) | (1u << lines[b]))) return true;
              continue;
            }
            for (int c = b + 1; c < n; ++c) {
              if (tryFish(byCol, bit, spots[a] | spots[b] | spots[c], 3,
                          (1u << lines[a]) | (1u << lines[b]) | (1u << lines[c]))) {
                return true;
              }
            }
          }
        }
      }
    }
    return false;
  }

  bool tryFish(const int byCol, const uint16_t bit, const uint16_t cover, const int k, const uint32_t baseLines) {
    if (bits(cover) != k) return false;
    bool changed = false;
    for (int p = 0; p < 9; ++p) {
      if (!(cover & (1u << p))) continue;
      for (int line = 0; line < 9; ++line) {
        if (baseLines & (1u << line)) continue;
        changed |= eliminate(byCol ? p * 9 + line : line * 9 + p, bit);
      }
    }
    return changed;
  }

  bool xyWing() {
    for (int p = 0; p < CELLS; ++p) {
      if (g[p] || bits(cand[p]) != 2) continue;
      for (const uint8_t a : T.peers[p]) {
        if (g[a] || bits(cand[a]) != 2) continue;
        const uint16_t shared = cand[a] & cand[p];
        if (bits(shared) != 1) continue;
        const uint16_t z = cand[a] & static_cast<uint16_t>(~cand[p]);
        const uint16_t want = static_cast<uint16_t>((cand[p] & ~shared) | z);
        for (const uint8_t b : T.peers[p]) {
          if (b == a || g[b] || cand[b] != want || sees(a, b)) continue;
          bool changed = false;
          for (const uint8_t i : T.peers[a]) {
            if (i != p && sees(i, b)) changed |= eliminate(i, z);
          }
          if (changed) return true;
        }
      }
    }
    return false;
  }
};

// Levels: the technique band, and a floor of clues that keeps the easy ones easy.
constexpr LevelSpec SPECS[10] = {
    {NakedSingle, NakedSingle, 46},
    {NakedSingle, NakedSingle, 38},
    {NakedSingle, HiddenSingle, 32},
    {HiddenSingle, HiddenSingle, 30},
    {HiddenSingle, HiddenSingle, 0},
    {LockedCandidates, LockedCandidates, 0},
    {Pair, Triple, 0},
    {XWing, Advanced, 0},
    {Advanced, Trial, 0},
    {Trial, Trial, 0},
};

// Backtracking budget of one uniqueness check.
constexpr uint32_t TRIAL_NODES = 20000;

// Generator attempts per puzzle before the closest one is taken. On a desktop
// core a puzzle takes 6-11 ms on average at levels 7-10 (44 ms worst) and
// 97-100% land in their band; expect roughly 100x that on the X4.
#ifndef X4_SUDOKU_ATTEMPTS
#define X4_SUDOKU_ATTEMPTS 32
#endif
constexpr int ATTEMPTS = X4_SUDOKU_ATTEMPTS;

}  // namespace

LevelSpec levelSpec(const uint8_t level) {
  const int i = level < 1 ? 0 : level > 10 ? 9 : level - 1;
  return SPECS[i];
}

int countSolutions(const uint8_t (&grid)[CELLS], const int limit, Work& work, uint8_t* first,
                   const uint32_t maxNodes) {
  uint8_t g[CELLS];
  std::memcpy(g, grid, CELLS);
  return search(g, limit, work, first, maxNodes, nullptr);
}

int8_t grade(const uint8_t (&grid)[CELLS], const int8_t maxTier, Work& work) {
  Solver solver(work);
  if (!solver.load(grid)) return Unsolved;
  const int8_t tier = solver.solve(maxTier < Advanced ? maxTier : Advanced);
  if (tier != Unsolved || maxTier < Trial) return tier;
  return countSolutions(grid, 2, work, nullptr, TRIAL_NODES) == 1 ? Trial : Unsolved;
}

void generate(const uint32_t seed, const uint8_t level, Puzzle& out, Work& work, void (*between)()) {
  const LevelSpec spec = levelSpec(level);
  out = Puzzle{};
  out.tier = Unsolved;
  Puzzle& trial = work.trial;
  for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
    if (between && attempt > 0) between();
    Rng rng(seed ^ mix32(0x9E3779B9u * static_cast<uint32_t>(attempt + 1)));
    std::memset(work.grid, 0, CELLS);
    search(work.grid, 1, work, trial.solution, 1u << 20, &rng);
    std::memcpy(trial.givens, trial.solution, CELLS);

    // Clears cells in symmetric pairs (180 degrees), in random order, while
    // the puzzle stays solvable within the level's techniques.
    for (int i = 0; i <= 40; ++i) work.order[i] = static_cast<uint8_t>(i);
    rng.shuffle(work.order, 41);
    int clues = CELLS;
    for (int k = 0; k <= 40; ++k) {
      const int a = work.order[k];
      const int b = CELLS - 1 - a;
      const int removed = a == b ? 1 : 2;
      if (clues - removed < spec.minClues) continue;
      const uint8_t va = trial.givens[a];
      const uint8_t vb = trial.givens[b];
      trial.givens[a] = 0;
      trial.givens[b] = 0;
      if (grade(trial.givens, spec.maxTier, work) == Unsolved) {
        trial.givens[a] = va;
        trial.givens[b] = vb;
      } else {
        clues -= removed;
      }
    }
    // Then single cells: fewer clues, harder puzzles, at some cost in symmetry.
    for (int i = 0; i < CELLS; ++i) work.order[i] = static_cast<uint8_t>(i);
    rng.shuffle(work.order, CELLS);
    for (int k = 0; k < CELLS && clues - 1 >= spec.minClues; ++k) {
      const int a = work.order[k];
      const uint8_t va = trial.givens[a];
      if (!va) continue;
      trial.givens[a] = 0;
      if (grade(trial.givens, spec.maxTier, work) == Unsolved) {
        trial.givens[a] = va;
      } else {
        --clues;
      }
    }
    trial.tier = grade(trial.givens, spec.maxTier, work);
    trial.clues = static_cast<uint8_t>(clues);
    // Keep the attempt closest to the band (harder wins below it).
    if (out.tier == Unsolved || trial.tier > out.tier) out = trial;
    if (trial.tier >= spec.minTier) return;
  }
}

}  // namespace x4::play::sudoku
