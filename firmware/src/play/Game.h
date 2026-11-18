#pragma once

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "Play.h"
#include "learn/ui/LearnUi.h"

// One game on the Play screen. GameActivity owns the lifecycle (daily or
// practice puzzle, level, saving, the result and play.jsonl); a game only
// plays one puzzle. Adding a game is one file that implements Game and one
// entry in Games.cpp.
namespace x4::play {

// One frame of input, mapped to logical buttons.
struct Keys {
  bool up = false;  // press, repeating while held
  bool down = false;
  bool left = false;
  bool right = false;
  bool confirm = false;      // press edge; a hold that follows acts on the state before it
  bool confirmHold = false;  // held past the hold threshold, once
};

enum class Outcome : uint8_t { Playing, Won, Lost };

class Game {
 public:
  virtual ~Game() = default;

  // A new puzzle from `seed` at `level` (1-10).
  virtual void start(uint32_t seed, uint8_t level) = 0;
  // The puzzle in progress as bytes (at most SAVE_MAX), and back.
  virtual size_t save(uint8_t* out) const = 0;
  virtual bool restore(const uint8_t* in, size_t size) = 0;

  // Each returns true when the screen changed. Called under the render lock.
  virtual bool input(const Keys& keys, uint32_t now) = 0;
  // Back pressed. True when the game took it as a step back inside the
  // puzzle; otherwise the host leaves the game.
  virtual bool back() { return false; }
  virtual bool tick(uint32_t now) {
    (void)now;
    return false;
  }
  // While true, the game runs on its own clock: tick() is called every loop
  // and refreshes stay fast (no periodic full refresh mid-round).
  virtual bool paced() const { return false; }
  // True when holding Confirm does something. Confirm acts on press, so the host then keeps a
  // save() from each press and restores it before passing the hold: save() must hold all state.
  virtual bool holdsConfirm() const { return false; }
  // True when save() now holds progress that restore() keeps. Mid-round,
  // where restore() starts the round over, saving while playing waits.
  virtual bool resumable() const { return !paced(); }

  // False when start() or restore() found nothing to play (a file that cannot be read).
  virtual bool playable() const { return true; }

  // Draws into the frame's content area. A finished game draws its finished board.
  virtual void draw(const GfxRenderer& renderer, const ui::Frame& frame) const = 0;
  // True when the board has true grays: after draw(), drawGray() runs once per gray plane (see
  // ui::displayWithAntiAliasing). There drawPixel(false) marks gray; pixels left alone keep draw()'s.
  virtual bool drawsGray() const { return false; }
  virtual void drawGray(const GfxRenderer& renderer, const ui::Frame& frame) const {
    (void)renderer;
    (void)frame;
  }

  virtual Outcome outcome() const = 0;
  // True when the finished board shows the outcome itself; otherwise the host marks it.
  virtual bool marksOutcome() const { return false; }

  // play.jsonl extras; -1 or null when the game has none.
  virtual int16_t score() const { return -1; }
  virtual int16_t tries() const { return -1; }
  virtual const char* lang() const { return nullptr; }

  // A word game's language, set before start(); restore() brings back the saved one.
  void setLang(const Lang lang) { language = lang; }

 protected:
  Lang language = DEFAULT_LANG;
};

struct GameInfo {
  const char* id;  // play.jsonl `game`, at most GAME_ID_MAX bytes
  StrId name;
  std::unique_ptr<Game> (*make)();
  // Whether the game's files are on the SD card (word games: for a language). Null when it needs none.
  bool (*ready)(Lang);
  bool languages;  // played in English or Hungarian
};

// The games in Play screen order.
const GameInfo* games();
uint8_t gameCount();
const GameInfo* findGame(const char* id);

// The language a game plays in: the chosen one if its files are there, else
// one that has them. False when no language has them; then the game is hidden.
bool activeLang(const GameInfo& game, Lang& out);
bool playable(const GameInfo& game);
// Playable games whose daily puzzle of `day` is not played yet, from one read of the records.
uint8_t dailiesLeft(uint32_t day);

}  // namespace x4::play
