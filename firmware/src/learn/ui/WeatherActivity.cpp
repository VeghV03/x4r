#include "WeatherActivity.h"

#include <FontCacheManager.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <cstdio>

#include "LearnUi.h"
#include "fontIds.h"
#include "learn/Learn.h"
#include "net/NetSync.h"

namespace ui = x4::ui;
namespace net = x4::net;

namespace {

constexpr int LINE_GAP = 10;   // between rows
constexpr int BLOCK_GAP = 34;  // between the hours and the days
constexpr int COLUMN_GAP = 18;

constexpr StrId WEEKDAYS[7] = {StrId::STR_SUNDAY,   StrId::STR_MONDAY, StrId::STR_TUESDAY, StrId::STR_WEDNESDAY,
                               StrId::STR_THURSDAY, StrId::STR_FRIDAY, StrId::STR_SATURDAY};

}  // namespace

void WeatherActivity::onEnter() {
  Activity::onEnter();
  agenda = net::loadAgenda();
  zone = net::zone();
  const int64_t now = x4::learn::wallClock();
  today = net::localDay(now, zone);
  hour = static_cast<uint8_t>((zone.toLocal(now) - static_cast<int64_t>(today) * 86400) / 3600);
  requestUpdate();
}

void WeatherActivity::onExit() {
  Activity::onExit();
  agenda.reset();
}

void WeatherActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) activityManager.goHome();
}

// Four columns: the time or the day, the temperature, the sky, and at the right edge the chance
// of rain or the day's high and low.
void WeatherActivity::draw() {
  if (!agenda || !agenda->hasWeather) return;
  const ui::Frame f = ui::frame(renderer);
  const int font = UI_12_FONT_ID;
  const int lh = renderer.getLineHeight(font) + LINE_GAP;
  const int right = f.left + f.width;
  const int tempRight = f.left + renderer.getTextWidth(font, "00:00") + COLUMN_GAP + renderer.getTextWidth(font, "-00\xC2\xB0");
  const int skyX = tempRight + COLUMN_GAP;
  int y = f.top;

  const net::WeatherHour* hours[net::Weather::kHours];
  const uint8_t hourCount = net::hoursFrom(*agenda, today, hour, hours, net::Weather::kHours);
  for (uint8_t i = 0; i < hourCount; ++i) {
    const net::WeatherHour& h = *hours[i];
    char text[16];
    snprintf(text, sizeof(text), "%02u:00", static_cast<unsigned>(h.hour));
    renderer.drawText(font, f.left, y, text);
    snprintf(text, sizeof(text), "%d\xC2\xB0", h.temp);
    renderer.drawText(font, tempRight - renderer.getTextWidth(font, text), y, text);
    renderer.drawText(font, skyX, y, net::conditionName(h.code));
    snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(h.rain));
    renderer.drawText(font, right - renderer.getTextWidth(font, text), y, text);
    y += lh;
  }
  if (hourCount > 0) y += BLOCK_GAP - LINE_GAP;

  for (uint8_t i = 0; i < agenda->weather.count; ++i) {
    const net::WeatherDay& d = agenda->weather.days[i];
    if (d.day < today) continue;
    char text[24];
    snprintf(text, sizeof(text), "%.3s", I18N.get(WEEKDAYS[net::weekdayOf(d.day)]));
    renderer.drawText(font, f.left, y, text);
    renderer.drawText(font, skyX, y, net::conditionName(d.code));
    net::formatTemperatures(d, text, sizeof(text));
    renderer.drawText(font, right - renderer.getTextWidth(font, text), y, text);
    y += lh;
  }
}

void WeatherActivity::render(RenderLock&&) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    draw();
    scope.endScanAndPrewarm();
  }
  renderer.clearScreen();
  draw();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
