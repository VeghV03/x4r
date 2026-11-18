#pragma once

#include <memory>

#include "LearnUi.h"
#include "activities/Activity.h"
#include "PaperDraw.h"
#include "net/Football.h"

// Football's pages after its pieces: Matches and one table per competition, under the paper's
// masthead of tabs that the pieces page shares (D44). Left and Right switch tabs, Up and Down
// turn pages, Back goes home. Nothing to select.
class FootballActivity final : public Activity {
 public:
  // `fromRight`: leafed into from the section after Football, so it opens on its last table.
  FootballActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromRight = false)
      : Activity("Football", renderer, mappedInput), fromRight(fromRight) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::unique_ptr<x4::net::Football> football;
  x4::net::TzRule zone;
  int64_t now = 0;
  x4::paper::Faces faces{};
  const bool fromRight;
  uint8_t tab = 0;  // 0 Matches, then the tables; Analysis, the pieces, is the tab before
  int page = 0;
  bool firstRender = true;
  bool continues = false;  // leafed back into the paper: the section screen goes on with the visit
  uint8_t fastRefreshes = 0;

  uint8_t tabCount() const;
  // The page under the masthead, within the paper's margins.
  x4::ui::Frame pageFrame() const;
  bool leafOut(bool right);
  void openTab(uint8_t index);
  // Lays out the tab and draws page `drawPage` (-1: measure only). Returns the page count;
  // `focusPage` gets the page of the match that matters now, or of the first followed club.
  int layout(int drawPage, int* focusPage);
  void draw(bool scanOnly);
};
