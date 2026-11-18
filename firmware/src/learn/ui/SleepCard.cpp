#include "SleepCard.h"

#include <FontCacheManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <memory>

#include "AgendaView.h"
#include "CrossPointSettings.h"
#include "LearnUi.h"
#include "SdCardFontSystem.h"
#include "fontIds.h"
#include "learn/Learn.h"
#include "net/NetSync.h"

namespace x4::ui {

namespace {

constexpr StrId WEEKDAYS[7] = {StrId::STR_SUNDAY,   StrId::STR_MONDAY, StrId::STR_TUESDAY, StrId::STR_WEDNESDAY,
                               StrId::STR_THURSDAY, StrId::STR_FRIDAY, StrId::STR_SATURDAY};
constexpr int EVENING_HOUR = 18;  // from then on, the sleep screen shows tomorrow
constexpr uint8_t SLEEP_EVENTS = 4;

// The day the sleep screen looks ahead to: the rest of today, or tomorrow in the evening.
struct AgendaDay {
  const net::Agenda* agenda = nullptr;
  net::TzRule zone;
  int32_t day = 0;
  int64_t after = 0;
  const char* heading = nullptr;

  bool any() const {
    if (!agenda) return false;
    const net::Event* events[1];
    return (agenda->hasWeather && net::weatherOfDay(*agenda, day)) ||
           (agenda->hasEvents && net::eventsOfDay(*agenda, zone, day, after, events, 1) > 0);
  }
};

struct SleepCard {
  GfxRenderer& renderer;
  const LineBuffer* buffer;  // null without a card
  const Item* item;
  const AgendaDay& agenda;

  const char* str(const Slice slice) const { return slice.length ? text(buffer->text, slice) : ""; }

  void draw() const {
    const Frame f = frame(renderer);
    const int font = SETTINGS.getReaderFontId();
    const int lh = renderer.getLineHeight(font, SETTINGS.getReaderLineCompression());
    int top = f.top;
    if (agenda.any()) {
      const int agendaTop = f.top - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      const int end = drawAgenda(renderer, f.left, f.width, agendaTop, *agenda.agenda, agenda.zone, agenda.day,
                                 agenda.after, SLEEP_EVENTS, agenda.heading);
      top = end + lh;
    }
    if (!item) return;
    const bool highlight = item->shape == Shape::Highlight;
    const char* main = highlight ? str(item->body) : str(item->title);
    const char* source = highlight ? str(item->title) : "";

    // Centre the block a little above the middle of the space left.
    const int bottom = f.dotsY;
    const int lines = flowText(renderer, font, EpdFontFamily::REGULAR, main, f.width, nullptr, nullptr);
    const int height = lines * lh + (*source ? 2 * lh : 0);
    const int y = std::max(top, top + (bottom - top - height) * 2 / 5);
    TextBox box{font, EpdFontFamily::REGULAR, f.left, y, f.width, lh, bottom - (*source ? 2 * lh : 0)};
    const int end = drawTextBox(renderer, box, main);
    if (*source) {
      box.y = end + lh;
      box.bottom = bottom;
      box.style = EpdFontFamily::ITALIC;
      drawTextBox(renderer, box, source);
    }
  }
};

void drawGray(void* card) { static_cast<const SleepCard*>(card)->draw(); }

}  // namespace

bool renderSleepCard(GfxRenderer& renderer, const HalDisplay::RefreshMode refresh) {
  // A fresh agenda, and with it the clock, when the last fetch is old.
  net::refreshBeforeSleep();

  AgendaDay day;
  std::unique_ptr<net::Agenda> agenda;
  if (const int64_t now = learn::wallClock(); now > 0) {
    agenda = net::loadAgenda();
    day.zone = net::zone();
    day.day = net::localDay(now, day.zone);
    const int64_t secondsToday = day.zone.toLocal(now) - static_cast<int64_t>(day.day) * 86400;
    if (secondsToday >= EVENING_HOUR * 3600) {
      ++day.day;
    } else {
      day.after = now;
    }
    day.agenda = agenda.get();
    day.heading = I18N.get(WEEKDAYS[net::weekdayOf(day.day)]);
  }

  // The card's strings, for this screen only (about 10 KB).
  auto buffer = makeUniqueNoThrow<LineBuffer>();
  if (!buffer) LOG_ERR("LEARN", "OOM: sleep card (%u bytes)", static_cast<unsigned>(sizeof(LineBuffer)));
  Item item;
  bool hasCard = buffer && learn::pickSleepItem(*buffer, item);
  if (hasCard) {
    const char* main = item.shape == Shape::Highlight ? item.body.length ? text(buffer->text, item.body) : ""
                                                      : item.title.length ? text(buffer->text, item.title) : "";
    hasCard = *main != '\0';
  }
  if (!hasCard && !day.any()) return false;

  sdFontSystem.ensureLoaded(renderer);
  const SleepCard card{renderer, hasCard ? buffer.get() : nullptr, hasCard ? &item : nullptr, day};
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    card.draw();
    scope.endScanAndPrewarm();
  }
  renderer.clearScreen();
  card.draw();
  if (refresh == HalDisplay::HALF_REFRESH) {
    // A clean base, then the text's grays when anti-aliasing is on.
    int pages = 1;
    displayWithAntiAliasing(renderer, pages, &drawGray, const_cast<SleepCard*>(&card));
  } else {
    renderer.displayBuffer(refresh);
  }
  return true;
}

}  // namespace x4::ui
