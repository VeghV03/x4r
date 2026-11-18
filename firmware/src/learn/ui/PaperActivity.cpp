#include "PaperActivity.h"

#include <FontCacheManager.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "FootballActivity.h"
#include "LearnUi.h"
#include "SdCardFontSystem.h"
#include "StreamActivity.h"
#include "fontIds.h"
#include "learn/Visits.h"

namespace paper = x4::paper;

namespace {

constexpr uint8_t CLEAN_EVERY = 10;  // fast refreshes between clean ones, as on the front page
constexpr unsigned long KEEP_HOLD_MS = 600;
paper::Place current;
// The piece whose article is open in the reader, until the reader comes back from it.
char opened[x4::readlist::BOOK_BYTES + 1] = {};
char openedId[x4::ID_MAX + 1] = {};
bool openedFromKept = false;

}  // namespace

paper::Place& paper::place() { return current; }

paper::Faces paper::faces() {
  const int reader = SETTINGS.getReaderFontId();
  const int head = sdFontSystem.readerFontAt(reader, 12);
  const int title = sdFontSystem.readerFontAt(reader, 14);
  return Faces{UI_10_FONT_ID, SMALL_FONT_ID, head ? head : NOTOSANS_12_FONT_ID, title ? title : NOTOSANS_14_FONT_ID};
}

void SectionActivity::opening(const char* id, const char* bookKey, const bool fromKept) {
  snprintf(opened, sizeof(opened), "%s", bookKey);
  snprintf(openedId, sizeof(openedId), "%s", id);
  openedFromKept = fromKept;
}

bool SectionActivity::returnsTo(const std::string& bookPath, std::string& id, bool& fromKept) {
  const char* key = bookPath.c_str() + (bookPath.c_str()[0] == '/' ? 1 : 0);
  if (!opened[0] || std::strcmp(opened, key) != 0) return false;
  opened[0] = '\0';
  id = openedId;
  fromKept = openedFromKept;
  return true;
}

void SectionActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.ensureLoaded(renderer);
  faces = paper::faces();
  buffer = makeUniqueNoThrow<x4::LineBuffer>();
  front = makeUniqueNoThrow<paper::Front>();
  if (!buffer || !front || !paper::loadFront(*buffer, *front) || current.section >= front->count) {
    // No issue, or one that changed under us: the front page sorts it out.
    if (!buffer || !front) LOG_ERR("PAPER", "OOM: section");
    current = paper::Place{};
    activityManager.goToToday();
    return;
  }
  current.screen = paper::Screen::Section;
  load();
  x4::visits::enter(front->sections[current.section].id);
  requestUpdate();
}

void SectionActivity::onExit() {
  Activity::onExit();
  if (!continues) x4::visits::leave();
  buffer.reset();
  front.reset();
}

void SectionActivity::load() {
  const char* id = front->sections[current.section].id;
  count = paper::loadSection(*buffer, id, pieces, paper::PIECES_MAX);
  if (current.piece >= count) current.piece = 0;
  tabCount = 0;
  if (std::strcmp(id, paper::FOOTBALL) != 0) return;
  tabs[tabCount++] = tr(STR_ANALYSIS);
  tabs[tabCount++] = tr(STR_MATCHES);
  const uint8_t n = paper::loadCompetitions(competitions, paper::COMPETITIONS_MAX);
  for (uint8_t i = 0; i < n; ++i) tabs[tabCount++] = competitions[i];
}

void SectionActivity::act(const paper::Go go) {
  switch (go) {
    case paper::Go::Move:
      requestUpdate();
      return;
    case paper::Go::Section: {
      RenderLock lock(*this);
      load();
    }
      x4::visits::enter(front->sections[current.section].id);
      requestUpdate();
      return;
    case paper::Go::Front:
      activityManager.goToToday();
      return;
    case paper::Go::Piece: {
      // Through its question when it has one to ask, else straight to the article.
      const paper::Piece& piece = pieces[current.piece];
      activityManager.replaceActivity(std::make_unique<StreamActivity>(
          renderer, mappedInput, StreamActivity::Piece{piece.id, piece.book, false, false}));
      return;
    }
    case paper::Go::Football:
      continues = true;
      activityManager.replaceActivity(std::make_unique<FootballActivity>(renderer, mappedInput, current.fromRight));
      return;
    default:
      return;
  }
}

void SectionActivity::loop() {
  using Button = MappedInputManager::Button;
  using paper::Key;
  if (!front) return;
  if (x4::ui::anyPress(mappedInput)) x4::visits::press();
  // A hold of Confirm keeps the piece, or lets a kept one go; the mark is the answer. It fires
  // at the threshold, and the release that follows reaches no screen.
  if (mappedInput.wasLongPressed(Button::Confirm, KEEP_HOLD_MS)) {
    if (current.piece < count) {
      RenderLock lock(*this);
      bool kept = false;
      if (x4::paper::toggleKept(*buffer, pieces[current.piece], kept)) pieces[current.piece].kept = kept;
    }
    requestUpdate();
    return;
  }
  // Back and Left act on release (a hold of Back is Today; Left's hold belongs to highlights
  // elsewhere), the others on press, as in the lists.
  static constexpr struct {
    Button button;
    Key key;
    bool onRelease;
  } KEYS[] = {{Button::Back, Key::Back, true},  {Button::Confirm, Key::Confirm, true}, {Button::Left, Key::Left, false},
              {Button::Right, Key::Right, false}, {Button::Up, Key::Up, false},        {Button::Down, Key::Down, false}};
  for (const auto& k : KEYS) {
    if (k.onRelease ? mappedInput.wasReleased(k.button) : mappedInput.wasPressed(k.button)) {
      act(paper::press(current, *front, count, k.key));
      return;
    }
  }
}

void SectionActivity::render(RenderLock&&) {
  if (!front) return;
  paper::SectionView view;
  view.name = front->sections[current.section].name;
  view.pieces = pieces;
  view.count = count;
  view.selected = current.piece;
  view.tabs = tabs;
  view.tabCount = tabCount;
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    paper::drawSection(renderer, faces, view, false);
    scope.endScanAndPrewarm();
  }
  renderer.clearScreen();
  paper::drawSection(renderer, faces, view, false);
  const bool clean = firstRender || ++fastRefreshes >= CLEAN_EVERY;
  if (clean) fastRefreshes = 0;
  renderer.displayBuffer(clean ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  firstRender = false;
}
