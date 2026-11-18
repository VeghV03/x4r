#include "StreamActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <string>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "learn/FeedHighlights.h"
#include "learn/LearnPaths.h"
#include "learn/Puzzle.h"
#include "play/ui/PlayActivity.h"
#include "PaperActivity.h"
#include "ThreadActivity.h"
#include "activities/library/LibraryListActivity.h"
#include "usb/UsbHooks.h"
#include "util/HeapLows.h"

using x4::Layout;
using x4::Shape;
namespace ui = x4::ui;
namespace learn = x4::learn;

namespace {

constexpr int RULE_WIDTH = 40;
constexpr int OPTION_INDENT = 26;
constexpr int OPTION_PAD = 8;

constexpr int QUOTE_INDENT = 18;

int readerFont() { return SETTINGS.getReaderFontId(); }

// The step screen on screen, for the `screen` command (main task only, like the activity).
const StreamActivity* onScreen = nullptr;

}  // namespace

// ---- lifecycle ---------------------------------------------------------------

void StreamActivity::onEnter() {
  Activity::onEnter();
  // The reader font may come from the SD card; load it as the reader does.
  sdFontSystem.ensureLoaded(renderer);
  // The first screen gets a HALF refresh to clear what was there before.
  pagesUntilFullRefresh = 1;
  buffer = makeUniqueNoThrow<x4::LineBuffer>();
  if (!buffer) {
    LOG_ERR("LEARN", "OOM: stream buffer (%u bytes)", static_cast<unsigned>(sizeof(x4::LineBuffer)));
    phase = Phase::Empty;
  } else {
    loadStep();
    if (outsideMode() == Outside::Bite && !leaving) learn::noteBite();
  }
  onScreen = this;
  x4::heap::mark("set");
  requestUpdate();
}

void StreamActivity::onExit() {
  Activity::onExit();
  if (onScreen == this) onScreen = nullptr;
  actions.dismiss();
  words.reset();
  buffer.reset();
}

bool StreamActivity::isColumn() const {
  const Layout shown = layout();
  return shown != Layout::Text && shown != Layout::Quote;
}

// Rows a key can select: options, the rows to put in order.
bool StreamActivity::hasRows() const {
  const Layout shown = layout();
  return shown == Layout::Options || shown == Layout::Order;
}

// The next step. Questions over: back to the reader.
void StreamActivity::loadStep() {
  const bool shows = load(buffer.get());
  if (piece()) {
    // A running story shows where it stands first, once for each version of that text.
    if (!pieceOf.after && pieceThread()[0]) {
      auto thread = makeUniqueNoThrow<x4::threads::Thread>();
      if (thread && x4::threads::load(pieceThread(), *thread)) {
        x4::threads::markShown(pieceThread(), *thread);
        leaving = true;
        activityManager.replaceActivity(
            std::make_unique<ThreadActivity>(renderer, mappedInput, std::move(thread), pieceOf));
        return;
      }
    }
    if (shows) {
      stepLoaded();
    } else if (pieceOf.after) {
      leavePiece();
    } else {
      openPiece();
    }
    return;
  }
  if (!shows) {
    leaveSteps();
    return;
  }
  stepLoaded();
}

// The bite and the questions go on to the reader under them; the book puzzles return to Play.
void StreamActivity::leaveSteps() {
  leaving = true;
  if (outsideMode() == Outside::Puzzles) {
    activityManager.replaceActivity(std::make_unique<PlayActivity>(renderer, mappedInput, PUZZLES_ROW));
    return;
  }
  finish();
}

// The piece's article, so that the reader's Back comes back to what follows it.
void StreamActivity::openPiece() {
  leaving = true;
  SectionActivity::opening(pieceOf.id.c_str(), pieceOf.book.c_str(), pieceOf.fromKept);
  activityManager.goToReader(std::string("/") + pieceOf.book);
}

// Back to where the piece was opened: its section, or the Library's Kept.
void StreamActivity::leavePiece() {
  leaving = true;
  if (pieceOf.fromKept) {
    if (auto library = makeUniqueNoThrow<LibraryListActivity>(renderer, mappedInput, std::string("/") + pieceOf.book)) {
      activityManager.replaceActivity(std::move(library));
      return;
    }
  }
  activityManager.replaceActivity(std::make_unique<SectionActivity>(renderer, mappedInput));
}

// Another step is in the buffer: laid out afresh, with its highlights.
void StreamActivity::stepLoaded() {
  page = 0;
  paged = false;
  anchor = Anchor::First;
  if (phase == Phase::Empty) return;
  loadMarks();
}

void StreamActivity::describeScreen(JsonObject out) {
  if (onScreen && !onScreen->leaving) onScreen->describe(out["step"].to<JsonObject>());
}

// usb/UsbHooks.h
void x4ScreenFields(JsonObject out) { StreamActivity::describeScreen(out); }

void StreamActivity::relayout(const Anchor next) {
  paged = false;
  anchor = next;
}

x4::StreamView StreamActivity::view() const {
  x4::StreamView v;
  v.phase = phase;
  v.shape = item.shape;
  v.morePages = paged && page + 1 < pages.count;
  v.earlierPages = page > 0;
  if (hasRows()) {
    v.selectionVisible = !paged || optionPage[selected] == page;
    v.firstOption = selected == 0;
    v.lastOption = selected + 1 >= item.optionCount;
  }
  v.holding = holding;
  return v;
}

// ---- highlighting ------------------------------------------------------------

bool StreamActivity::canHighlight() const {
  return buffer && (phase == Phase::Prompt || phase == Phase::Revealed || phase == Phase::Answered) &&
         x4::feed::fieldsFor(item, phase).count > 0;
}

// A stream item by its id; a card as "<deck>/<card id>", like its card key.
void StreamActivity::stepKey(char (&out)[2 * x4::ID_MAX + 2]) const {
  if (isCard()) {
    snprintf(out, sizeof(out), "%s/%s", step.entry.deck, item.id);
  } else {
    snprintf(out, sizeof(out), "%s", item.id);
  }
}

void StreamActivity::loadMarks() {
  char key[2 * x4::ID_MAX + 2];
  stepKey(key);
  markCount = x4::feed::loadMarks(key, marks, x4::feed::MARKS_MAX);
}

void StreamActivity::startHighlight() {
  fields = x4::feed::fieldsFor(item, phase);
  const size_t count = x4::feed::splitWords(buffer->text, fields, nullptr, 0);
  if (count == 0 || count > UINT16_MAX) return;
  // A word costs 6 bytes: a 4000-byte bite is at most about 12 KB, usually 4.
  words = makeUniqueNoThrow<x4::feed::Word[]>(count);
  if (!words) {
    LOG_ERR("LEARN", "OOM: highlight words (%u)", static_cast<unsigned>(count));
    return;
  }
  wordCount = static_cast<uint16_t>(x4::feed::splitWords(buffer->text, fields, words.get(), count));
  selecting = true;
  selectionPending = true;
  selectionInput = WordSelectionInput{};
  // Laid out again on the screen shown, so every word learns its screen.
  relayout(Anchor::Keep);
}

void StreamActivity::stopHighlight() {
  selecting = false;
  selectionPending = false;
  words.reset();
  wordCount = 0;
}

x4::feed::Run StreamActivity::run() const { return {buffer->text, &fields, words.get(), wordCount}; }

// After a layout: the selection starts at the first sentence of the screen shown.
void StreamActivity::settleSelection() {
  if (!selecting || !selectionPending || !paged) return;
  selectionPending = false;
  selection = x4::feed::firstSentence(run(), page);
  const uint8_t at = words[selection.focus].screen;
  if (at != x4::feed::SCREEN_UNKNOWN) page = at;
  setShades();
}

void StreamActivity::setShades() {
  shadeCount = 0;
  if (selecting) {
    if (selectionPending) return;
    const auto r = run();
    shades[shadeCount++] = {x4::feed::rangeStart(r, selection.first), x4::feed::rangeEnd(r, selection.last)};
    return;
  }
  for (uint8_t i = 0; i < markCount; ++i) shades[shadeCount++] = {marks[i].start, marks[i].end};
}

ui::Shading StreamActivity::shadingFor(const x4::Slice slice, const bool gray) const {
  ui::Shading s;
  const int field = x4::feed::fieldOf(fields, slice);
  if (gray || field < 0) return s;
  s.shades = shades;
  s.count = shadeCount;
  s.base = fields.base[field];
  return s;
}

// While laying out: every word of `slice` landed on `screen`.
void StreamActivity::noteScreen(const x4::Slice slice, const uint8_t screen) {
  const int field = x4::feed::fieldOf(fields, slice);
  if (!words || field < 0) return;
  const uint16_t start = fields.base[field];
  const uint16_t end = static_cast<uint16_t>(start + slice.length);
  for (uint16_t i = 0; i < wordCount; ++i) {
    if (words[i].start >= start && words[i].start < end) words[i].screen = screen;
  }
}

namespace {

struct LineScreens {
  x4::feed::Word* words;
  uint16_t count;
  uint16_t base;
  const ui::Pages& pages;
};

void lineScreens(void* ctx, const char*, const int index, const ui::Span* spans, const int count) {
  auto& l = *static_cast<LineScreens*>(ctx);
  uint8_t screen = 0;
  while (screen + 1 < l.pages.count && l.pages.start[screen + 1] <= index) ++screen;
  for (int i = 0; i < count; ++i) {
    const int w = x4::feed::findWord(l.words, l.count, static_cast<uint16_t>(l.base + spans[i].source));
    if (w >= 0) l.words[w].screen = screen;
  }
}

}  // namespace

// While laying out a text paged by lines (`pages` holds line indices).
void StreamActivity::noteLineScreens(const ui::TextBox& box, const x4::Slice slice) {
  const int field = x4::feed::fieldOf(fields, slice);
  if (!words || field < 0) return;
  LineScreens l{words.get(), wordCount, fields.base[field], pages};
  ui::flowSpans(renderer, box.fontId, box.style, x4::text(buffer->text, slice), box.width, &lineScreens, &l);
}

void StreamActivity::showActions() {
  const auto r = run();
  const uint16_t start = x4::feed::rangeStart(r, selection.first);
  const uint16_t end = x4::feed::rangeEnd(r, selection.last);
  const char* const options[] = {tr(STR_HIGHLIGHT), tr(STR_QUESTION), tr(STR_DELETE)};
  actions.show("", options, x4::feed::overlaps(marks, markCount, start, end) ? 3 : 2, 0,
               [this](const int choice) { keep(choice); });
}

// Highlight (0), Question (1) or Delete (2) the selection, then leave it; the shading confirms.
void StreamActivity::keep(const int action) {
  const auto r = run();
  const uint16_t start = x4::feed::rangeStart(r, selection.first);
  const uint16_t end = x4::feed::rangeEnd(r, selection.last);
  char key[2 * x4::ID_MAX + 2];
  stepKey(key);
  if (action == 2) {
    x4::feed::removeOverlapping(key, start, end);
  } else {
    bool known = false;
    for (uint8_t i = 0; i < markCount; ++i) known = known || (marks[i].start == start && marks[i].end == end);
    if (!known) {
      const char* arena = buffer->text;
      char book[256] = {};
      x4::feed::Source source;
      source.key = key;
      source.kind = x4::kindOf(item.shape).name;
      if (item.shape == Shape::Highlight) {
        // A resurfaced highlight: its title is its book's.
        source.bookTitle = x4::text(arena, item.title);
        source.title = source.bookTitle;
      } else {
        source.title = item.title.length ? x4::text(arena, item.title) : "";
      }
      if (item.book.length) {
        source.book = x4::text(arena, item.book);
      } else if (isCard() && x4::feed::deckBook(step.entry.deck, book, sizeof(book))) {
        source.book = book;
      }
      const std::string text = x4::feed::selectedText(r, selection.first, selection.last);
      const auto kind = action == 1 ? x4::HighlightKind::Question : x4::HighlightKind::Highlight;
      x4::feed::Mark mark;
      if (!x4::feed::save(source, start, end, text.c_str(), kind, mark)) LOG_ERR("LEARN", "Failed to save highlight");
    }
  }
  loadMarks();
  stopHighlight();
}

void StreamActivity::loopHighlight() {
  using Input = WordSelectionInput;
  const uint8_t buttons = selectionInput.pollSentenceButtons(mappedInput, millis(), Input::PARAGRAPH_REPEAT_MS);
  if (!buttons) return;
  RenderLock lock(*this);
  const auto update = [this] { requestUpdate(); };
  // Sentence steps poll no navigation edges; every direction moves in the popup.
  if (actions.handleButtons(buttons & (Input::INPUT_UP | Input::INPUT_LEFT | Input::INPUT_WORD_LEFT),
                            buttons & (Input::INPUT_DOWN | Input::INPUT_RIGHT | Input::INPUT_WORD_RIGHT),
                            buttons & Input::INPUT_CONFIRM, buttons & Input::INPUT_BACK, update)) {
    return;
  }
  if (buttons & Input::INPUT_BACK) {
    stopHighlight();
    lock.unlock();
    requestUpdate();
    return;
  }
  if (selectionPending) return;  // not laid out yet
  if (buttons & Input::INPUT_CONFIRM) {
    showActions();
    lock.unlock();
    requestUpdate();
    return;
  }
  // As in a book: Up/Down move by sentence; Right grows a sentence (held, a
  // paragraph), Left shrinks one (held, a word). The screen follows the end.
  const auto r = run();
  const auto steps = r.steps();
  sentences::Range range = selection;
  bool moved = false;
  if (buttons & Input::INPUT_DOWN) {
    moved = steps.next(range);
  } else if (buttons & Input::INPUT_UP) {
    moved = steps.previous(range);
  } else if (buttons & Input::INPUT_RIGHT) {
    moved = steps.grow(range);
  } else if (buttons & Input::INPUT_LEFT) {
    moved = steps.shrink(range);
  } else if (buttons & Input::INPUT_WORD_RIGHT) {
    moved = steps.growParagraph(range);
  } else if (buttons & Input::INPUT_WORD_LEFT) {
    moved = steps.shrinkWord(range);
  }
  if (!moved) return;
  selection = range;
  const uint8_t at = words[selection.focus].screen;
  if (at != x4::feed::SCREEN_UNKNOWN) page = at;
  lock.unlock();
  requestUpdate();
}

// ---- input -------------------------------------------------------------------

void StreamActivity::loop() {
  using Button = MappedInputManager::Button;
  // On its way to another screen (a piece's article, its section): a press belongs to that one.
  if (leaving) return;
  if (selecting) {
    loopHighlight();
    return;
  }
  if (mappedInput.wasReleased(Button::Back)) {
    apply(x4::StreamMove::Back, {});
    if (piece()) {
      leavePiece();
    } else {
      leaveSteps();
    }
    return;
  }
  if (phase == Phase::Empty) return;
  // Holding Left highlights; its release is then not a Left.
  if (canHighlight() && mappedInput.wasLongPressed(Button::Left, ReaderUtils::BOOKMARK_HOLD_MS)) {
    RenderLock lock(*this);
    startHighlight();
    lock.unlock();
    requestUpdate();
    return;
  }

  // One event per key (see StreamKeys): a key never acts on both press and release.
  x4::StreamKeys keys;
  navigator.onNextPress([&keys] { keys.next = true; });
  navigator.onPreviousPress([&keys] { keys.previous = true; });
  if (mappedInput.wasPressed(Button::Left)) keys.previous = false;  // front Left acts on release
  const bool leftTap =
      mappedInput.wasReleased(Button::Left) && mappedInput.getHeldTime() < ReaderUtils::BOOKMARK_HOLD_MS;
  keys.previous = keys.previous || leftTap;
  keys.frontNext = keys.next && mappedInput.wasPressed(Button::Right);
  keys.frontPrevious = leftTap;
  if (mappedInput.wasPressed(Button::Confirm)) {
    keys.confirm = true;
    // Its release reaches no screen: not the next step's, the reader, a section.
    if (mappedInput.isPressed(Button::Confirm)) mappedInput.suppressNextRelease(Button::Confirm);
  }
  if (!keys.confirm && !keys.next && !keys.previous) return;

  // State read by render(): change it under the render lock.
  RenderLock lock(*this);
  using Move = x4::StreamMove;
  const Move move = x4::streamMove(view(), keys);
  if (move == Move::None) return;
  // The step decides what the move means; the screen follows.
  switch (apply(move, keys)) {
    case After::Stay:
      if (move == Move::PageNext) ++page;
      if (move == Move::PagePrevious) --page;
      if ((move == Move::SelectNext || move == Move::SelectPrevious) && paged) page = optionPage[selected];
      break;
    case After::Reveal:
      relayout(Anchor::Reveal);
      break;
    case After::Chosen:
      relayout(Anchor::Chosen);
      break;
    case After::Selected:
      relayout(Anchor::Selected);
      break;
    case After::Step:
      stepLoaded();
      break;
    case After::Open:
      lock.unlock();
      openPiece();
      return;
    case After::Leave:
      lock.unlock();
      if (piece()) {
        leavePiece();
      } else {
        leaveSteps();
      }
      return;
  }
  lock.unlock();
  requestUpdate();
}

// ---- drawing -----------------------------------------------------------------

namespace {

const char* str(const char* arena, const x4::Slice slice) { return slice.length ? x4::text(arena, slice) : ""; }

}  // namespace

void StreamActivity::render(RenderLock&&) {
  if (leaving) return;
  // Scan first so an SD reader font loads its glyphs in one batch.
  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    drawScreen(true);
    scope.endScanAndPrewarm();
  }
  drawScreen(false);
  // Highlight, Question, Delete: drawn over the screen, which stays as it is under it.
  if (actions.processRender(renderer, mappedInput)) return;
  ui::displayWithAntiAliasing(renderer, pagesUntilFullRefresh, &StreamActivity::drawGrayPass, this);
}

void StreamActivity::drawGrayPass(void* self) { static_cast<StreamActivity*>(self)->drawScreen(true); }

void StreamActivity::drawScreen(const bool gray) {
  const ui::Frame f = ui::frame(renderer);
  if (!gray) renderer.clearScreen();
  fields = phase == Phase::Empty ? x4::feed::Fields{} : x4::feed::fieldsFor(item, phase);
  setShades();
  if (phase == Phase::Empty) {
    drawEmpty(f, gray);
  } else if (isColumn()) {
    drawCard(f, gray);
  } else {
    drawItem(f, gray);
  }
  if (gray) return;
  if (phase != Phase::Empty && !piece()) ui::drawProgressDots(renderer, progress.done, progress.total, f.dotsY);
}

// The scan pass records where each screen starts (column offsets in `pages`);
// the draw pass draws the blocks of the shown screen. A block that does not fit
// below the current screen's start begins the next screen.
struct StreamActivity::Column {
  ui::Pages& pages;
  bool scan;
  int top;     // screen y of a screen's first block
  int height;  // room for blocks on one screen
  int from = 0;
  int to = 0;  // column range of the shown screen
  int y = 0;   // column y of the next block

  // Reserves `h` pixels; returns the screen y to draw at, or -1 off screen.
  int place(const int h) {
    const int at = y;
    y += h;
    if (scan) {
      const int start = pages.start[pages.count - 1];
      if (at + h > start + height && at > start && pages.count < ui::Pages::MAX) {
        pages.start[pages.count++] = static_cast<uint16_t>(at);
      }
      return -1;
    }
    return at >= from && at < to ? top + at - from : -1;
  }

  // Space between blocks; dropped where a screen breaks.
  void gap(const int h) {
    const int start = pages.start[pages.count - 1];
    if (scan && y + h > start + height && y > start && pages.count < ui::Pages::MAX) {
      y += h;
      pages.start[pages.count++] = static_cast<uint16_t>(y);
      return;
    }
    y += h;
  }

  // While scanning, the screen the last block landed on.
  uint8_t screen() const { return static_cast<uint8_t>(pages.count - 1); }
};

namespace {

struct ColumnText {
  const GfxRenderer& renderer;
  StreamActivity::Column& column;
  int fontId;
  EpdFontFamily::Style style;
  int x;
  int lineHeight;
  bool black;
  ui::Shading shading;
  // While selecting: the words, so the scan notes the screen of each.
  x4::feed::Word* words = nullptr;
  uint16_t wordCount = 0;
  int firstY = -1;
  uint8_t firstScreen = 0;
  bool started = false;
};

void columnLine(void* ctx, const char* line, int, const ui::Span* spans, const int count) {
  auto& t = *static_cast<ColumnText*>(ctx);
  if (*line == '\0') {
    if (t.started) t.column.gap(t.lineHeight);
    return;
  }
  const int y = t.column.place(t.lineHeight);
  if (!t.started) {
    t.started = true;
    t.firstY = y;
    t.firstScreen = t.column.screen();
  }
  if (t.column.scan && t.words) {
    for (int i = 0; i < count; ++i) {
      const int w =
          x4::feed::findWord(t.words, t.wordCount, static_cast<uint16_t>(t.shading.base + spans[i].source));
      if (w >= 0) t.words[w].screen = t.column.screen();
    }
  }
  if (y < 0) return;
  if (t.shading.count) ui::drawShading(t.renderer, t.x, y, t.lineHeight, spans, count, t.shading);
  t.renderer.drawText(t.fontId, t.x, y, line, t.black, t.style);
}

}  // namespace

int StreamActivity::layText(Column& column, const int x, const int width, const EpdFontFamily::Style style,
                            const char* text, const bool black, uint8_t* firstPage, const x4::Slice source) {
  const int font = readerFont();
  ColumnText t{renderer, column, font, style, x, renderer.getLineHeight(font, SETTINGS.getReaderLineCompression()),
               black};
  const int field = x4::feed::fieldOf(fields, source);
  if (field >= 0) {
    t.shading = shadingFor(source, !black || renderer.getRenderMode() != GfxRenderer::BW);
    t.shading.base = fields.base[field];
    t.words = words.get();
    t.wordCount = wordCount;
  }
  ui::flowSpans(renderer, font, style, text, width, &columnLine, &t);
  if (firstPage && t.started) *firstPage = t.firstScreen;
  return t.firstY;
}

void StreamActivity::drawCard(const ui::Frame& f, const bool gray) {
  const int height = f.bottom - f.top;
  if (!paged && !renderer.isFontCacheScanning()) {
    // Laid out once per screen state, with real measurements (not while the
    // font cache only records text).
    pages = ui::Pages{};
    pages.count = 1;
    Column scan{pages, true, f.top, height};
    layCard(scan, f, gray);
    pages.start[pages.count] = static_cast<uint16_t>(scan.y);
    paged = true;
    page = anchor == Anchor::Reveal     ? revealPage
           : anchor == Anchor::Chosen   ? optionPage[chosen >= 0 ? chosen : 0]
           : anchor == Anchor::Selected ? optionPage[selected]
           : anchor == Anchor::Keep     ? std::min<int>(page, pages.count - 1)
                                        : 0;
    settleSelection();
  }
  const int shown = paged ? std::min<int>(page, pages.count - 1) : 0;
  Column column{pages, false, f.top, height};
  column.from = paged ? pages.start[shown] : 0;
  column.to = !paged ? height : shown + 1 < pages.count ? pages.start[shown + 1] : 1 << 30;
  layCard(column, f, gray);
}

void StreamActivity::layCard(Column& c, const ui::Frame& f, const bool gray) {
  const char* arena = buffer->text;
  const int font = readerFont();
  const int lh = renderer.getLineHeight(font, SETTINGS.getReaderLineCompression());
  const bool answered = phase == Phase::Answered;
  const bool revealed = phase == Phase::Revealed;

  // The prompt; a cloze shows its filled-in text once answered.
  const char* prompt = item.shape == Shape::Cloze && answered ? str(arena, item.body) : str(arena, item.title);
  layText(c, f.left, f.width, EpdFontFamily::REGULAR, prompt);

  switch (layout()) {
    case Layout::Answer:
      if (revealed) {
        c.gap(lh / 2);
        const int ruleY = c.place(lh / 2 + lh / 3);
        if (c.scan) revealPage = c.screen();
        if (!gray && ruleY >= 0) renderer.fillRect(f.left, ruleY, RULE_WIDTH, 2, true);
        layText(c, f.left, f.width, EpdFontFamily::REGULAR, str(arena, item.body), true, nullptr, item.body);
      }
      break;
    case Layout::Points:
      if (revealed) {
        c.gap(lh);
        for (uint8_t i = 0; i < item.optionCount; ++i) {
          uint8_t first = 0;
          const int y = layText(c, f.left + OPTION_INDENT, f.width - OPTION_INDENT, EpdFontFamily::REGULAR,
                                str(arena, item.options[i]), true, &first, item.options[i]);
          if (c.scan && i == 0) revealPage = first;
          if (!gray && y >= 0) renderer.fillRect(f.left + 2, y + lh * 5 / 8 - 3, 7, 7, true);  // centred on the x-height
          c.gap(lh / 3);
        }
      }
      break;
    case Layout::Order:
      layRows(c, f, gray);
      break;
    case Layout::Options:
      c.gap(lh);
      for (uint8_t i = 0; i < item.optionCount; ++i) {
        const char* option = str(arena, item.options[i]);
        // The answer turns bold once checked; measured bold, it keeps its last line.
        const auto style = answered && i == item.answer ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
        const int lines = std::max(1, ui::flowText(renderer, font, style, option,
                                                   f.width - OPTION_INDENT - OPTION_PAD, nullptr, nullptr));
        const int height = lines * lh + 2 * OPTION_PAD;
        const int y = c.place(height);
        if (c.scan) optionPage[i] = c.screen();
        c.gap(4);
        if (y < 0) continue;
        ui::TextBox box{font, style, f.left + OPTION_INDENT, y + OPTION_PAD,
                        f.width - OPTION_INDENT - OPTION_PAD, lh, y + height};
        if (!gray && !answered && i == selected) drawSelected(f, y, height);
        ui::drawTextBox(renderer, box, option);
        if (!gray && answered) {
          const int mark = std::min(OPTION_INDENT - 6, lh * 3 / 4);
          const int markY = y + OPTION_PAD + (lh - mark) / 2;
          if (i == item.answer) {
            ui::drawCheck(renderer, f.left, markY, mark);
          } else if (i == chosen) {
            ui::drawCross(renderer, f.left, markY, mark);  // the wrong pick
          }
        }
      }
      break;
    default:
      break;
  }

  const bool checked = layout() == Layout::Options || layout() == Layout::Order;
  if ((checked ? answered : revealed) && item.why.length) {
    c.gap(lh / 2);
    // Kept whole: a why that would break across screens starts the next one.
    const char* why = str(arena, item.why);
    const int lines = ui::flowText(renderer, font, EpdFontFamily::ITALIC, why, f.width, nullptr, nullptr);
    if (lines * lh > c.height) {
      layText(c, f.left, f.width, EpdFontFamily::ITALIC, why, true, nullptr, item.why);
    } else {
      const int y = c.place(lines * lh);
      if (c.scan) noteScreen(item.why, c.screen());
      if (y >= 0) {
        ui::TextBox box{font, EpdFontFamily::ITALIC, f.left, y, f.width, lh, y + lines * lh};
        const ui::Shading shading = shadingFor(item.why, gray);
        ui::drawTextBox(renderer, box, why, 0, 1 << 30, nullptr, &shading);
      }
    }
  }
}

// The selected option: the bar Today and the Library use, beside its lines.
void StreamActivity::drawSelected(const ui::Frame& f, const int y, const int height) const {
  ui::drawRowSelection(renderer, f, y + OPTION_PAD - ui::ROW_PAD / 2, y + height - OPTION_PAD + ui::ROW_PAD / 2);
}

// Order rows. The selected row has the bar; a picked-up row also shows a grip. Once checked,
// the solved order, each row marked by whether the reader had it in place.
void StreamActivity::layRows(Column& c, const ui::Frame& f, const bool gray) {
  const char* arena = buffer->text;
  const int font = readerFont();
  const int lh = renderer.getLineHeight(font, SETTINGS.getReaderLineCompression());
  const bool answered = phase == Phase::Answered;
  c.gap(lh);
  for (uint8_t i = 0; i < item.optionCount; ++i) {
    const uint8_t shown = answered ? i : rows[i];
    const char* row = str(arena, item.options[shown]);
    const int lines = std::max(1, ui::flowText(renderer, font, EpdFontFamily::REGULAR, row,
                                               f.width - OPTION_INDENT - OPTION_PAD, nullptr, nullptr));
    const int height = lines * lh + 2 * OPTION_PAD;
    const int y = c.place(height);
    if (c.scan) {
      optionPage[i] = c.screen();
      noteScreen(item.options[shown], c.screen());
    }
    c.gap(4);
    if (y < 0) continue;
    ui::TextBox box{font, EpdFontFamily::REGULAR, f.left + OPTION_INDENT, y + OPTION_PAD,
                    f.width - OPTION_INDENT - OPTION_PAD, lh, y + height};
    if (!gray && !answered && i == selected && !selecting) {
      drawSelected(f, y, height);
      if (holding) {
        const int gripY = y + OPTION_PAD + lh / 2 - 5;
        for (int bar = 0; bar < 3; ++bar) {
          renderer.fillRect(f.left + OPTION_PAD, gripY + bar * 4, OPTION_INDENT - 2 * OPTION_PAD, 2, true);
        }
      }
    }
    const ui::Shading shading = shadingFor(item.options[shown], gray);
    ui::drawTextBox(renderer, box, row, 0, 1 << 30, nullptr, &shading);
    if (!gray && answered) {
      const int mark = std::min(OPTION_INDENT - 6, lh * 3 / 4);
      const int markY = y + OPTION_PAD + (lh - mark) / 2;
      if (rows[i] == i) {
        ui::drawCheck(renderer, f.left, markY, mark);
      } else {
        ui::drawCross(renderer, f.left, markY, mark);
      }
    }
  }
}

void StreamActivity::drawItem(const ui::Frame& f, const bool gray) {
  const char* arena = buffer->text;
  const int font = readerFont();
  const int lh = renderer.getLineHeight(font, SETTINGS.getReaderLineCompression());
  ui::TextBox box{font, EpdFontFamily::REGULAR, f.left, f.top, f.width, lh, f.bottom};

  // Pages are laid out once per item, with real measurements (not while the
  // font cache only records text).
  const bool laying = !paged && !renderer.isFontCacheScanning();

  if (layout() == Layout::Quote) {
    // The highlight behind a quote bar, its book under it.
    box.x = f.left + QUOTE_INDENT;
    box.width = f.width - QUOTE_INDENT;
    box.bottom = f.bottom - (item.title.length ? 2 * lh : 0);
    if (laying) {
      ui::paginate(renderer, box, f.top, f.top, str(arena, item.body), pages);
      paged = true;
      noteLineScreens(box, item.body);
      settleSelection();
    }
    const int pageIndex = paged ? std::min<int>(page, pages.count - 1) : 0;
    const int end = paged ? pages.start[pageIndex + 1] : 1 << 30;
    const ui::Shading shading = shadingFor(item.body, gray);
    const int y = ui::drawTextBox(renderer, box, str(arena, item.body), paged ? pages.start[pageIndex] : 0, end,
                                  nullptr, &shading);
    if (!gray && y > f.top) renderer.fillRect(f.left, f.top + lh / 6, 3, y - f.top - lh / 3, true);
    if ((!paged || pageIndex + 1 == pages.count) && item.title.length) {
      box.y = y + lh;
      box.bottom = f.bottom;
      box.style = EpdFontFamily::ITALIC;
      ui::drawTextBox(renderer, box, str(arena, item.title));
    }
    return;
  }

  // A bite: a title, then the body over as many screens as it needs.
  // Later pages drop the title.
  box.style = EpdFontFamily::BOLD;
  const char* title = str(arena, item.title);
  const int titleLines = ui::flowText(renderer, font, EpdFontFamily::BOLD, title, f.width, nullptr, nullptr);
  const int bodyTop = f.top + (titleLines ? titleLines * lh + lh / 2 : 0);
  if (laying) {
    box.style = EpdFontFamily::REGULAR;
    ui::paginate(renderer, box, bodyTop, f.top, str(arena, item.body), pages);
    paged = true;
    noteLineScreens(box, item.body);
    settleSelection();
  }
  const int pageIndex = paged ? std::min<int>(page, pages.count - 1) : 0;
  if (pageIndex == 0) {
    box.style = EpdFontFamily::BOLD;
    ui::drawTextBox(renderer, box, title);
  }
  box.style = EpdFontFamily::REGULAR;
  box.y = pageIndex == 0 ? bodyTop : f.top;
  const ui::Shading shading = shadingFor(item.body, gray);
  ui::drawTextBox(renderer, box, str(arena, item.body), paged ? pages.start[pageIndex] : 0,
                  paged ? pages.start[pageIndex + 1] : 1 << 30, nullptr, &shading);
}

// Nothing to show (out of memory): a check, never a blank page.
void StreamActivity::drawEmpty(const ui::Frame& f, const bool gray) {
  if (gray) return;
  ui::drawOutcome(renderer, f, true);
}
