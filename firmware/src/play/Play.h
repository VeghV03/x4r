#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

// The Play core: per-game levels and counts, daily seeds, play.jsonl and the
// saved game in progress (docs/DATA.md, "/x4/play.jsonl"). Pure C++ over
// HalStorage; main task only. The day, the set number and the wall clock come
// from the caller, so this does not depend on the learning core.
namespace x4::play {

inline constexpr uint8_t LEVEL_MIN = 1;
inline constexpr uint8_t LEVEL_MAX = 10;
inline constexpr uint8_t LEVEL_START = 3;
inline constexpr size_t GAME_ID_MAX = 10;  // "crossword" fits
inline constexpr uint8_t GAMES_MAX = 16;
// Largest saved game (Sudoku is about 420 bytes).
inline constexpr size_t SAVE_MAX = 1024;

enum class Mode : uint8_t { Daily, Practice, Set };
const char* modeName(Mode mode);

// The language of a word game, chosen per game.
enum class Lang : uint8_t { En, Hu };
inline constexpr uint8_t LANGS = 2;
// A word game never set to a language plays in this one.
inline constexpr Lang DEFAULT_LANG = Lang::Hu;
// "en" or "hu": play.jsonl `lang` and the folder names under /x4/words/.
const char* langCode(Lang lang);

// One game's standing. `played` and `won` count daily and set games only.
struct Standing {
  uint8_t level = LEVEL_START;
  uint32_t played = 0;
  uint32_t won = 0;
  uint32_t dailyDay = 0;  // day whose daily was last played (dailyDay()); 0 = never
  uint32_t practice = 0;  // practice puzzles started, for their seeds
  Lang lang = DEFAULT_LANG;
};

Standing standing(const char* game);
// The standings of `count` games from one read of the records file, for screens that show many.
void standings(const char* const* games, uint8_t count, Standing* out);
void setLang(const char* game, Lang lang);
// The day the dailies belong to (SPEC: Play): `today`, counted from 1, or with the clock unknown
// (0) the last day a daily was played, so that day's dailies stay until the clock is known.
uint32_t dailyDay(uint32_t today);
bool dailyDone(const char* game, uint32_t day);

// Seeds are stable for a game and day, so a daily puzzle survives reboots.
uint32_t dailySeed(const char* game, uint32_t day);
// The seed of the next practice puzzle; counts it.
uint32_t nextPracticeSeed(const char* game, uint32_t day);

// A level after one game: a win one harder, a loss one easier.
uint8_t adapt(uint8_t level, bool won);

struct Result {
  const char* game = "";
  const char* lang = nullptr;  // word games only
  Mode mode = Mode::Practice;
  bool daily = false;  // the puzzle was this day's daily
  uint8_t level = LEVEL_START;
  bool won = false;
  int16_t score = -1;  // -1 = none
  int16_t tries = -1;
  uint32_t ms = 0;
};

// Adapts the level, counts the game, marks the daily of `day` played and appends the
// play.jsonl line, which carries `day`. `at` 0 writes null. Returns the new level.
uint8_t finish(const Result& result, uint32_t day, int64_t at);

// The stats.json `play` object: {"<game>":{"level":4,"played":31,"won":24}}.
void addStats(JsonObject play);

// A game in progress, saved on leaving and loaded on return.
struct Saved {
  Mode mode = Mode::Practice;
  bool daily = false;
  uint8_t level = LEVEL_START;
  uint32_t day = 0;   // day it was started on (dailyDay())
  uint32_t ms = 0;    // play time so far
  uint16_t size = 0;  // bytes of game state
};
bool saveGame(const char* game, const Saved& saved, const uint8_t* state);
// `state` holds SAVE_MAX bytes. False when nothing valid is saved.
bool loadGame(const char* game, Saved& saved, uint8_t* state);
void clearGame(const char* game);

}  // namespace x4::play
