#pragma once

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "learn/Paper.h"
#include "learn/ui/PaperDraw.h"
#include "net/Agenda.h"

// The x4r home: the paper's front page (SPEC: The newspaper). A masthead with the date, the
// weather and the next event (Confirm on it opens the forecast), then a row per section with
// the headline of its first unread piece, then Play, Book and Library. Named "Home" so
// ActivityManager treats it as the home screen.
class TodayActivity final : public Activity {
 public:
  // The front page opens on the row it was left from (paper::place()).
  TodayActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, HomeMenuItem /*initialMenuItem*/,
                bool cleanInitialRefresh)
      : Activity("Home", renderer, mappedInput), cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }

 private:
  const bool cleanInitialRefresh;
  bool firstRender = true;
  uint8_t fastRefreshes = 0;  // since the last clean refresh

  // Copied in onEnter and loop; render() reads only these.
  std::unique_ptr<x4::paper::Front> front;
  x4::paper::Faces faces{};
  bool onMasthead = false;  // the selection is on the date line, one Up above the first row
  std::string bookPath;
  std::string bookTitle;
  char bookPercent[8] = {};
  char playLine[32] = {};  // the first daily puzzle not yet played
  char date[24] = {};      // "Wed 11 Nov"; empty while the clock is unknown
  char agendaLine[96] = {};  // today's weather and the next event
  int32_t day = 0;         // the local day the masthead was written for
  unsigned long clockCheckedAt = 0;
  // What the X4 fetched online (net::), while the front page shows.
  std::unique_ptr<x4::net::Agenda> agenda;
  x4::net::TzRule zone;
  uint32_t agendaGeneration = 0;
  unsigned long lastInputAt = 0;

  void loadIssue();
  void loadBook();
  void loadPlay();
  void loadAgenda();
  bool writeMasthead();
  bool weatherShown() const;
  void open(x4::paper::Go go);
};
