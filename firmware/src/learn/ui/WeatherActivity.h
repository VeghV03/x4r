#pragma once

#include <memory>

#include "activities/Activity.h"
#include "net/Agenda.h"

// The forecast the X4 last fetched: the day ahead in three-hour steps (time, temperature,
// sky, chance of rain), then the week by day. Opened from Today's date and weather lines.
// Nothing to select: Back goes home.
class WeatherActivity final : public Activity {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::unique_ptr<x4::net::Agenda> agenda;
  x4::net::TzRule zone;
  int32_t today = 0;  // local day
  uint8_t hour = 0;

  void draw();
};
