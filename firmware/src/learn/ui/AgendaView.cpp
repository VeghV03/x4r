#include "AgendaView.h"

#include <cstdio>
#include <string>

#include "fontIds.h"

namespace x4::ui {

namespace {
constexpr int TIME_GAP = 14;  // between an event's time and its title
constexpr int LINE_GAP = 4;   // between agenda lines
constexpr int HEAD_GAP = 8;   // between the weather line and the events
}  // namespace

int drawAgenda(const GfxRenderer& renderer, const int left, const int width, const int top, const net::Agenda& agenda,
               const net::TzRule& zone, const int32_t day, const int64_t after, const uint8_t maxEvents,
               const char* heading) {
  const int font = UI_12_FONT_ID;
  const int lh = renderer.getLineHeight(font) + LINE_GAP;
  int y = top;

  const net::WeatherDay* weather = agenda.hasWeather ? net::weatherOfDay(agenda, day) : nullptr;
  if (weather || (heading && *heading)) {
    char text[64] = "";
    const char* condition = weather ? net::conditionName(weather->code) : "";
    if (heading && *heading && *condition) {
      snprintf(text, sizeof(text), "%s, %s", heading, condition);
    } else {
      snprintf(text, sizeof(text), "%s", heading && *heading ? heading : condition);
    }
    renderer.drawText(font, left, y, text);
    if (weather) {
      char temps[24];
      net::formatTemperatures(*weather, temps, sizeof(temps));
      renderer.drawText(font, left + width - renderer.getTextWidth(font, temps), y, temps);
    }
    y += lh + HEAD_GAP;
  }

  const net::Event* events[6];
  const uint8_t n = agenda.hasEvents ? net::eventsOfDay(agenda, zone, day, after, events,
                                                        maxEvents < 6 ? maxEvents : 6)
                                     : 0;
  const int timeWidth = renderer.getTextWidth(font, "00:00") + TIME_GAP;
  for (uint8_t i = 0; i < n; ++i) {
    const net::Event& e = *events[i];
    if (!e.allDay) {
      char clock[8];
      net::formatClock(e.start, zone, clock, sizeof(clock));
      renderer.drawText(font, left, y, clock);
    }
    const char* title = e.title[0] ? e.title : "Busy";
    const std::string fitted = renderer.truncatedText(font, title, width - timeWidth);
    renderer.drawText(font, left + timeWidth, y, fitted.c_str());
    y += lh;
  }
  return y;
}

}  // namespace x4::ui
