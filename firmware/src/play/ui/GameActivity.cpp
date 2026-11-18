#include "GameActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "PlayActivity.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "learn/Learn.h"
#include "learn/ui/LearnUi.h"

namespace learn = x4::learn;
namespace play = x4::play;
namespace ui = x4::ui;

namespace {

constexpr unsigned long NOTE_HOLD_MS = 500;
// The check or cross above a finished board, in the margin games leave above `f.top - 20`.
constexpr int MARK = 32;
constexpr int MARK_GAP = 6;
// One visit counts at most this long towards `ms` (a game left on screen is not played).
constexpr uint32_t VISIT_MS_MAX = 60UL * 60UL * 1000UL;

}  // namespace

void GameActivity::onEnter() {
  Activity::onEnter();
  learn::ensureReady();
  const uint32_t day = play::dailyDay(learn::today());
  game = info.make();
  if (!game) {
    LOG_ERR("PLAY", "OOM: game %s", info.id);
    leave();  // nothing to draw: back where it was opened
    return;
  }

  play::Lang lang = play::DEFAULT_LANG;
  play::activeLang(info, lang);
  bool resumed = false;
  if (auto state = makeUniqueNoThrow<uint8_t[]>(play::SAVE_MAX)) {
    resumed = play::loadGame(info.id, saved, state.get()) && game->restore(state.get(), saved.size);
  }
  // A word game left in the other language starts over in this one.
  if (resumed && game->lang() && std::strcmp(game->lang(), play::langCode(lang)) != 0) resumed = false;
  game->setLang(lang);
  if (resumed) {
    // A daily left on an earlier day no longer counts as today's.
    if (saved.daily && saved.day != day) {
      saved.daily = false;
      saved.mode = play::Mode::Practice;
    }
  } else {
    saved = play::Saved{};
    saved.day = day;
    saved.level = play::standing(info.id).level;
    saved.daily = !play::dailyDone(info.id, day);
    saved.mode = saved.daily ? play::Mode::Daily : play::Mode::Practice;
    game->start(saved.daily ? play::dailySeed(info.id, day) : play::nextPracticeSeed(info.id, day), saved.level);
  }
  if (!game->playable()) {
    LOG_ERR("PLAY", "Nothing to play: %s", info.id);
    game.reset();
    leave();
    return;
  }
  resumedAt = millis();
  // A board saved as it finished ends now, on that board.
  if (game->outcome() != play::Outcome::Playing) end();
  // A new puzzle is saved at the first pause, so a reboot brings back this one.
  if (!resumed) autosave.changed(resumedAt);
  // The first screen gets a HALF refresh to clear what was there before.
  pagesUntilFullRefresh = 1;
  requestUpdate();
}

void GameActivity::onExit() {
  Activity::onExit();
  if (game && !finished) saveProgress();
  game.reset();
}

uint32_t GameActivity::playMs() const {
  const uint32_t visit = millis() - resumedAt;
  return saved.ms + (visit < VISIT_MS_MAX ? visit : VISIT_MS_MAX);
}

void GameActivity::saveProgress() {
  // A failed save is logged and tried again after the next change.
  autosave.saved();
  auto state = makeUniqueNoThrow<uint8_t[]>(play::SAVE_MAX);
  if (!state) {
    LOG_ERR("PLAY", "OOM: saving %s", info.id);
    return;
  }
  play::Saved out = saved;
  out.size = static_cast<uint16_t>(game->save(state.get()));
  out.ms = playMs();
  play::saveGame(info.id, out, state.get());
}

void GameActivity::end() {
  finished = true;
  resultGuard = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                mappedInput.isPressed(MappedInputManager::Button::Back);
  won = game->outcome() == play::Outcome::Won;
  play::Result result;
  result.game = info.id;
  result.lang = game->lang();
  result.mode = saved.mode;
  result.daily = saved.daily;
  result.level = saved.level;
  result.won = won;
  result.score = game->score();
  result.tries = game->tries();
  result.ms = playMs();
  play::finish(result, saved.day, learn::wallClock());
  play::clearGame(info.id);
  learn::refreshStats();
}

void GameActivity::leave() {
  activityManager.replaceActivity(std::make_unique<PlayActivity>(renderer, mappedInput, info.id));
}

void GameActivity::loop() {
  using Button = MappedInputManager::Button;
  if (!game || finished) {
    // The press that ended the game must not also leave its board.
    if (resultGuard) {
      if (!mappedInput.isPressed(Button::Confirm) && !mappedInput.isPressed(Button::Back)) resultGuard = false;
      return;
    }
    // On release: a press would leave its release to the screen it opens, which would act on it.
    if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasReleased(Button::Right) ||
        mappedInput.wasReleased(Button::Back)) {
      leave();
    }
    return;
  }
  if (autosave.due(millis(), game->resumable())) saveProgress();
  if (mappedInput.wasReleased(Button::Back)) {
    bool took = false;
    {
      RenderLock lock(*this);
      took = game->back();
      if (took) autosave.changed(millis());
    }
    if (took) {
      requestUpdate();
    } else {
      leave();
    }
    return;
  }

  play::Keys keys;
  navigator.onPressAndContinuous({Button::Up}, [&keys] { keys.up = true; });
  navigator.onPressAndContinuous({Button::Down}, [&keys] { keys.down = true; });
  navigator.onPressAndContinuous({Button::Left}, [&keys] { keys.left = true; });
  navigator.onPressAndContinuous({Button::Right}, [&keys] { keys.right = true; });
  keys.confirmHold = mappedInput.wasLongPressed(Button::Confirm, NOTE_HOLD_MS);
  keys.confirm = mappedInput.wasPressed(Button::Confirm);
  if (mappedInput.wasReleased(Button::Confirm)) beforePress.reset();
  const bool pressed = keys.up || keys.down || keys.left || keys.right || keys.confirm || keys.confirmHold;
  if (!pressed && !game->paced()) return;

  bool changed = false;
  {
    // State read by render(): change it under the render lock.
    RenderLock lock(*this);
    const uint32_t now = millis();
    if (keys.confirm && game->holdsConfirm()) {
      // A press shows at once; should it become a hold, the hold starts from the game before it.
      beforePress = makeUniqueNoThrow<uint8_t[]>(play::SAVE_MAX);
      if (beforePress) beforePressSize = game->save(beforePress.get());
    }
    if (keys.confirmHold && beforePress) {
      changed = game->restore(beforePress.get(), beforePressSize);
      beforePress.reset();
    }
    if (pressed) changed = game->input(keys, now) || changed;
    if (game->paced()) changed = game->tick(now) || changed;
    if (game->outcome() != play::Outcome::Playing) {
      end();
      changed = true;
    } else if (changed) {
      autosave.changed(now);
    }
  }
  if (changed) requestUpdate();
}

void GameActivity::drawMark(const ui::Frame& f) const {
  const int x = (renderer.getScreenWidth() - MARK) / 2;
  const int y = f.top - 20 - MARK_GAP - MARK;
  if (won) {
    ui::drawCheck(renderer, x, y, MARK);
  } else {
    ui::drawCross(renderer, x, y, MARK);
  }
}

void GameActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const ui::Frame f = ui::frame(renderer);
  if (game) game->draw(renderer, f);
  if (game && finished && !game->marksOutcome()) drawMark(f);
  if (game && !finished && game->paced()) {
    // A round on its own clock: no full refresh in the middle of it.
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else if (game && game->drawsGray()) {
    ui::displayWithAntiAliasing(
        renderer, pagesUntilFullRefresh,
        [](void* ctx) {
          auto* self = static_cast<GameActivity*>(ctx);
          self->game->drawGray(self->renderer, ui::frame(self->renderer));
        },
        this);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
}
