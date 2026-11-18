#pragma once

#include <cstdint>

// The day (docs/DATA.md "The day") and the state learn.bin keeps. Pure C++, no Arduino
// dependencies.
namespace x4 {

// Persisted in /x4/state/learn.bin. Plain data so it can be stored as-is.
struct LearnState {
  uint32_t day;          // DATA.md "The day": days since 1 January 1970 once dated; 0 before the first boot
  uint8_t unused2;       // was the daily set's open flag
  uint8_t dated;         // 1 once a clock set the day; before that it counts from 1
  uint16_t unused0;
  uint32_t countMs;      // reading towards the next day while the clock is unknown
  // Every card and book moves by shiftDelta days when the day jumps (DATA.md "The day"); a
  // deck or books.bin carries the shiftSeq it got, so a shift cut by power loss is finished
  // and never applied twice. shiftDelta is 0 once every file has it.
  uint32_t shiftSeq;
  int64_t unused1;
  int32_t shiftDelta;
  int32_t tzMinutes;     // last offset the host sent
  uint16_t unused3;      // unused3 and unused4: the daily set's counters
  uint16_t newToday;     // new cards asked today
  uint16_t unused4[2];
  uint32_t totalsDay;    // the day the totals below were counted on
  // Cached for stats.json; refreshed whenever the cards are scanned.
  float retention;       // < 0 while unknown (no reviewed card)
  uint32_t cards;
  uint32_t due;
};

// The stored size: a different one would drop the saved state (Learn.cpp).
static_assert(sizeof(LearnState) == 56, "LearnState is stored as-is");

// Offline, this much time in the reader makes a day (D1).
inline constexpr uint32_t DAY_READ_MS = 20u * 60u * 1000u;

// Calendar day number of `unixSeconds` at UTC offset `tzMinutes`.
int64_t civilDay(int64_t unixSeconds, int32_t tzMinutes);

// A wall-clock moment broken down at a UTC offset (proleptic Gregorian).
struct CivilTime {
  int32_t year;
  uint8_t month;    // 1-12
  uint8_t day;      // 1-31
  uint8_t weekday;  // 0 = Sunday
  uint8_t hour;
  uint8_t minute;
};
CivilTime civilTime(int64_t unixSeconds, int32_t tzMinutes);

// A fresh state starts on day 1. True when it started it.
bool openFirstDay(LearnState& state);

// The day moved: it starts with no new card asked.
void startDay(LearnState& state, uint32_t day);

}  // namespace x4
