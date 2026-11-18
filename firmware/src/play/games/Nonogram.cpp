#include "Nonogram.h"

#include <cstring>

#include "play/Rng.h"

namespace x4::play::nonogram {

namespace {

constexpr uint8_t SIZES[10] = {5, 6, 7, 8, 9, 11, 12, 13, 14, 15};
// Solvable pictures drawn per puzzle; the one that takes the most sweeps wins.
constexpr uint8_t CANDIDATES[10] = {1, 1, 1, 2, 2, 3, 3, 4, 5, 6};

// Pictures drawn per puzzle at most, and cells touched per picture to make it solve.
constexpr int ATTEMPTS = 32;
constexpr int TOUCHES_PER_CELL = 8;  // budget: size*size / this
// Grids from this side up are smoothed into blobs; smaller ones stay pixel sprites.
constexpr int SMOOTH_FROM = 9;

// ---- the line solver -----------------------------------------------------------

inline uint32_t bit(const int i) { return 1u << i; }

// A run of `len` fits at `p`: no known empty cell under it, and the cell after
// it (if any) may be empty.
inline bool fits(const uint8_t* line, const int n, const int p, const int len) {
  if (p + len > n) return false;
  for (int i = p; i < p + len; ++i) {
    if (line[i] == Empty) return false;
  }
  return p + len == n || line[p + len] != Filled;
}

// ---- pictures ------------------------------------------------------------------

// Filled cells in the 3x3 block around (r, c), itself included; off the grid counts empty.
int block(const uint8_t* g, const int n, const int r, const int c) {
  int sum = 0;
  for (int dr = -1; dr <= 1; ++dr) {
    for (int dc = -1; dc <= 1; ++dc) {
      const int rr = r + dr;
      const int cc = c + dc;
      if (rr >= 0 && rr < n && cc >= 0 && cc < n) sum += g[rr * n + cc] ? 1 : 0;
    }
  }
  return sum;
}

// A picture: noise, two times in three mirrored left to right like a mask, a
// creature or a vase; on the larger grids smoothed once by majority vote into
// blobs. `tmp` is size*size scratch.
void drawPicture(Rng& rng, const int n, const int level, uint8_t* out, uint8_t* tmp) {
  const bool mirror = rng.below(3) != 0;
  // Percent ink before smoothing; less ink at higher levels, so more and shorter runs.
  const int density = 56 - 10 * (level - 1) / 9 + static_cast<int>(rng.below(12));
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) {
      const bool copy = mirror && c >= (n + 1) / 2;
      out[r * n + c] = copy ? out[r * n + (n - 1 - c)] : rng.below(100) < static_cast<uint32_t>(density);
    }
  }
  if (n < SMOOTH_FROM) return;
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n; ++c) tmp[r * n + c] = block(out, n, r, c) >= 5 ? 1 : 0;
  }
  std::memcpy(out, tmp, static_cast<size_t>(n * n));
}

// Pictures worth solving: some ink but not a slab, few blank lines, and clues
// short enough to sit beside the grid.
bool fair(const uint8_t* g, const int n) {
  int filled = 0;
  int blankLines = 0;
  for (int line = 0; line < 2 * n; ++line) {
    int inLine = 0;
    int runs = 0;
    for (int k = 0; k < n; ++k) {
      const bool on = g[cellOf(line, k, n)];
      inLine += on ? 1 : 0;
      if (on && (k == 0 || !g[cellOf(line, k - 1, n)])) ++runs;
    }
    if (runs > RUNS_SHOWN) return false;
    if (line < n) filled += inLine;
    if (inLine == 0) ++blankLines;
  }
  const int cells = n * n;
  return filled * 100 >= cells * 35 && filled * 100 <= cells * 75 && blankLines <= (n + 2) / 5;
}

bool mirrored(const uint8_t* g, const int n) {
  for (int r = 0; r < n; ++r) {
    for (int c = 0; c < n / 2; ++c) {
      if (g[r * n + c] != g[r * n + n - 1 - c]) return false;
    }
  }
  return true;
}

// Touches `g` up until the line solver finishes it, within a budget: a cell
// the solver could not decide is filled (with its mirror image, so a symmetric
// picture stays symmetric). Returns the sweeps the solve took, or -1.
int makeSolvable(uint8_t* g, const int n, Rng& rng, Work& work, uint8_t& touches) {
  const int cells = n * n;
  const bool symmetric = mirrored(g, n);
  const auto set = [&](const int cell, const uint8_t v) {
    g[cell] = v;
    if (symmetric) g[(cell / n) * n + n - 1 - cell % n] = v;
  };
  const int budget = cells / TOUCHES_PER_CELL;
  for (int touch = 0; touch <= budget; ++touch) {
    if (!fair(g, n)) return -1;
    cluesOf(g, n, work.clues);
    std::memset(work.state, Unknown, static_cast<size_t>(cells));
    int sweeps = 0;
    const int unknown = solve(work.clues, n, work.state, &sweeps);
    if (unknown == 0) {
      touches = static_cast<uint8_t>(touch);
      return sweeps;
    }
    if (unknown < 0) return -1;  // cannot happen: the clues come from a picture
    int pick = static_cast<int>(rng.below(static_cast<uint32_t>(unknown)));
    int cell = 0;
    while (work.state[cell] != Unknown || pick-- > 0) ++cell;
    if (!g[cell]) {
      set(cell, 1);
      continue;
    }
    // Already ink: fill the next undecided blank instead, else clear this one.
    int blank = -1;
    for (int i = 1; i < cells && blank < 0; ++i) {
      const int j = (cell + i) % cells;
      if (work.state[j] == Unknown && !g[j]) blank = j;
    }
    if (blank >= 0) {
      set(blank, 1);
    } else {
      set(cell, 0);
    }
  }
  return -1;
}

}  // namespace

uint8_t sizeFor(const uint8_t level) {
  const int i = level < 1 ? 0 : level > 10 ? 9 : level - 1;
  return SIZES[i];
}

void cluesOf(const uint8_t* cells, const int size, Clues& out) {
  for (int line = 0; line < 2 * size; ++line) {
    uint8_t count = 0;
    uint8_t run = 0;
    for (int k = 0; k <= size; ++k) {
      if (k < size && cells[cellOf(line, k, size)]) {
        ++run;
      } else if (run) {
        out.runs[line][count++] = run;
        run = 0;
      }
    }
    out.count[line] = count;
  }
}

bool solveLine(uint8_t* line, const int n, const uint8_t* runs, const int k) {
  // ahead[j] bit i: cells [0, i) hold runs 0..j-1 (each with the empty cell
  // after it), consistent with what is known. behind[j] bit i: cells [i, n)
  // hold runs j..k-1. Bit positions run 0..n, so n stays under 32.
  uint32_t ahead[RUNS_MAX + 1] = {};
  uint32_t behind[RUNS_MAX + 1] = {};
  const auto after = [n](const int end) { return end < n ? end + 1 : n; };

  ahead[0] = bit(0);
  for (int j = 0; j <= k; ++j) {
    for (int i = 0; i < n; ++i) {
      if (!(ahead[j] & bit(i))) continue;
      if (line[i] != Filled) ahead[j] |= bit(i + 1);
      if (j < k && fits(line, n, i, runs[j])) ahead[j + 1] |= bit(after(i + runs[j]));
    }
  }
  if (!(ahead[k] & bit(n))) return false;

  behind[k] = bit(n);
  for (int j = k; j >= 0; --j) {
    for (int i = n - 1; i >= 0; --i) {
      if (line[i] != Filled && (behind[j] & bit(i + 1))) behind[j] |= bit(i);
      if (j < k && fits(line, n, i, runs[j]) && (behind[j + 1] & bit(after(i + runs[j])))) behind[j] |= bit(i);
    }
  }

  // A cell is filled in some placement, empty in some, or both.
  uint32_t canFill = 0;
  uint32_t canEmpty = 0;
  for (int j = 0; j <= k; ++j) {
    for (int i = 0; i < n; ++i) {
      if (!(ahead[j] & bit(i))) continue;
      if (line[i] != Filled && (behind[j] & bit(i + 1))) canEmpty |= bit(i);
      if (j == k || !fits(line, n, i, runs[j])) continue;
      const int end = i + runs[j];
      if (!(behind[j + 1] & bit(after(end)))) continue;
      canFill |= (bit(end) - 1) & ~(bit(i) - 1);
      if (end < n) canEmpty |= bit(end);
    }
  }
  for (int i = 0; i < n; ++i) {
    const bool fill = canFill & bit(i);
    const bool empty = canEmpty & bit(i);
    if (!fill && !empty) return false;
    if (line[i] == Unknown && fill != empty) line[i] = fill ? Filled : Empty;
  }
  return true;
}

int solve(const Clues& clues, const int size, uint8_t* state, int* sweeps) {
  // Sweeps alternate between the rows and the columns, as a person solves;
  // a sweep looks again at the lines a crossing line changed.
  const uint32_t all = bit(size) - 1;
  uint32_t dirty[2] = {all, all};  // rows, columns
  uint8_t line[MAX];
  for (int side = 0; dirty[0] | dirty[1]; side ^= 1) {
    if (!dirty[side]) continue;
    if (sweeps) ++*sweeps;
    const uint32_t now = dirty[side];
    dirty[side] = 0;
    for (int at = 0; at < size; ++at) {
      if (!(now & bit(at))) continue;
      const int id = side * size + at;
      for (int k = 0; k < size; ++k) line[k] = state[cellOf(id, k, size)];
      if (!solveLine(line, size, clues.runs[id], clues.count[id])) return -1;
      for (int k = 0; k < size; ++k) {
        const int cell = cellOf(id, k, size);
        if (state[cell] == line[k]) continue;
        state[cell] = line[k];
        dirty[side ^ 1] |= bit(k);  // the crossing line learned something
      }
    }
  }
  int unknown = 0;
  for (int i = 0; i < size * size; ++i) unknown += state[i] == Unknown ? 1 : 0;
  return unknown;
}

void generate(const uint32_t seed, const uint8_t level, Puzzle& out, Work& work, void (*between)()) {
  const int n = sizeFor(level);
  const int cells = n * n;
  const int wanted = CANDIDATES[level < 1 ? 0 : level > 10 ? 9 : level - 1];
  out = Puzzle{};
  out.size = static_cast<uint8_t>(n);
  int found = 0;
  for (int attempt = 0; attempt < ATTEMPTS && found < wanted; ++attempt) {
    if (between && attempt > 0) between();
    Rng rng(seed ^ mix32(0x9E3779B9u * static_cast<uint32_t>(attempt + 1)));
    drawPicture(rng, n, level, work.picture, work.field);
    uint8_t touches = 0;
    const int sweeps = makeSolvable(work.picture, n, rng, work, touches);
    if (sweeps < 0) continue;
    ++found;
    if (sweeps <= out.sweeps) continue;
    std::memcpy(out.solution, work.picture, static_cast<size_t>(cells));
    out.sweeps = static_cast<uint8_t>(sweeps);
    out.attempts = static_cast<uint8_t>(attempt + 1);
    out.touches = touches;
  }
  if (found) return;
  // Not reached in the tests over thousands of seeds; a full grid always solves.
  std::memset(out.solution, 1, static_cast<size_t>(cells));
  out.sweeps = 1;
  out.attempts = ATTEMPTS;
}

}  // namespace x4::play::nonogram
