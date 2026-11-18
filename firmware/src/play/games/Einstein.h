#pragma once

#include <cstdint>

// Einstein-style logic grid: generation and a deduction solver. Pure C++, no
// allocation: every buffer lives in the caller's Work (under 1 KB).
//
// A puzzle has `cats` categories of `items` items each. Category 0 is the
// position, 1 to `items` from left to right; every other category places each
// of its items at one position. Clues relate two or three items.
namespace x4::play::einstein {

inline constexpr int CATS_MAX = 5;
inline constexpr int ITEMS_MAX = 5;
inline constexpr int CLUES_MAX = 32;
inline constexpr int POSITION = 0;
// Name sets the other categories draw from (colors, pets, drinks, people).
inline constexpr int SETS = 4;

// An item: its category in the high nibble, its index in the low one.
using Ref = uint8_t;
inline Ref ref(const int cat, const int item) { return static_cast<Ref>(cat << 4 | item); }
inline int catOf(const Ref r) { return r >> 4; }
inline int itemOf(const Ref r) { return r & 0x0F; }

enum Kind : uint8_t {
  Same,    // a is with b (b a position: a is at b)
  Not,     // a is not with b
  NextTo,  // a and b are at neighbouring positions
  LeftOf,  // a is directly left of b
  Before,  // a is somewhere left of b
  Either,  // a is with b or with c (b and c share a category)
  KINDS,
};

struct Clue {
  uint8_t kind;
  Ref a;
  Ref b;
  Ref c;  // Either only
};

// The hardest deduction a puzzle needs, easiest first.
enum Tier : int8_t {
  Direct = 0,     // the clues as marks, one yes per line, a yes shares what its pair knows
  Relation = 1,   // the position clues and either-or, worked on the positions left
  Chain = 2,      // a pair is out when no item of a third category fits both
  Unsolved = -1,  // beyond these deductions
};

struct Puzzle {
  uint8_t cats;
  uint8_t items;
  uint8_t set[CATS_MAX];              // name set of each category; unused for the position
  uint8_t name[CATS_MAX][ITEMS_MAX];  // index into the set, ascending
  uint8_t at[CATS_MAX][ITEMS_MAX];    // the solution: the position of each item
  uint8_t clueCount;
  int8_t tier;
  uint8_t steps;  // times the solve needed more than direct deductions
  Clue clues[CLUES_MAX];
};

// What a level asks of the generator.
struct LevelSpec {
  uint8_t cats;
  uint8_t items;
  int8_t minTier;
  int8_t maxTier;
  uint8_t minSteps;
  uint8_t weight[KINDS];  // how often each kind of clue is drawn
};
LevelSpec levelSpec(uint8_t level);

// What a solve knows: poss[a][b][i] holds the items of category b that item i
// of category a may still be with (bit j = item j). Kept symmetric.
struct Grid {
  uint8_t poss[CATS_MAX][CATS_MAX][ITEMS_MAX];
};

inline constexpr int POOL_MAX = 64;

// Scratch for the generator (kept off the stack).
struct Work {
  Grid grid;
  Clue pool[POOL_MAX];  // clues drawn so far
  Clue live[POOL_MAX];  // the pool less those pruned
  uint8_t order[POOL_MAX];
  Puzzle trial;
};

// Solves `count` clues with deductions up to `maxTier`, easiest first. Returns
// the hardest one needed, or Unsolved. A solved grid has one solution, since
// every deduction only removes impossible pairs. `steps` counts the times
// direct deductions ran dry.
int8_t solve(uint8_t cats, uint8_t items, const Clue* clues, int count, int8_t maxTier, Grid& grid,
             uint8_t* steps = nullptr);

// Counts solutions by brute force up to `limit` (host tests; 5 x 5 is 120^4 at worst, pruned).
int countSolutions(uint8_t cats, uint8_t items, const Clue* clues, int count, int limit);

// Whether the clue holds for the placement `at` (at[cat][item] = position).
bool holds(const Clue& clue, const uint8_t (&at)[CATS_MAX][ITEMS_MAX]);

// The player's yes marks judged together: a yes joins two items, and the
// puzzle is solved when the joins place every item at its position.
enum class Verdict : uint8_t {
  Open,   // not yet a whole solution
  Wrong,  // a whole solution, one item per category at each position, but not this one
  Right,
};

class Joins {
 public:
  explicit Joins(const Puzzle& puzzle);
  void join(Ref a, Ref b);
  Verdict verdict();

 private:
  const Puzzle& p;
  uint8_t parent[CATS_MAX * ITEMS_MAX];  // indexed cat * ITEMS_MAX + item
  bool wrong = false;                    // a yes joins items at different positions

  int find(int x);
};

// A puzzle for `level` (1-10) from `seed`. Deterministic. `between` runs
// between attempts (the device yields there).
void generate(uint32_t seed, uint8_t level, Puzzle& out, Work& work, void (*between)() = nullptr);

}  // namespace x4::play::einstein
