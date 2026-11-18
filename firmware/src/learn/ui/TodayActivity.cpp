#include "TodayActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "PaperActivity.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "WeatherActivity.h"
#include "learn/Learn.h"
#include "learn/ReadList.h"
#include "learn/Visits.h"
#include "net/Football.h"
#include "net/NetSync.h"
#include "play/Game.h"
#include "play/Play.h"
#include "play/ui/PlayActivity.h"
#include "util/BookProgress.h"
#include "util/HeapLows.h"

namespace learn = x4::learn;
namespace paper = x4::paper;

namespace {

constexpr unsigned long CLOCK_POLL_MS = 1000;
constexpr unsigned long REST_MS = 2000;  // no press for this long: the X4 may go online
// Fast refreshes between two that clean the ghosting, as the reader's page turns.
constexpr uint8_t CLEAN_EVERY = 10;

constexpr StrId WEEKDAYS[7] = {StrId::STR_SUNDAY,   StrId::STR_MONDAY, StrId::STR_TUESDAY, StrId::STR_WEDNESDAY,
                               StrId::STR_THURSDAY, StrId::STR_FRIDAY, StrId::STR_SATURDAY};
constexpr StrId MONTHS[12] = {StrId::STR_JANUARY,   StrId::STR_FEBRUARY, StrId::STR_MARCH,    StrId::STR_APRIL,
                              StrId::STR_MAY,       StrId::STR_JUNE,     StrId::STR_JULY,     StrId::STR_AUGUST,
                              StrId::STR_SEPTEMBER, StrId::STR_OCTOBER,  StrId::STR_NOVEMBER, StrId::STR_DECEMBER};

}  // namespace

void TodayActivity::onEnter() {
  Activity::onEnter();
  x4::heap::mark("away");  // since the last mark: the boot, a book, a game, the Library
  // Headlines use the reader's family, which may come from the SD card.
  sdFontSystem.ensureLoaded(renderer);
  faces = paper::faces();
  learn::ensureReady();
  clockCheckedAt = millis();
  lastInputAt = millis();
  loadAgenda();
  loadIssue();
  loadBook();
  loadPlay();
  writeMasthead();

  paper::Place& place = paper::place();
  place.screen = paper::Screen::Front;
  if (!front || place.row >= paper::rows(*front)) place.row = 0;
  x4::heap::mark("today");  // the issue, the agenda and the match loaded
  requestUpdate();
}

void TodayActivity::onExit() {
  Activity::onExit();
  // The next screen needs the RAM a fetch holds.
  x4::net::stop();
  agenda.reset();
  front.reset();
  bookPath.clear();
  bookPath.shrink_to_fit();
  bookTitle.clear();
  bookTitle.shrink_to_fit();
}

// The issue's sections with their lines. Football's line is the match that matters now.
void TodayActivity::loadIssue() {
  auto loaded = makeUniqueNoThrow<paper::Front>();
  auto buffer = makeUniqueNoThrow<x4::LineBuffer>();
  if (!loaded || !buffer) {
    LOG_ERR("PAPER", "OOM: front page");
    return;
  }
  // A card without an issue has no sections: Play, Book and Library alone.
  if (!paper::loadFront(*buffer, *loaded)) *loaded = paper::Front{};
  x4::visits::noteIssue(loaded->date);
  // A new issue: saved links arrive kept, and what was neither read nor kept leaves the card.
  paper::settle(*buffer, *loaded);
  buffer.reset();
  const int football = loaded->find(paper::FOOTBALL);
  if (football >= 0) {
    if (const std::unique_ptr<x4::net::Football> data = x4::net::loadFootball(false)) {
      if (const x4::net::Match* m = x4::net::headline(*data, learn::wallClock(), zone)) {
        x4::net::formatMatch(*m, loaded->sections[football].head, sizeof(loaded->sections[football].head));
      }
    }
  }
  front = std::move(loaded);
}

void TodayActivity::loadBook() {
  bookPercent[0] = '\0';
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    // Pieces are in their sections; files under /x4/ are the device's own data, never a book.
    if (RecentBooksStore::isMissing(book) || x4::readlist::isReadBook(book.path.c_str()) ||
        book.path.rfind("/x4/", 0) == 0) {
      continue;
    }
    bookPath = book.path;
    bookTitle = book.title.empty() ? book.path.substr(book.path.find_last_of('/') + 1) : book.title;
    const int percent = loadBookProgress(book.path);
    if (percent > 0) snprintf(bookPercent, sizeof(bookPercent), "%d%%", std::min(percent, 100));
    break;
  }
}

// The first game whose daily puzzle is still open; none when all are played.
void TodayActivity::loadPlay() {
  playLine[0] = '\0';
  const uint32_t day = x4::play::dailyDay(learn::today());
  for (uint8_t i = 0; i < x4::play::gameCount(); ++i) {
    const x4::play::GameInfo& game = x4::play::games()[i];
    x4::play::Lang lang;
    if (!x4::play::activeLang(game, lang) || x4::play::dailyDone(game.id, day)) continue;
    snprintf(playLine, sizeof(playLine), "%s", I18N.get(game.name));
    return;
  }
}

void TodayActivity::loadAgenda() {
  agenda = x4::net::loadAgenda();
  zone = x4::net::zone();
  agendaGeneration = x4::net::generation();
}

// Today's forecast is known, so there is a forecast to open.
bool TodayActivity::weatherShown() const {
  if (!date[0] || !agenda || !agenda->hasWeather) return false;
  return x4::net::weatherOfDay(*agenda, day) != nullptr;
}

// Writes the date, and the weather and the next event after it. True when either changed.
bool TodayActivity::writeMasthead() {
  char newDate[sizeof(date)] = "";
  char line[sizeof(agendaLine)] = "";
  x4::CivilTime now{};
  if (learn::localTime(now)) {
    snprintf(newDate, sizeof(newDate), "%.3s %u %.3s", I18N.get(WEEKDAYS[now.weekday % 7]),
             static_cast<unsigned>(now.day), I18N.get(MONTHS[(now.month + 11) % 12]));
    const int64_t clock = learn::wallClock();
    day = x4::net::localDay(clock, zone);
    if (agenda) {
      size_t used = 0;
      if (const x4::net::WeatherDay* weather = agenda->hasWeather ? x4::net::weatherOfDay(*agenda, day) : nullptr) {
        char temps[24];
        x4::net::formatTemperatures(*weather, temps, sizeof(temps));
        used = snprintf(line, sizeof(line), "%s %s", temps, x4::net::conditionName(weather->code));
      }
      const x4::net::Event* next[1];
      if (agenda->hasEvents && x4::net::eventsOfDay(*agenda, zone, day, clock, next, 1) == 1 && used < sizeof(line)) {
        char when[8] = "";
        if (!next[0]->allDay) x4::net::formatClock(next[0]->start, zone, when, sizeof(when));
        snprintf(line + used, sizeof(line) - used, "%s%s%s%s", used ? " \xC2\xB7 " : "", when, when[0] ? " " : "",
                 next[0]->title[0] ? next[0]->title : "Busy");
      }
    }
  }
  const bool changed = std::strcmp(newDate, date) != 0 || std::strcmp(line, agendaLine) != 0;
  std::memcpy(date, newDate, sizeof(date));
  std::memcpy(agendaLine, line, sizeof(agendaLine));
  if (onMasthead && !weatherShown()) onMasthead = false;
  return changed;
}

void TodayActivity::open(const paper::Go go) {
  switch (go) {
    case paper::Go::Move:
      requestUpdate();
      return;
    case paper::Go::Section:
      activityManager.replaceActivity(std::make_unique<SectionActivity>(renderer, mappedInput));
      return;
    case paper::Go::Play:
      activityManager.replaceActivity(std::make_unique<PlayActivity>(renderer, mappedInput));
      return;
    case paper::Go::Book:
      // Before the first book the row opens the Library, where books are.
      if (bookPath.empty()) {
        activityManager.goToLibrary();
      } else {
        activityManager.goToReader(bookPath);
      }
      return;
    case paper::Go::Library:
      activityManager.goToLibrary();
      return;
    default:
      return;
  }
}

void TodayActivity::loop() {
  using Button = MappedInputManager::Button;
  for (const Button b : {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down}) {
    if (mappedInput.isPressed(b)) lastInputAt = millis();
  }
  // A fetch has the least memory to spare of anything the X4 does: about 10 KB free at its
  // lowest with the page's glyphs held, 18 KB without. The page is drawn, so its glyphs go
  // before the fetch asks for room, and are loaded again at the next draw.
  x4::net::poll(
      millis() - lastInputAt >= REST_MS,
      [](void* self) {
        auto* today = static_cast<TodayActivity*>(self);
        RenderLock lock(*today);
        if (auto* fcm = today->renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
      },
      this);
  if (x4::net::generation() != agendaGeneration) {
    // A fetch ended: the agenda may be new, and so may the issue.
    {
      RenderLock lock(*this);
      loadAgenda();
      loadIssue();
      writeMasthead();
      if (front && paper::place().row >= paper::rows(*front)) paper::place().row = 0;
    }
    requestUpdate();
  }
  if (!front) return;
  paper::Place& place = paper::place();
  const uint8_t last = paper::rows(*front) - 1;

  // The masthead is one Up above the first row, and the last row one Up above it.
  if (mappedInput.wasPressed(Button::Up)) {
    if (onMasthead) {
      onMasthead = false;
      place.row = last;
    } else if (place.row == 0 && weatherShown()) {
      onMasthead = true;
    } else {
      paper::press(place, *front, 0, paper::Key::Up);
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(Button::Down)) {
    if (onMasthead) {
      onMasthead = false;
      place.row = 0;
    } else if (place.row == last && weatherShown()) {
      onMasthead = true;
    } else {
      paper::press(place, *front, 0, paper::Key::Down);
    }
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    if (onMasthead) {
      activityManager.replaceActivity(std::make_unique<WeatherActivity>(renderer, mappedInput));
      return;
    }
    open(paper::press(place, *front, 0, paper::Key::Confirm));
    return;
  }
  // The day turns over, or the clock becomes known: the masthead is written again.
  if (millis() - clockCheckedAt >= CLOCK_POLL_MS) {
    clockCheckedAt = millis();
    bool changed = false;
    {
      RenderLock lock(*this);
      changed = writeMasthead();
    }
    if (changed) requestUpdate();
  }
}

void TodayActivity::render(RenderLock&&) {
  if (!front) return;
  paper::FrontView view;
  view.front = front.get();
  view.date = date;
  view.agenda = agendaLine;
  view.play = playLine;
  view.book = bookTitle.c_str();
  view.percent = bookPercent;
  view.selected = paper::place().row;
  view.masthead = onMasthead;
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    paper::drawFront(renderer, faces, view, false);
    scope.endScanAndPrewarm();
  }
  renderer.clearScreen();
  paper::drawFront(renderer, faces, view, false);
  const bool clean = (cleanInitialRefresh && firstRender) || ++fastRefreshes >= CLEAN_EVERY;
  if (clean) fastRefreshes = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
  x4::heap::mark("draw");  // the front page drawn: its glyphs loaded, perhaps while a fetch was open
}
