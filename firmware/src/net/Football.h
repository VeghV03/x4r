#pragma once

#include <cstddef>
#include <cstdint>

#include "Ical.h"

// The followed clubs' results, fixtures and tables, as the Mac writes them to
// /x4/football.json (docs/DATA.md). No hardware here.
namespace x4::net {

inline constexpr char FOOTBALL_PATH[] = "/x4/football.json";

struct Match {
  int64_t at = 0;  // kick-off, UTC
  char comp[13] = {};
  char home[25] = {};
  char away[25] = {};
  int8_t homeGoals = -1;  // -1 before the match
  int8_t awayGoals = -1;
  bool live = false;

  bool hasScore() const { return homeGoals >= 0 && awayGoals >= 0; }
  bool over() const { return hasScore() && !live; }
};

struct TableRow {
  uint8_t pos = 0;
  char team[25] = {};
  uint16_t points = 0;
  bool mine = false;
};

struct Table {
  static constexpr size_t kRows = 36;
  char comp[13] = {};
  TableRow rows[kRows];
  uint8_t count = 0;
};

struct Football {
  static constexpr size_t kMatches = 40;
  static constexpr size_t kTables = 4;
  int64_t fetched = 0;
  Match matches[kMatches];
  uint8_t matchCount = 0;
  Table tables[kTables];
  uint8_t tableCount = 0;
};

// With `tables` false only the matches are read, which is all Today needs.
bool parseFootball(const char* json, size_t length, Football& out, bool tables = true);

// The match that matters now: one under way, else today's (the next, else the last result),
// else tomorrow's, else the last result of the past two days, else the next fixture.
const Match* headline(const Football& football, int64_t now, const TzRule& zone);

// "Brighton 2–1 Brentford", or "Brighton – Brentford" before the match.
void formatMatch(const Match& match, char* out, size_t size);

}  // namespace x4::net
