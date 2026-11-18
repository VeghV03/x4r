#pragma once

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "learn/Paper.h"
#include "learn/ui/PaperDraw.h"

// The newspaper's section screen (SPEC: The newspaper). The front page is TodayActivity.
namespace x4::paper {

// Where the paper stands, kept while the X4 is awake: the front page opens on the row it was
// left from, and a piece closed returns to its section.
Place& place();
// The faces the paper is set in: the reader's family at 12 and 14, else the built-in sans.
Faces faces();

}  // namespace x4::paper

class SectionActivity final : public Activity {
 public:
  SectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Section", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // A piece's article is about to open: the reader's Back then returns to what follows it.
  static void opening(const char* id, const char* bookKey, bool fromKept);
  // True once for the article opened so: `id` and `fromKept` get the piece it belongs to.
  static bool returnsTo(const std::string& bookPath, std::string& id, bool& fromKept);

 private:
  std::unique_ptr<x4::LineBuffer> buffer;
  std::unique_ptr<x4::paper::Front> front;
  x4::paper::Piece pieces[x4::paper::PIECES_MAX];
  uint8_t count = 0;
  x4::paper::Faces faces{};
  bool firstRender = true;
  uint8_t fastRefreshes = 0;
  bool continues = false;  // left for Football's tables, the same visit
  // Football's tab bar, when the section is Football (D44).
  char competitions[x4::paper::COMPETITIONS_MAX][x4::paper::COMPETITION_BYTES + 1] = {};
  const char* tabs[x4::paper::FOOTBALL_TABS_MAX] = {};
  uint8_t tabCount = 0;

  void load();
  void act(x4::paper::Go go);
};
