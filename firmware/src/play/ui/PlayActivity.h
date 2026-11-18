#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "play/Play.h"
#include "util/ButtonNavigator.h"

// One row per game with its level as a bare number; a check marks the games
// whose daily puzzle was played today. Word games show their language,
// EN or HU; holding Confirm switches it. Confirm plays. A word game without
// its files on the SD card has no row. Last, while one waits, the book puzzles.
inline constexpr char PUZZLES_ROW[] = "puzzles";
class PlayActivity final : public Activity {
 public:
  // `selectedId` picks the row selected on entry (null = the first).
  PlayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* selectedId = nullptr)
      : Activity("Play", renderer, mappedInput), selectedId(selectedId) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Row {
    uint8_t game;  // index in games()
    bool puzzles;  // the book puzzles' row
    uint8_t level;
    bool dailyDone;
    bool words;  // has a language
    x4::play::Lang lang;
  };

  const char* selectedId;
  Row rows[x4::play::GAMES_MAX + 1] = {};
  uint8_t count = 0;
  uint8_t selected = 0;
  bool firstRender = true;
  uint8_t fastRefreshes = 0;
  ButtonNavigator navigator;

  int drawRows(bool scanOnly);
  bool switchLang();
};
