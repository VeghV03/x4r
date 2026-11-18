#include "PlayActivity.h"

#include <FontCacheManager.h>
#include <I18n.h>

#include <cstdio>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "GameActivity.h"
#include "SdCardFontSystem.h"
#include "learn/Learn.h"
#include "learn/Visits.h"
#include "learn/ui/LearnUi.h"
#include "learn/ui/StreamActivity.h"
#include "play/Game.h"
#include "play/ui/PlayUi.h"

namespace play = x4::play;
namespace ui = x4::ui;

namespace {

constexpr unsigned long LANG_HOLD_MS = 500;

int readerFont() { return SETTINGS.getReaderFontId(); }

}  // namespace

void PlayActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.ensureLoaded(renderer);
  x4::learn::ensureReady();
  const uint32_t day = play::dailyDay(x4::learn::today());
  count = 0;
  for (uint8_t i = 0; i < play::gameCount() && count < play::GAMES_MAX; ++i) {
    const play::GameInfo& game = play::games()[i];
    Row& row = rows[count];
    row = Row{};
    if (!play::activeLang(game, row.lang)) continue;
    const play::Standing standing = play::standing(game.id);
    row.game = i;
    row.level = standing.level;
    row.dailyDone = day != 0 && standing.dailyDay == day;
    row.words = game.languages;
    if (selectedId && std::strcmp(selectedId, game.id) == 0) selected = count;
    ++count;
  }
  if (x4::learn::hasUnseen(&x4::isBookPuzzle)) {
    rows[count] = Row{};
    rows[count].puzzles = true;
    if (selectedId && std::strcmp(selectedId, PUZZLES_ROW) == 0) selected = count;
    ++count;
  }
  x4::visits::enter("play");
  requestUpdate();
}

void PlayActivity::onExit() {
  Activity::onExit();
  x4::visits::leave();
}

void PlayActivity::loop() {
  using Button = MappedInputManager::Button;
  if (x4::ui::anyPress(mappedInput)) x4::visits::press();
  navigator.onNext([this] {
    selected = static_cast<uint8_t>(ButtonNavigator::nextIndex(selected, count));
    requestUpdate();
  });
  navigator.onPrevious([this] {
    selected = static_cast<uint8_t>(ButtonNavigator::previousIndex(selected, count));
    requestUpdate();
  });
  if (mappedInput.wasReleased(Button::Back)) {
    activityManager.goHome();
    return;
  }
  if (count == 0) return;
  if (mappedInput.wasLongPressed(Button::Confirm, LANG_HOLD_MS)) {
    if (switchLang()) requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    if (rows[selected].puzzles) {
      activityManager.replaceActivity(
          std::make_unique<StreamActivity>(renderer, mappedInput, StreamActivity::Outside::Puzzles));
      return;
    }
    activityManager.replaceActivity(
        std::make_unique<GameActivity>(renderer, mappedInput, play::games()[rows[selected].game]));
  }
}

bool PlayActivity::switchLang() {
  Row& row = rows[selected];
  if (row.puzzles) return false;
  const play::GameInfo& game = play::games()[row.game];
  if (!row.words) return false;
  const auto other = static_cast<play::Lang>((static_cast<uint8_t>(row.lang) + 1) % play::LANGS);
  if (!game.ready(other)) return false;
  play::setLang(game.id, other);
  row.lang = other;
  return true;
}

int PlayActivity::drawRows(const bool scanOnly) {
  const ui::Frame f = ui::frame(renderer);
  const int font = readerFont();
  const int lh = renderer.getLineHeight(font, SETTINGS.getReaderLineCompression());
  const char* title = tr(STR_PLAY);
  ui::drawTabs(renderer, f, &title, 1, 0);
  int y = f.top;
  for (uint8_t i = 0; i < count; ++i) {
    // The level at the right edge, like Home's counts; a done daily shows the check instead.
    char level[4];
    snprintf(level, sizeof(level), "%u", static_cast<unsigned>(rows[i].level));
    ui::MenuRow row;
    const char* id = rows[i].puzzles ? PUZZLES_ROW : play::games()[rows[i].game].id;
    row.icon = scanOnly ? nullptr : &play::ui::gameIcon(id);
    row.label = rows[i].puzzles ? tr(STR_GAME_BOOK_PUZZLES) : I18n::getInstance().get(play::games()[rows[i].game].name);
    row.style = EpdFontFamily::REGULAR;
    // The book puzzles have no level: they are the books' own.
    if (!rows[i].puzzles) row.value = level;
    if (rows[i].words) row.tag = rows[i].lang == play::Lang::Hu ? "HU" : "EN";
    row.check = !scanOnly && rows[i].dailyDone;
    y = ui::drawMenuRow(renderer, f, font, lh, y, row, !scanOnly && i == selected);
  }
  return y;
}

void PlayActivity::render(RenderLock&&) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    drawRows(true);
    scope.endScanAndPrewarm();
  }
  renderer.clearScreen();
  drawRows(false);
  // HALF clears what was there before; then fast, with a clean one every 10 as on Today.
  const bool clean = firstRender || ++fastRefreshes >= 10;
  if (clean) fastRefreshes = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
}
