#include "Einstein.h"

#include <cstring>

#include "play/Rng.h"

namespace x4::play::einstein {

namespace {

constexpr int ATTEMPTS = 24;
constexpr int NAMES_PER_SET = 5;

inline int bits(uint32_t v) {
  int n = 0;
  for (; v; v &= v - 1) ++n;
  return n;
}

inline int lowest(const uint32_t v) {
  int b = 0;
  while (!(v & (1u << b))) ++b;
  return b;
}

inline int highest(const uint32_t v) {
  int b = 31;
  while (!(v & (1u << b))) --b;
  return b;
}

inline uint8_t bit(const int i) { return static_cast<uint8_t>(1u << i); }

// One solve on a grid: removes pairs and records whether anything changed or
// a line ran out of candidates.
struct Solver {
  Grid& g;
  int cats;
  int items;
  uint8_t full;
  bool changed = false;
  bool broken = false;

  Solver(Grid& grid, const int c, const int n) : g(grid), cats(c), items(n), full(static_cast<uint8_t>(bit(n) - 1)) {}

  void reset() {
    std::memset(&g, 0, sizeof(g));
    for (int a = 0; a < cats; ++a) {
      for (int b = 0; b < cats; ++b) {
        if (a == b) continue;
        for (int i = 0; i < items; ++i) g.poss[a][b][i] = full;
      }
    }
  }

  void cut(const int a, const int i, const int b, const int j) {
    if (!(g.poss[a][b][i] & bit(j))) return;
    g.poss[a][b][i] &= static_cast<uint8_t>(~bit(j));
    g.poss[b][a][j] &= static_cast<uint8_t>(~bit(i));
    changed = true;
    if (!g.poss[a][b][i] || !g.poss[b][a][j]) broken = true;
  }

  // Item i of category a is with one of `mask` in category b.
  void keep(const int a, const int i, const int b, const uint8_t mask) {
    for (int j = 0; j < items; ++j) {
      if (!(mask & bit(j))) cut(a, i, b, j);
    }
  }

  void yes(const int a, const int i, const int b, const int j) {
    keep(a, i, b, bit(j));
    for (int k = 0; k < items; ++k) {
      if (k != i) cut(a, k, b, j);
    }
  }

  // The positions item x may be at.
  uint8_t domain(const Ref x) const {
    return catOf(x) == POSITION ? bit(itemOf(x)) : g.poss[catOf(x)][POSITION][itemOf(x)];
  }

  void restrict(const Ref x, const uint8_t mask) {
    if (catOf(x) == POSITION) {
      if (!(mask & bit(itemOf(x)))) broken = true;
      return;
    }
    keep(catOf(x), itemOf(x), POSITION, mask);
  }

  // x and y are at different positions.
  void apart(const Ref x, const Ref y) {
    if (catOf(x) != catOf(y)) cut(catOf(x), itemOf(x), catOf(y), itemOf(y));
  }

  void apply(const Clue& clue) {
    if (clue.kind == Same) yes(catOf(clue.a), itemOf(clue.a), catOf(clue.b), itemOf(clue.b));
    if (clue.kind == Not) apart(clue.a, clue.b);
  }

  bool solved() const {
    for (int c = 1; c < cats; ++c) {
      for (int i = 0; i < items; ++i) {
        if (bits(g.poss[c][POSITION][i]) != 1) return false;
      }
    }
    return true;
  }

  // One yes per line, and a yes shares all its pair knows, until nothing changes.
  void direct() {
    do {
      changed = false;
      for (int a = 0; a < cats; ++a) {
        for (int b = 0; b < cats; ++b) {
          if (a == b) continue;
          for (int i = 0; i < items; ++i) {
            const uint8_t m = g.poss[a][b][i];
            if (bits(m) != 1) continue;
            const int j = lowest(m);
            for (int k = 0; k < items; ++k) {
              if (k != i) cut(a, k, b, j);
            }
            for (int c = 0; c < cats; ++c) {
              if (c == a || c == b) continue;
              const uint8_t shared = g.poss[a][c][i] & g.poss[b][c][j];
              keep(a, i, c, shared);
              keep(b, j, c, shared);
            }
          }
        }
      }
      if (broken) return;
    } while (changed);
  }

  // One pass of the position clues and either-or over the positions still open.
  bool relation(const Clue* clues, const int count) {
    changed = false;
    for (int k = 0; k < count && !broken; ++k) {
      const Clue& c = clues[k];
      switch (c.kind) {
        case NextTo: {
          restrict(c.a, static_cast<uint8_t>((domain(c.b) << 1 | domain(c.b) >> 1) & full));
          restrict(c.b, static_cast<uint8_t>((domain(c.a) << 1 | domain(c.a) >> 1) & full));
          apart(c.a, c.b);
          break;
        }
        case LeftOf: {
          restrict(c.a, static_cast<uint8_t>(domain(c.b) >> 1));
          restrict(c.b, static_cast<uint8_t>((domain(c.a) << 1) & full));
          apart(c.a, c.b);
          break;
        }
        case Before: {
          const uint8_t db = domain(c.b);
          if (db) restrict(c.a, static_cast<uint8_t>(bit(highest(db)) - 1));
          const uint8_t da = domain(c.a);
          if (da) restrict(c.b, static_cast<uint8_t>(full & ~(bit(lowest(da) + 1) - 1)));
          apart(c.a, c.b);
          break;
        }
        case Either: {
          keep(catOf(c.a), itemOf(c.a), catOf(c.b), static_cast<uint8_t>(bit(itemOf(c.b)) | bit(itemOf(c.c))));
          restrict(c.a, static_cast<uint8_t>(domain(c.b) | domain(c.c)));
          if (!(domain(c.a) & domain(c.b))) apart(c.a, c.b);
          if (!(domain(c.a) & domain(c.c))) apart(c.a, c.c);
          break;
        }
        default:
          break;
      }
    }
    return changed;
  }

  // One pass: a pair is out when no item of some third category fits both.
  bool chain() {
    changed = false;
    for (int a = 0; a < cats; ++a) {
      for (int b = a + 1; b < cats; ++b) {
        for (int i = 0; i < items; ++i) {
          for (int j = 0; j < items; ++j) {
            if (!(g.poss[a][b][i] & bit(j))) continue;
            for (int c = 0; c < cats; ++c) {
              if (c == a || c == b) continue;
              if (!(g.poss[a][c][i] & g.poss[b][c][j])) {
                cut(a, i, b, j);
                break;
              }
            }
          }
        }
      }
    }
    return changed;
  }

  // Runs the deductions, easiest first, from the grid as it is.
  int8_t run(const Clue* clues, const int count, const int8_t maxTier, uint8_t* steps) {
    int8_t tier = Direct;
    uint8_t escalations = 0;
    for (;;) {
      direct();
      if (broken) return Unsolved;
      if (solved()) break;
      if (maxTier >= Relation && relation(clues, count)) {
        if (tier < Relation) tier = Relation;
      } else if (maxTier >= Chain && chain()) {
        tier = Chain;
      } else {
        return Unsolved;
      }
      if (broken) return Unsolved;
      if (escalations < 255) ++escalations;
    }
    if (steps) *steps = escalations;
    return tier;
  }
};

// ---- generation -----------------------------------------------------------------

LevelSpec makeSpec(const uint8_t cats, const uint8_t items, const int8_t minTier, const int8_t maxTier,
                   const uint8_t minSteps, const uint8_t same, const uint8_t no, const uint8_t next, const uint8_t left,
                   const uint8_t before, const uint8_t either) {
  return LevelSpec{cats, items, minTier, maxTier, minSteps, {same, no, next, left, before, either}};
}

bool sameClue(const Clue& x, const Clue& y) { return x.kind == y.kind && x.a == y.a && x.b == y.b && x.c == y.c; }

// A true clue of a kind drawn by the level's weights.
Clue randomClue(Rng& rng, const LevelSpec& spec, const uint8_t (&who)[CATS_MAX][ITEMS_MAX]) {
  const int n = spec.items;
  const int k = spec.cats;
  int total = 0;
  for (const uint8_t w : spec.weight) total += w;
  int r = static_cast<int>(rng.below(static_cast<uint32_t>(total)));
  uint8_t kind = 0;
  while (r >= spec.weight[kind]) r -= spec.weight[kind++];

  // Category a is never the position; b may be for Same, Not and Either.
  const int ca = 1 + static_cast<int>(rng.below(static_cast<uint32_t>(k - 1)));
  const bool positional = kind == NextTo || kind == LeftOf || kind == Before;
  int cb;
  if (positional) {
    cb = 1 + static_cast<int>(rng.below(static_cast<uint32_t>(k - 1)));
  } else {
    cb = static_cast<int>(rng.below(static_cast<uint32_t>(k - 1)));
    if (cb >= ca) ++cb;
  }
  int p = static_cast<int>(rng.below(static_cast<uint32_t>(n)));
  int q = static_cast<int>(rng.below(static_cast<uint32_t>(n - 1)));
  if (q >= p) ++q;  // q != p
  switch (kind) {
    case Same:
      q = p;
      break;
    case NextTo:
      q = p + 1 < n && (p == 0 || rng.below(2)) ? p + 1 : p - 1;
      break;
    case LeftOf:
    case Before:
      if (p > q) {
        const int t = p;
        p = q;
        q = t;
      }
      if (kind == LeftOf) q = p + 1;
      break;
    default:
      break;
  }

  Clue clue{kind, ref(ca, who[ca][p]), ref(cb, who[cb][q]), 0};
  if (kind == Same) clue.b = ref(cb, who[cb][p]);
  if (kind == Either) {
    clue.b = ref(cb, who[cb][p]);
    clue.c = ref(cb, who[cb][q]);
    if (clue.c < clue.b) {
      const Ref t = clue.b;
      clue.b = clue.c;
      clue.c = t;
    }
  }
  // Symmetric kinds in one order, so duplicates compare equal: the higher
  // category first, which leaves a position second ("Red is at 3").
  if ((kind == Same || kind == Not || kind == NextTo) && clue.a < clue.b) {
    const Ref t = clue.a;
    clue.a = clue.b;
    clue.b = t;
  }
  return clue;
}

}  // namespace

LevelSpec levelSpec(const uint8_t level) {
  //                     cats items min      max    steps same not next left before either
  switch (level <= 1 ? 1 : level >= 10 ? 10 : level) {
    case 1:
      return makeSpec(3, 3, Direct, Direct, 0, 4, 3, 0, 0, 0, 0);
    case 2:
      return makeSpec(3, 4, Direct, Relation, 0, 4, 3, 2, 2, 0, 0);
    case 3:
      return makeSpec(4, 4, Relation, Chain, 2, 3, 3, 2, 2, 1, 1);
    case 4:
      return makeSpec(4, 4, Chain, Chain, 4, 3, 3, 2, 2, 2, 1);
    case 5:
      return makeSpec(4, 5, Chain, Chain, 5, 2, 3, 2, 2, 2, 2);
    case 6:
      return makeSpec(4, 5, Chain, Chain, 6, 2, 3, 2, 2, 2, 2);
    case 7:
      return makeSpec(5, 4, Chain, Chain, 6, 2, 3, 2, 2, 2, 2);
    case 8:
      return makeSpec(5, 5, Chain, Chain, 7, 2, 3, 2, 2, 2, 2);
    case 9:
      return makeSpec(5, 5, Chain, Chain, 8, 1, 3, 2, 2, 2, 2);
    default:
      return makeSpec(5, 5, Chain, Chain, 9, 1, 2, 2, 2, 2, 3);
  }
}

bool holds(const Clue& clue, const uint8_t (&at)[CATS_MAX][ITEMS_MAX]) {
  const int pa = at[catOf(clue.a)][itemOf(clue.a)];
  const int pb = at[catOf(clue.b)][itemOf(clue.b)];
  switch (clue.kind) {
    case Same:
      return pa == pb;
    case Not:
      return pa != pb;
    case NextTo:
      return pa - pb == 1 || pb - pa == 1;
    case LeftOf:
      return pa + 1 == pb;
    case Before:
      return pa < pb;
    case Either:
      return pa == pb || pa == at[catOf(clue.c)][itemOf(clue.c)];
    default:
      return false;
  }
}

int8_t solve(const uint8_t cats, const uint8_t items, const Clue* clues, const int count, const int8_t maxTier,
             Grid& grid, uint8_t* steps) {
  Solver s(grid, cats, items);
  s.reset();
  for (int k = 0; k < count; ++k) s.apply(clues[k]);
  return s.run(clues, count, maxTier, steps);
}

namespace {

struct Count {
  uint8_t cats;
  uint8_t items;
  const Clue* clues;
  int count;
  int limit;
  int found;
  uint8_t at[CATS_MAX][ITEMS_MAX];
};

int topCat(const Clue& c) {
  int top = catOf(c.a) > catOf(c.b) ? catOf(c.a) : catOf(c.b);
  if (c.kind == Either && catOf(c.c) > top) top = catOf(c.c);
  return top;
}

// Places category `cat` in every order, checking the clues it completes.
void place(Count& w, const int cat, const int item, const uint8_t used) {
  if (w.found >= w.limit) return;
  if (cat == w.cats) {
    ++w.found;
    return;
  }
  if (item == w.items) {
    for (int k = 0; k < w.count; ++k) {
      if (topCat(w.clues[k]) == cat && !holds(w.clues[k], w.at)) return;
    }
    place(w, cat + 1, 0, 0);
    return;
  }
  for (int p = 0; p < w.items; ++p) {
    if (used & bit(p)) continue;
    w.at[cat][item] = static_cast<uint8_t>(p);
    place(w, cat, item + 1, static_cast<uint8_t>(used | bit(p)));
  }
}

}  // namespace

Joins::Joins(const Puzzle& puzzle) : p(puzzle) {
  for (int k = 0; k < CATS_MAX * ITEMS_MAX; ++k) parent[k] = static_cast<uint8_t>(k);
}

int Joins::find(int x) {
  while (parent[x] != x) x = parent[x] = parent[parent[x]];
  return x;
}

void Joins::join(const Ref a, const Ref b) {
  if (p.at[catOf(a)][itemOf(a)] != p.at[catOf(b)][itemOf(b)]) wrong = true;
  parent[find(catOf(a) * ITEMS_MAX + itemOf(a))] = static_cast<uint8_t>(find(catOf(b) * ITEMS_MAX + itemOf(b)));
}

Verdict Joins::verdict() {
  // Whole when the group of each position holds one item of every category.
  for (int i = 0; i < p.items; ++i) {
    const int root = find(POSITION * ITEMS_MAX + i);
    uint8_t cats = 0;
    int members = 0;
    for (int c = 0; c < p.cats; ++c) {
      for (int j = 0; j < p.items; ++j) {
        if (find(c * ITEMS_MAX + j) != root) continue;
        ++members;
        cats |= bit(c);
      }
    }
    if (members != p.cats || cats != bit(p.cats) - 1) return Verdict::Open;
  }
  return wrong ? Verdict::Wrong : Verdict::Right;
}

int countSolutions(const uint8_t cats, const uint8_t items, const Clue* clues, const int count, const int limit) {
  Count w{};
  w.cats = cats;
  w.items = items;
  w.clues = clues;
  w.count = count;
  w.limit = limit;
  for (int i = 0; i < items; ++i) w.at[POSITION][i] = static_cast<uint8_t>(i);
  for (int k = 0; k < count; ++k) {
    if (topCat(clues[k]) == POSITION && !holds(clues[k], w.at)) return 0;
  }
  place(w, 1, 0, 0);
  return w.found;
}

void generate(const uint32_t seed, const uint8_t level, Puzzle& out, Work& work, void (*between)()) {
  const LevelSpec spec = levelSpec(level);
  const int n = spec.items;
  out = Puzzle{};
  out.tier = Unsolved;
  Puzzle& trial = work.trial;
  for (int attempt = 0; attempt < ATTEMPTS; ++attempt) {
    if (between && attempt > 0) between();
    Rng rng(seed ^ mix32(0x9E3779B9u * static_cast<uint32_t>(attempt + 1)));
    trial = Puzzle{};
    trial.cats = spec.cats;
    trial.items = spec.items;

    // Name sets and the names drawn from each, then the solution.
    uint8_t sets[SETS];
    for (int s = 0; s < SETS; ++s) sets[s] = static_cast<uint8_t>(s);
    rng.shuffle(sets, SETS);
    uint8_t who[CATS_MAX][ITEMS_MAX] = {};
    for (int c = 0; c < spec.cats; ++c) {
      uint8_t pick[NAMES_PER_SET];
      for (int i = 0; i < NAMES_PER_SET; ++i) pick[i] = static_cast<uint8_t>(i);
      rng.shuffle(pick, NAMES_PER_SET);
      // The chosen names in set order, so the grid lists them the same way every time.
      int i = 0;
      for (int name = 0; name < NAMES_PER_SET; ++name) {
        for (int k = 0; k < n; ++k) {
          if (pick[k] == name) trial.name[c][i++] = static_cast<uint8_t>(name);
        }
      }
      trial.set[c] = c == POSITION ? 0 : sets[c - 1];
      uint8_t order[ITEMS_MAX];
      for (int p = 0; p < n; ++p) order[p] = static_cast<uint8_t>(p);
      if (c != POSITION) rng.shuffle(order, static_cast<uint32_t>(n));
      for (int item = 0; item < n; ++item) {
        trial.at[c][item] = order[item];
        who[c][order[item]] = static_cast<uint8_t>(item);
      }
    }

    // Adds true clues until the deductions finish the grid. The grid carries
    // over between clues, since a clue only ever removes pairs.
    Solver s(work.grid, spec.cats, spec.items);
    s.reset();
    int count = 0;
    bool solved = false;
    for (int tries = 0; count < POOL_MAX && tries < POOL_MAX * 4 && !solved; ++tries) {
      const Clue clue = randomClue(rng, spec, who);
      bool repeat = false;
      for (int k = 0; k < count && !repeat; ++k) repeat = sameClue(work.pool[k], clue);
      if (repeat) continue;
      work.pool[count++] = clue;
      s.apply(clue);
      solved = s.run(work.pool, count, spec.maxTier, nullptr) != Unsolved;
    }
    if (!solved) continue;

    // Drops every clue the rest can do without: "is" clues first, so the
    // deductions lean on the relations.
    int order = 0;
    for (int pass = 0; pass < 2; ++pass) {
      const int start = order;
      for (int k = 0; k < count; ++k) {
        if ((work.pool[k].kind == Same) == (pass == 0)) work.order[order++] = static_cast<uint8_t>(k);
      }
      rng.shuffle(work.order + start, static_cast<uint32_t>(order - start));
    }
    bool live[POOL_MAX];
    for (int k = 0; k < count; ++k) live[k] = true;
    int left = count;
    for (int o = 0; o < count; ++o) {
      live[work.order[o]] = false;
      int m = 0;
      for (int k = 0; k < count; ++k) {
        if (live[k]) work.live[m++] = work.pool[k];
      }
      if (solve(spec.cats, spec.items, work.live, m, spec.maxTier, work.grid) == Unsolved) {
        live[work.order[o]] = true;
      } else {
        --left;
      }
    }
    if (left > CLUES_MAX) continue;
    for (int k = 0; k < count; ++k) {
      if (live[k]) trial.clues[trial.clueCount++] = work.pool[k];
    }
    trial.tier = solve(spec.cats, spec.items, trial.clues, trial.clueCount, spec.maxTier, work.grid, &trial.steps);

    // Keep the attempt closest to the band (harder wins below it).
    const bool better =
        out.tier == Unsolved || trial.tier > out.tier || (trial.tier == out.tier && trial.steps > out.steps);
    if (better) out = trial;
    if (trial.tier >= spec.minTier && trial.steps >= spec.minSteps) return;
  }
}

}  // namespace x4::play::einstein
