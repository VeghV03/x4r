#pragma once

#include <memory>

#include "activities/Activity.h"
#include "learn/LearnTypes.h"
#include "play/Autosave.h"
#include "play/Game.h"
#include "play/Play.h"
#include "util/ButtonNavigator.h"

// Plays one puzzle: the saved one if a game was left midway, else today's
// daily if it was not played yet, else a practice puzzle. Progress is saved
// in pauses while playing (Autosave) and on leaving, sleep included. Back
// leaves; holding Back goes to Today, as everywhere. A result screen ends the game.
class GameActivity final : public Activity {
 public:
  // From the Play screen.
  GameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const x4::play::GameInfo& game)
      : Activity("Game", renderer, mappedInput), info(game) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const x4::play::GameInfo& info;

  std::unique_ptr<x4::play::Game> game;
  x4::play::Saved saved;
  uint32_t resumedAt = 0;  // millis() when this visit started counting play time
  bool finished = false;
  bool won = false;
  bool resultGuard = false;  // a button held when the game ended; cleared on its release
  int pagesUntilFullRefresh = 1;
  // The game before the Confirm press now down, for a hold to start from (holdsConfirm games).
  std::unique_ptr<uint8_t[]> beforePress;
  size_t beforePressSize = 0;
  x4::play::Autosave autosave;
  ButtonNavigator navigator;

  uint32_t playMs() const;
  void saveProgress();
  void end();
  void leave();
  void drawMark(const x4::ui::Frame& f) const;
};
