#include "FootballActivity.h"

#include <FontCacheManager.h>
#include <I18n.h>
#include <Memory.h>

#include <climits>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "LearnUi.h"
#include "PaperActivity.h"
#include "SdCardFontSystem.h"
#include "fontIds.h"
#include "learn/Learn.h"
#include "learn/Visits.h"
#include "net/Agenda.h"
#include "net/NetSync.h"

namespace ui = x4::ui;
namespace net = x4::net;

namespace {

constexpr int HEADING_GAP = 18;  // above a day's heading, as the Library's
constexpr int TABLE_PAD = 5;     // above and below a table row
constexpr int POS_GAP = 14;      // a table's position to its team
constexpr uint8_t CLEAN_EVERY = 10;

constexpr StrId WEEKDAYS[7] = {StrId::STR_SUNDAY,   StrId::STR_MONDAY, StrId::STR_TUESDAY, StrId::STR_WEDNESDAY,
                               StrId::STR_THURSDAY, StrId::STR_FRIDAY, StrId::STR_SATURDAY};
constexpr StrId MONTHS[12] = {StrId::STR_JANUARY,   StrId::STR_FEBRUARY, StrId::STR_MARCH,    StrId::STR_APRIL,
                              StrId::STR_MAY,       StrId::STR_JUNE,     StrId::STR_JULY,     StrId::STR_AUGUST,
                              StrId::STR_SEPTEMBER, StrId::STR_OCTOBER,  StrId::STR_NOVEMBER, StrId::STR_DECEMBER};

int readerFont() { return SETTINGS.getReaderFontId(); }

// "Saturday 8 November"
void formatDay(const int32_t day, char* out, const size_t size) {
  int32_t year = 0;
  int32_t month = 0;
  int32_t date = 0;
  net::civilFromDays(day, year, month, date);
  snprintf(out, size, "%s %d %s", I18N.get(WEEKDAYS[net::weekdayOf(day)]), static_cast<int>(date),
           I18N.get(MONTHS[(month + 11) % 12]));
}

}  // namespace

void FootballActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.ensureLoaded(renderer);
  faces = x4::paper::faces();
  football = net::loadFootball(true);
  zone = net::zone();
  now = x4::learn::wallClock();
  x4::visits::enter(x4::paper::FOOTBALL);
  openTab(fromRight ? static_cast<uint8_t>(tabCount() - 1) : 0);
  requestUpdate();
}

void FootballActivity::onExit() {
  Activity::onExit();
  if (!continues) x4::visits::leave();
  football.reset();
}

// Leafs out of the tables into the paper. False when there is nowhere to go.
bool FootballActivity::leafOut(const bool right) {
  auto buffer = makeUniqueNoThrow<x4::LineBuffer>();
  auto front = makeUniqueNoThrow<x4::paper::Front>();
  if (!buffer || !front || !x4::paper::loadFront(*buffer, *front)) return false;
  x4::paper::Place& place = x4::paper::place();
  const int at = front->find(x4::paper::FOOTBALL);
  if (at < 0) return false;
  place.section = static_cast<uint8_t>(at);
  if (x4::paper::leaveFootball(place, *front, right) != x4::paper::Go::Section) return false;
  buffer.reset();
  front.reset();
  continues = true;
  activityManager.replaceActivity(std::make_unique<SectionActivity>(renderer, mappedInput));
  return true;
}

uint8_t FootballActivity::tabCount() const { return 1 + (football ? football->tableCount : 0); }

ui::Frame FootballActivity::pageFrame() const {
  ui::Frame f = ui::frame(renderer);
  f.left = x4::paper::PAGE_MARGIN;
  f.width = renderer.getScreenWidth() - 2 * x4::paper::PAGE_MARGIN;
  f.top = x4::paper::belowTabs(renderer, faces);
  return f;
}

void FootballActivity::openTab(const uint8_t index) {
  tab = index;
  int focus = 0;
  layout(-1, &focus);
  page = focus;
}

int FootballActivity::layout(const int drawPage, int* focusPage) {
  const ui::Frame f = pageFrame();
  const int font = readerFont();
  const int lh = renderer.getLineHeight(font, SETTINGS.getReaderLineCompression());
  const bool drawing = drawPage >= 0;
  int pages = 1;
  int y = f.top;
  if (focusPage) *focusPage = 0;
  if (!football) return pages;

  if (tab == 0) {
    const net::Match* focus = net::headline(*football, now, zone);
    const int smallLh = renderer.getLineHeight(UI_12_FONT_ID);
    int32_t lastDay = INT32_MIN;
    for (uint8_t i = 0; i < football->matchCount; ++i) {
      const net::Match& m = football->matches[i];
      const int32_t day = net::localDay(m.at, zone);
      const int rowHeight = 2 * ui::ROW_PAD + lh;
      // Every page and every new day opens with the day's name; a heading keeps with its first match.
      bool heading = day != lastDay;
      const int need = (heading ? (y > f.top ? HEADING_GAP : 0) + smallLh : 0) + rowHeight;
      if (y + need > f.bottom && y > f.top) {
        ++pages;
        y = f.top;
        heading = true;
      }
      const bool shown = drawing && pages - 1 == drawPage;
      if (focusPage && &m == focus) *focusPage = pages - 1;
      if (heading) {
        if (y > f.top) y += HEADING_GAP;
        if (shown) {
          char name[48];
          formatDay(day, name, sizeof(name));
          renderer.drawText(UI_12_FONT_ID, f.left, y, name);
        }
        y += smallLh;
        lastDay = day;
      }
      if (shown) {
        char line[64];
        net::formatMatch(m, line, sizeof(line));
        char clock[8] = "";
        if (!m.over()) net::formatClock(m.at, zone, clock, sizeof(clock));
        ui::MenuRow row;
        row.plain = true;
        row.label = line;
        row.style = m.live ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
        // The teams take the row; a fixture adds its kick-off. The tables name the competitions.
        row.value = clock;
        ui::drawMenuRow(renderer, f, font, lh, y, row, false);
      }
      y += rowHeight;
    }
    return pages;
  }

  const net::Table& table = football->tables[tab - 1];
  const int posWidth = renderer.getTextWidth(font, "00");
  bool focused = false;
  for (uint8_t i = 0; i < table.count; ++i) {
    const net::TableRow& r = table.rows[i];
    const int h = 2 * TABLE_PAD + lh;
    if (y + h > f.bottom && y > f.top) {
      ++pages;
      y = f.top;
    }
    const bool shown = drawing && pages - 1 == drawPage;
    if (focusPage && r.mine && !focused) {
      *focusPage = pages - 1;
      focused = true;
    }
    if (shown) {
      const auto style = r.mine ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      const int lineTop = y + TABLE_PAD;
      char pos[4];
      snprintf(pos, sizeof(pos), "%u", static_cast<unsigned>(r.pos));
      renderer.drawText(font, f.left + posWidth - renderer.getTextWidth(font, pos, style), lineTop, pos, true, style);
      char points[6];
      snprintf(points, sizeof(points), "%u", static_cast<unsigned>(r.points));
      const int pointsX = f.left + f.width - renderer.getTextWidth(font, points, style);
      renderer.drawText(font, pointsX, lineTop, points, true, style);
      const int teamX = f.left + posWidth + POS_GAP;
      const ui::TextBox box{font, style, teamX, lineTop, pointsX - POS_GAP - teamX, lh, lineTop + lh};
      ui::drawTextBox(renderer, box, r.team);
    }
    y += h;
  }
  return pages;
}

void FootballActivity::loop() {
  using Button = MappedInputManager::Button;
  if (ui::anyPress(mappedInput)) x4::visits::press();
  if (mappedInput.wasReleased(Button::Back)) {
    activityManager.goHome();
    return;
  }
  const uint8_t tabs = tabCount();
  if (mappedInput.wasPressed(Button::Right) || mappedInput.wasPressed(Button::Left)) {
    const bool forward = mappedInput.wasPressed(Button::Right);
    if (forward ? tab + 1 < tabs : tab > 0) {
      openTab(static_cast<uint8_t>(forward ? tab + 1 : tab - 1));
      requestUpdate();
      return;
    }
    // Past its ends Football is a page of the paper: its pieces before it, the next section after.
    if (leafOut(forward)) return;
  }
  const bool down = mappedInput.wasPressed(Button::Down);
  if (down || mappedInput.wasPressed(Button::Up)) {
    const int pages = layout(-1, nullptr);
    const int next = down ? page + 1 : page - 1;
    if (next >= 0 && next < pages) {
      page = next;
      requestUpdate();
    }
  }
}

void FootballActivity::draw(const bool scanOnly) {
  const ui::Frame f = pageFrame();
  const char* names[2 + net::Football::kTables];
  names[0] = tr(STR_ANALYSIS);
  names[1] = tr(STR_MATCHES);
  const uint8_t tabs = tabCount();
  for (uint8_t t = 1; t < tabs; ++t) names[t + 1] = football->tables[t - 1].comp;
  x4::paper::drawTabs(renderer, faces, names, static_cast<uint8_t>(tabs + 1), static_cast<uint8_t>(tab + 1), false);
  const int pages = layout(page, nullptr);
  if (!scanOnly && pages > 1) ui::drawProgressDots(renderer, page, pages, f.dotsY);
}

void FootballActivity::render(RenderLock&&) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    draw(true);
    scope.endScanAndPrewarm();
  }
  renderer.clearScreen();
  draw(false);
  const bool clean = firstRender || ++fastRefreshes >= CLEAN_EVERY;
  if (clean) fastRefreshes = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
}
