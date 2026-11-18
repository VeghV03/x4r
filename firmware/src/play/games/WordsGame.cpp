// Five-letter word: six guesses at a word from answers5.txt. Arrows move over
// the keyboard, Confirm types the letter under the cursor; holding Confirm
// or the check key enters the guess. A finished board stays, with the answer
// under it when missed, until Confirm.
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <new>

#include "Keyboard.h"
#include "Words.h"
#include "play/Game.h"
#include "play/Play.h"
#include "play/ui/PlayUi.h"

namespace x4::play {

using words::Box;
using words::GUESSES;
using words::Keyboard;
using words::Letter;
using words::Mark;
using words::Tile;
using words::WORD_LEN;

bool wordsReady(const Lang lang) {
  char answers[words::PATH_SIZE];
  char valid[words::PATH_SIZE];
  words::listPath(lang, "answers5.txt", answers);
  words::listPath(lang, "valid5.txt", valid);
  return Storage.exists(answers) && Storage.exists(valid);
}

namespace {

constexpr uint8_t STATE_VERSION = 1;
constexpr int TILE_GAP = 8;

struct State {
  uint8_t version;
  uint8_t lang;
  uint8_t rows;     // guesses entered
  uint8_t typed;    // letters of the guess being typed
  uint8_t key;      // keyboard cursor
  uint8_t invalid;  // the typed guess is not a word
  uint8_t done;
  uint8_t failed;  // no answer could be read
  Letter answer[WORD_LEN];
  Letter guesses[GUESSES][WORD_LEN];
  Letter entry[WORD_LEN];
};

struct Layout {
  Box board;  // the first tile
  int pitch;  // tile to tile
  Box keyboard;
};

class WordsGame final : public Game {
 public:
  void start(const uint32_t seed, const uint8_t level) override {
    s = State{};
    s.version = STATE_VERSION;
    s.lang = static_cast<uint8_t>(language);
    keyboard.setLang(language);
    char path[words::PATH_SIZE];
    words::listPath(language, "answers5.txt", path);
    char line[words::LINE_SIZE];
    if (!words::pickLine(path, seed, level, line, sizeof(line), &isWord, &language) ||
        words::decode(language, line, std::strlen(line), s.answer, WORD_LEN) != WORD_LEN) {
      LOG_ERR("PLAY", "No answer in %s", path);
      s.failed = 1;
    }
  }

  bool holdsConfirm() const override { return true; }

  size_t save(uint8_t* out) const override {
    State copy = s;
    copy.key = keyboard.cursor();
    std::memcpy(out, &copy, sizeof(copy));
    return sizeof(copy);
  }

  bool restore(const uint8_t* in, const size_t size) override {
    if (size != sizeof(State)) return false;
    std::memcpy(&s, in, sizeof(s));
    if (s.version != STATE_VERSION || s.lang >= LANGS || s.rows > GUESSES || s.typed > WORD_LEN) return false;
    language = static_cast<Lang>(s.lang);
    keyboard.setLang(language);
    keyboard.setCursor(s.key);
    return true;
  }

  bool input(const Keys& keys, uint32_t) override {
    if (s.done) return false;
    if (keyboard.move(keys, true)) return true;
    if (keys.confirmHold) return submit();
    if (!keys.confirm) return false;
    switch (keyboard.key()) {
      case Keyboard::ENTER:
        return submit();
      case Keyboard::DELETE:
        if (s.typed == 0) return false;
        --s.typed;
        s.invalid = 0;
        return true;
      default:
        if (s.invalid) {
          s.typed = 0;
          s.invalid = 0;
        }
        if (s.typed == WORD_LEN) return false;
        s.entry[s.typed++] = keyboard.letter();
        return true;
    }
  }

  void draw(const GfxRenderer& r, const x4::ui::Frame& f) const override {
    const Layout l = layout(r, f);
    for (uint8_t row = 0; row < GUESSES; ++row) {
      Mark marks[WORD_LEN] = {};
      if (row < s.rows) words::score(s.answer, s.guesses[row], marks);
      for (uint8_t k = 0; k < WORD_LEN; ++k) {
        const Box b{l.board.x + k * l.pitch, l.board.y + row * l.pitch, l.board.width, l.board.height};
        if (row < s.rows) {
          words::drawTile(r, b, s.guesses[row][k], tileOf(marks[k]));
        } else if (row == s.rows && !s.done && k < s.typed) {
          words::drawTile(r, b, s.entry[k], Tile::Typed);
        } else {
          words::drawTile(r, b, 0, Tile::Empty);
        }
      }
      if (row == s.rows && s.invalid) words::strikeThrough(r, l.board, WORD_LEN, l.pitch);
    }
    if (!s.done) {
      Mark marks[words::ALPHABET_MAX] = {};
      keyMarks(marks);
      keyboard.draw(r, l.keyboard, marks, ui::FOCUS_PEN, words::EnterIcon::Check);
      return;
    }
    if (won()) return;
    // Missed: the answer, centred where the keyboard was.
    const int y = l.keyboard.y + (l.keyboard.height - l.board.height) / 2;
    for (uint8_t k = 0; k < WORD_LEN; ++k) {
      words::drawTile(r, {l.board.x + k * l.pitch, y, l.board.width, l.board.height}, s.answer[k], Tile::Correct);
    }
  }

  bool playable() const override { return !s.failed; }

  Outcome outcome() const override {
    if (!s.done) return Outcome::Playing;
    return won() ? Outcome::Won : Outcome::Lost;
  }

  int16_t tries() const override { return s.rows; }
  const char* lang() const override { return langCode(language); }

 private:
  State s{};
  Keyboard keyboard;

  static bool isWord(const char* line, void* ctx) {
    Letter out[WORD_LEN];
    return words::decode(*static_cast<const Lang*>(ctx), line, std::strlen(line), out, WORD_LEN) == WORD_LEN;
  }

  static Tile tileOf(const Mark mark) {
    return mark == Mark::Correct ? Tile::Correct : mark == Mark::Present ? Tile::Present : Tile::Absent;
  }

  bool won() const { return s.rows > 0 && std::equal(s.answer, s.answer + WORD_LEN, s.guesses[s.rows - 1]); }

  bool submit() {
    if (s.typed < WORD_LEN || s.invalid) return false;
    char path[words::PATH_SIZE];
    words::listPath(language, "valid5.txt", path);
    if (!words::contains(path, s.entry, WORD_LEN)) {
      s.invalid = 1;
      return true;
    }
    std::copy(s.entry, s.entry + WORD_LEN, s.guesses[s.rows++]);
    s.typed = 0;
    if (won() || s.rows == GUESSES) s.done = 1;
    return true;
  }

  // The best mark each letter got in the guesses so far.
  void keyMarks(Mark* out) const {
    for (uint8_t row = 0; row < s.rows; ++row) {
      Mark marks[WORD_LEN];
      words::score(s.answer, s.guesses[row], marks);
      for (uint8_t k = 0; k < WORD_LEN; ++k) {
        const int i = keyboard.indexOf(s.guesses[row][k]);
        if (i >= 0 && marks[k] > out[i]) out[i] = marks[k];
      }
    }
  }

  // Portrait: the board above the keyboard. Landscape: side by side.
  Layout layout(const GfxRenderer& r, const x4::ui::Frame& f) const {
    const int width = r.getScreenWidth();
    const int height = f.bottom - f.top;
    Layout l{};
    const bool wide = width > r.getScreenHeight();
    const int kbWidth = wide ? width / 2 - 24 : std::min(width - 24, 560);
    const int kbHeight = keyboard.height(kbWidth);
    const int boardHeight = wide ? height : height - kbHeight - 24;
    const int boardWidth = wide ? width / 2 - 48 : f.width;
    const int tile = std::min((boardHeight + TILE_GAP) / GUESSES, (boardWidth + TILE_GAP) / WORD_LEN) - TILE_GAP;
    l.pitch = tile + TILE_GAP;
    const int gridWidth = WORD_LEN * l.pitch - TILE_GAP;
    const int gridHeight = GUESSES * l.pitch - TILE_GAP;
    if (wide) {
      l.board = {(width / 2 - gridWidth) / 2, f.top + (height - gridHeight) / 2, tile, tile};
      l.keyboard = {width / 2 + 12, f.top + (height - kbHeight) / 2, kbWidth, kbHeight};
    } else {
      const int top = f.top + (height - gridHeight - 24 - kbHeight) / 2;
      l.board = {(width - gridWidth) / 2, top, tile, tile};
      l.keyboard = {(width - kbWidth) / 2, top + gridHeight + 24, kbWidth, kbHeight};
    }
    return l;
  }
};

}  // namespace

std::unique_ptr<Game> makeWords() { return std::unique_ptr<Game>(new (std::nothrow) WordsGame()); }

}  // namespace x4::play
