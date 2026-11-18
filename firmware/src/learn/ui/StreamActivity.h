#pragma once

#include <memory>
#include <string>

#include "LearnUi.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "learn/FeedText.h"
#include "learn/StepRun.h"
#include "util/ButtonNavigator.h"
#include "util/WordSelectionInput.h"

// Steps one per screen: the questions after a chapter, a piece's question and what follows it,
// the bite and the book puzzles. Every answer is saved at once, so Back or sleep never loses
// it. What a step is and what a move does to it is StepRun; this is its screen.
class StreamActivity final : public Activity, private x4::learn::StepRun {
 public:
  // Questions (SPEC: Questions): the due cards of `bookPath`, over the reader that
  // just finished one of its chapters. Finishes back to the reader.
  StreamActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath)
      : Activity("Questions", renderer, mappedInput) {
    askAbout(std::move(bookPath));
  }
  // A piece of the paper (SPEC: A piece): before its article its question, back from it what
  // follows. With nothing to show it goes straight on: to the article, or back to where the
  // piece was opened (`fromKept`: the Library's Kept, else its section).
  struct Piece {
    std::string id;
    std::string book;  // book key
    bool after = false;
    bool fromKept = false;
  };
  StreamActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Piece of)
      : Activity("Questions", renderer, mappedInput), pieceOf(std::move(of)) {
    forPiece(pieceOf.id, pieceOf.after);
  }
  // The day's bite (SPEC: Book) over the reader before its page, which
  // finishes back to the reader; or the book puzzles from Play, back to Play when none is left.
  using Outside = x4::learn::StepRun::Outside;
  StreamActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Outside what)
      : Activity(what == Outside::Bite ? "Bite" : "Puzzles", renderer, mappedInput) {
    forOutside(what);
  }
  // The `screen` command: adds "step" while a step is on screen.
  static void describeScreen(JsonObject out);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // A card laid out as one tall column, shown a screen at a time.
  struct Column;

 private:
  using Phase = x4::StreamPhase;
  // Which screen a fresh layout opens on.
  enum class Anchor : uint8_t { First, Reveal, Chosen, Selected, Keep };

  // Strings of the current step (about 10 KB). Allocated in onEnter, freed in
  // onExit: a card's text never outlives its screen.
  std::unique_ptr<x4::LineBuffer> buffer;
  Piece pieceOf;
  void openPiece();
  void leavePiece();
  void leaveSteps();
  // On its way to another screen: nothing more to paint.
  bool leaving = false;
  uint8_t page = 0;
  // Screens of the current step, laid out on its first render. Cards record
  // column offsets in `pages`, items line indices.
  bool paged = false;
  x4::ui::Pages pages;
  Anchor anchor = Anchor::First;
  uint8_t revealPage = 0;
  uint8_t optionPage[x4::LIST_MAX] = {};
  int pagesUntilFullRefresh = 0;
  ButtonNavigator navigator;

  // Highlighting, as in a book (SPEC: Highlights). `fields` is the step's text
  // that can be highlighted; `marks` its saved highlights. While `selecting`,
  // `words` holds that text as words (6 bytes each, freed when the selection
  // ends) and `selection` the selected sentences.
  x4::feed::Fields fields;
  x4::feed::Mark marks[x4::feed::MARKS_MAX];
  uint8_t markCount = 0;
  std::unique_ptr<x4::feed::Word[]> words;
  uint16_t wordCount = 0;
  sentences::Range selection;
  bool selecting = false;
  bool selectionPending = false;  // the next layout picks where it starts
  WordSelectionInput selectionInput;
  OptionPopup actions;
  // What drawScreen shades: the selection while selecting, else the marks.
  x4::ui::Shade shades[x4::feed::MARKS_MAX];
  uint8_t shadeCount = 0;

  void loadStep();
  void stepLoaded();
  // Laid out as a column of blocks (cards, a piece's questions and the book puzzles).
  bool isColumn() const;
  bool hasRows() const;

  bool canHighlight() const;
  void stepKey(char (&out)[2 * x4::ID_MAX + 2]) const;
  void loadMarks();
  void startHighlight();
  void stopHighlight();
  void loopHighlight();
  void showActions();
  void keep(int action);
  x4::feed::Run run() const;
  void settleSelection();
  void setShades();
  x4::ui::Shading shadingFor(x4::Slice slice, bool gray) const;
  void noteScreen(x4::Slice slice, uint8_t screen);
  void noteLineScreens(const x4::ui::TextBox& box, x4::Slice slice);

  void drawScreen(bool gray);
  x4::StreamView view() const;
  void relayout(Anchor next);
  void drawCard(const x4::ui::Frame& f, bool gray);
  void layCard(Column& column, const x4::ui::Frame& f, bool gray);
  // `source`: the slice `text` is, when it can be highlighted.
  int layText(Column& column, int x, int width, EpdFontFamily::Style style, const char* text, bool black = true,
              uint8_t* firstPage = nullptr, x4::Slice source = {});
  void layRows(Column& column, const x4::ui::Frame& f, bool gray);
  void drawSelected(const x4::ui::Frame& f, int y, int height) const;
  void drawItem(const x4::ui::Frame& f, bool gray);
  void drawEmpty(const x4::ui::Frame& f, bool gray);
  static void drawGrayPass(void* self);
};
