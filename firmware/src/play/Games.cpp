#include <algorithm>
#include <cstring>

#include "Game.h"

namespace x4::play {

std::unique_ptr<Game> makeSudoku();
std::unique_ptr<Game> makeNonogram();
std::unique_ptr<Game> makeEinstein();
std::unique_ptr<Game> makeArith();
std::unique_ptr<Game> makeWords();
std::unique_ptr<Game> makeCrossword();
std::unique_ptr<Game> makeChess();
bool wordsReady(Lang lang);
bool crosswordReady(Lang lang);
bool chessReady(Lang lang);

namespace {

// The registry: one line per game.
const GameInfo GAMES[] = {
    {"sudoku", StrId::STR_GAME_SUDOKU, &makeSudoku, nullptr, false},
    {"nonogram", StrId::STR_GAME_NONOGRAM, &makeNonogram, nullptr, false},
    {"einstein", StrId::STR_GAME_EINSTEIN, &makeEinstein, nullptr, false},
    {"words", StrId::STR_GAME_WORDS, &makeWords, &wordsReady, true},
    {"crossword", StrId::STR_GAME_CROSSWORD, &makeCrossword, &crosswordReady, true},
    {"chess", StrId::STR_GAME_CHESS, &makeChess, &chessReady, false},
    {"arith", StrId::STR_GAME_ARITH, &makeArith, nullptr, false},
};

}  // namespace

const GameInfo* games() { return GAMES; }

uint8_t gameCount() { return sizeof(GAMES) / sizeof(GAMES[0]); }

const GameInfo* findGame(const char* id) {
  for (const GameInfo& game : GAMES) {
    if (std::strcmp(game.id, id) == 0) return &game;
  }
  return nullptr;
}

namespace {

bool activeLangFrom(const GameInfo& game, const Lang chosen, Lang& out) {
  out = DEFAULT_LANG;
  if (!game.ready) return true;
  for (uint8_t k = 0; k < LANGS; ++k) {
    const auto lang = static_cast<Lang>((static_cast<uint8_t>(chosen) + k) % LANGS);
    if (game.ready(lang)) {
      out = lang;
      return true;
    }
  }
  return false;
}

}  // namespace

bool activeLang(const GameInfo& game, Lang& out) {
  return activeLangFrom(game, game.ready ? standing(game.id).lang : DEFAULT_LANG, out);
}

bool playable(const GameInfo& game) {
  Lang lang;
  return activeLang(game, lang);
}

uint8_t dailiesLeft(const uint32_t day) {
  const uint8_t count = std::min<uint8_t>(gameCount(), GAMES_MAX);
  const char* ids[GAMES_MAX];
  for (uint8_t i = 0; i < count; ++i) ids[i] = games()[i].id;
  Standing all[GAMES_MAX];
  standings(ids, count, all);
  uint8_t left = 0;
  for (uint8_t i = 0; i < count; ++i) {
    Lang lang;
    const bool done = day != 0 && all[i].dailyDay == day;
    if (!done && activeLangFrom(games()[i], all[i].lang, lang)) ++left;
  }
  return left;
}

}  // namespace x4::play
