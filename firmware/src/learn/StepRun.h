#pragma once

#include <ArduinoJson.h>

#include <string>

#include "Learn.h"
#include "StreamInput.h"

// The step on screen and what a move does to it: all of a step that is not drawing. The step
// screen is one of these plus its layout; the host plays it through the same code (x4play).
namespace x4::learn {

class StepRun {
 public:
  // What the screen does after a move.
  enum class After : uint8_t {
    Stay,      // the same layout
    Reveal,    // laid out again, opening on the answer
    Chosen,    // laid out again, opening on the chosen option
    Selected,  // laid out again, opening on the selected row
    Step,      // another step is loaded
    Open,      // the piece's article opens
    Leave,     // nothing more: back where the steps were opened
  };

  Item item;
  Step step;
  Progress progress;
  StreamPhase phase = StreamPhase::Empty;
  uint8_t selected = 0;
  int8_t chosen = -1;
  // Order: rows[i] is the solved index shown at place i; `holding` while the
  // selected row is picked up.
  uint8_t rows[LIST_MAX] = {};
  bool holding = false;
  // A piece that asks is shown in parts, built from `article`: its predict question (item.shape
  // is Predict), then once read the question again with the guess and the answer, and its link
  // question (Link) or its forecast. `guess` is how far it has got.
  Guess guess;

  // Questions (SPEC: Questions): the cards of this book.
  void askAbout(std::string bookPath) { questionsFor = std::move(bookPath); }
  bool questions() const { return !questionsFor.empty(); }
  // A piece of the paper (SPEC: A piece). `after` false: before its article, its question when
  // it has one not yet guessed. True: back from the article once it was read: the answer to the
  // guess, then one more thing, its link or else its forecast. load() is false when there is
  // nothing to show.
  void forPiece(std::string readId, bool after) {
    pieceId = std::move(readId);
    pieceAfter = after;
  }
  bool piece() const { return !pieceId.empty(); }
  // After load(): the thread the piece names (empty for none), and whether it carries a link
  // or a forecast, in which case no card follows it.
  const char* pieceThread() const { return thread; }
  bool pieceCarriesMore() const { return article.links[0].length > 0 || forecast.id[0] != 0; }
  // The day's bite (SPEC: Book) and nothing after it, whose Back goes on as Next does; or the
  // book puzzles (SPEC: Play), one after another until none is left.
  enum class Outside : uint8_t { None, Bite, Puzzles };
  void forOutside(Outside what) { outsideOf = what; }
  bool outside() const { return outsideOf != Outside::None; }
  Outside outsideMode() const { return outsideOf; }

  // Loads the next step into `buffer`. False when there is none.
  bool load(LineBuffer* buffer);
  After apply(StreamMove move, const StreamKeys& keys);
  // For the `screen` command: which step this is and where it stands.
  void describe(JsonObject out) const;

  bool isCard() const { return kindOf(item.shape).result == Result::Graded; }
  Layout layout() const { return kindOf(item.shape).layout; }

 private:
  LineBuffer* buffer = nullptr;
  std::string questionsFor;
  std::string pieceId;
  bool pieceAfter = false;
  Outside outsideOf = Outside::None;
  bool outsideShown = false;
  bool loadOutside();
  Item forecast;
  char thread[ID_MAX + 1] = {};
  bool loadPiece();
  bool showPiecePart();
  After finishPiece();
  uint8_t asked = 0;
  uint32_t shownAt = 0;

  uint32_t elapsedMs() const;
  bool loadQuestion();
  void grade(const Answer& answer);
  void solve(bool correct, int8_t pick);
  After next() { return load(buffer) ? After::Step : After::Leave; }
  Item article;
  bool answerShown = false;  // the predict's answer was read on, in this visit
  After pass();
  void pick();
};

}  // namespace x4::learn
