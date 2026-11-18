#include "ThreadActivity.h"

#include <FontCacheManager.h>

#include "CrossPointSettings.h"
#include "PaperActivity.h"
#include "StreamActivity.h"
#include "components/UITheme.h"
#include "learn/ui/LearnUi.h"

void ThreadActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void ThreadActivity::loop() {
  using Button = MappedInputManager::Button;
  if (leaving) return;
  bool read = mappedInput.wasReleased(Button::Confirm);
  navigator.onNextPress([&read] { read = true; });
  if (mappedInput.wasReleased(Button::Back)) {
    leaving = true;
    if (piece.fromKept) {
      activityManager.goToLibrary();
    } else {
      activityManager.replaceActivity(std::make_unique<SectionActivity>(renderer, mappedInput));
    }
  } else if (read) {
    // The thread is shown now, so the piece goes on to its question or its article.
    leaving = true;
    activityManager.replaceActivity(std::make_unique<StreamActivity>(renderer, mappedInput, std::move(piece)));
  }
}

void ThreadActivity::render(RenderLock&&) {
  if (leaving) return;
  // Scan first so an SD reader font loads its glyphs in one batch.
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    draw(true);
    scope.endScanAndPrewarm();
  }
  draw(false);
  x4::ui::displayWithAntiAliasing(renderer, pagesUntilFullRefresh, &ThreadActivity::drawGrayPass, this);
}

void ThreadActivity::drawGrayPass(void* self) { static_cast<ThreadActivity*>(self)->draw(true); }

// The story's name, then its text: laid out as a book's recap is.
void ThreadActivity::draw(const bool gray) {
  namespace ui = x4::ui;
  const ui::Frame f = ui::frame(renderer);
  if (!gray) renderer.clearScreen();
  const int font = SETTINGS.getReaderFontId();
  const int lh = renderer.getLineHeight(font, SETTINGS.getReaderLineCompression());
  ui::TextBox box{font, EpdFontFamily::BOLD, f.left, f.top, f.width, lh, f.bottom};
  box.y = ui::drawTextBox(renderer, box, thread->title) + lh / 2;
  box.style = EpdFontFamily::REGULAR;
  ui::drawTextBox(renderer, box, thread->body);
}
