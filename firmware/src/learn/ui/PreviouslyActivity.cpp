#include "PreviouslyActivity.h"

#include <FontCacheManager.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "learn/ui/LearnUi.h"

PreviouslyActivity::PreviouslyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* recap)
    : Activity("Previously", renderer, mappedInput) {
  std::strncpy(text, recap, sizeof(text) - 1);
  text[sizeof(text) - 1] = '\0';
}

void PreviouslyActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void PreviouslyActivity::loop() {
  using Button = MappedInputManager::Button;
  if (leaving) return;
  bool read = mappedInput.wasReleased(Button::Confirm);
  navigator.onNextPress([&read] { read = true; });
  if (mappedInput.wasReleased(Button::Back)) {
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    setResult(std::move(cancelled));
  } else if (!read) {
    return;
  }
  leaving = true;
  finish();
}

void PreviouslyActivity::render(RenderLock&&) {
  if (leaving) return;
  // Scan first so an SD reader font loads its glyphs in one batch.
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    draw(true);
    scope.endScanAndPrewarm();
  }
  draw(false);
  x4::ui::displayWithAntiAliasing(renderer, pagesUntilFullRefresh, &PreviouslyActivity::drawGrayPass, this);
}

void PreviouslyActivity::drawGrayPass(void* self) { static_cast<PreviouslyActivity*>(self)->draw(true); }

void PreviouslyActivity::draw(const bool gray) {
  namespace ui = x4::ui;
  const ui::Frame f = ui::frame(renderer);
  if (!gray) renderer.clearScreen();
  const int font = SETTINGS.getReaderFontId();
  const int lh = renderer.getLineHeight(font, SETTINGS.getReaderLineCompression());
  ui::TextBox box{font, EpdFontFamily::BOLD, f.left, f.top, f.width, lh, f.bottom};
  box.y = ui::drawTextBox(renderer, box, tr(STR_PREVIOUSLY)) + lh / 2;
  box.style = EpdFontFamily::REGULAR;
  ui::drawTextBox(renderer, box, text);
}
