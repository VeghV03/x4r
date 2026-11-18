#include "StepRun.h"

#include <algorithm>
#include <cstdio>

#include "Journal.h"
#include "Paper.h"
#include "Puzzle.h"

namespace x4::learn {

uint32_t StepRun::elapsedMs() const { return platformMillis() - shownAt; }

bool StepRun::load(LineBuffer* into) {
  buffer = into;
  answerShown = false;
  selected = 0;
  chosen = -1;
  holding = false;
  if (questions()) return loadQuestion();
  if (piece()) return loadPiece();
  if (outside()) return loadOutside();
  return false;
}

// The bite once, or the next book puzzle.
bool StepRun::loadOutside() {
  if (!buffer || (outsideOf == Outside::Bite && outsideShown)) return false;
  if (!loadUnseen(*buffer, outsideOf == Outside::Bite ? &isBite : &isBookPuzzle, item, step)) return false;
  outsideShown = true;
  phase = StreamPhase::Prompt;
  shownAt = platformMillis();
  if (item.shape == Shape::Order) orderShuffle(item.id, item.optionCount, rows);
  progress = Progress{};
  return true;
}

// The piece's line, from the issue or from where a kept piece keeps it, and the part to show.
bool StepRun::loadPiece() {
  phase = StreamPhase::Empty;
  size_t length = 0;
  if (!buffer || !paper::loadPieceLine(*buffer, pieceId.c_str(), length)) return false;
  if (!parsePiece(buffer->text, length, article, forecast, thread)) return false;
  if (article.optionCount == 0 && !pieceCarriesMore()) return false;
  guess = guessFor(article);
  if (!guess.piece) {
    // From now on it waits for its article, so that reading it is noted.
    guess = Guess{};
    guess.piece = true;
    saveGuess(article, text(buffer->text, article.book), guess);
  }
  return showPiecePart();
}

// Before the article: the question, until it is guessed. After it, once read: the guess with
// the answer, then the link, else the forecast while its day has not passed.
bool StepRun::showPiecePart() {
  const bool predicts = article.optionCount > 0;
  item = article;
  selected = 0;
  chosen = -1;
  phase = StreamPhase::Prompt;
  shownAt = platformMillis();
  if (!pieceAfter) {
    if (!predicts || guess.pick >= 0) return false;
    item.shape = Shape::Predict;
    item.title = article.body;
    return true;
  }
  if (guess.done || !guess.read) return false;
  if (predicts && guess.pick >= 0 && !answerShown) {
    item.shape = Shape::Predict;
    item.title = article.body;
    phase = StreamPhase::Answered;
    chosen = guess.pick;
    return true;
  }
  if (article.links[0].length > 0) {
    item.shape = Shape::Link;
    item.title = article.linkPrompt;
    item.why = article.linkWhy;
    item.answer = article.linkAnswer;
    item.optionCount = LINK_OPTIONS;
    for (uint8_t i = 0; i < LINK_OPTIONS; ++i) item.options[i] = article.links[i];
    if (guess.link >= 0) {
      phase = StreamPhase::Answered;
      chosen = guess.link;
    }
    return true;
  }
  if (forecast.id[0] && !guess.forecast) {
    CivilTime now{};
    const bool passed = localTime(now) && static_cast<uint32_t>(now.year) * 10000 + now.month * 100 + now.day >
                                              forecast.settles;
    if (passed) return false;
    item = forecast;
    selected = FORECAST_START;
    return true;
  }
  return false;
}

// What followed the piece was shown, or left: it asks nothing again.
StepRun::After StepRun::finishPiece() {
  guess.done = true;
  saveGuess(article, text(buffer->text, article.book), guess);
  return After::Leave;
}

// The next of at most QUESTIONS_PER_END cards.
bool StepRun::loadQuestion() {
  uint16_t left = 0;
  if (!buffer || asked >= QUESTIONS_PER_END || !learn::loadQuestion(questionsFor.c_str(), *buffer, item, step, left)) {
    return false;
  }
  phase = StreamPhase::Prompt;
  shownAt = platformMillis();
  progress.done = asked;
  progress.total = static_cast<uint16_t>(asked + std::min<uint16_t>(left, QUESTIONS_PER_END - asked));
  return true;
}

void StepRun::grade(const Answer& answer) {
  answerQuestion(step, item, answer, elapsedMs());
  ++asked;
  progress.done = asked;
}

// A book puzzle's result; the screen stays until the reader moves on.
void StepRun::solve(const bool correct, const int8_t pick) { markSolved(item, correct, pick, elapsedMs()); }

StepRun::After StepRun::pass() {
  markSeen(item);
  return next();
}

void StepRun::pick() {
  chosen = static_cast<int8_t>(selected);
  const bool right = selected == item.answer;
  if (isCard()) {
    grade({right ? fsrs::Grade::Good : fsrs::Grade::Again, static_cast<int8_t>(right ? 1 : 0), chosen});
  } else {
    solve(right, chosen);
  }
  phase = StreamPhase::Answered;
}

StepRun::After StepRun::apply(const StreamMove move, const StreamKeys& keys) {
  using Move = StreamMove;
  switch (move) {
    case Move::None:
    case Move::PageNext:
    case Move::PagePrevious:
      return After::Stay;
    case Move::Reveal:
      phase = StreamPhase::Revealed;
      return After::Reveal;
    case Move::Back:
      // Before the article, Back leaves it unopened; after it, what was left is not asked again.
      if (piece()) return pieceAfter ? finishPiece() : After::Leave;
      // The bite is passed on the way to the page; a puzzle left unanswered stays unseen, and a
      // question skipped stays due.
      return outsideOf == Outside::Bite ? pass() : After::Leave;
    case Move::Good:
    case Move::Again:
      // Left grades Again, or a riddle missed.
      if (kindOf(item.shape).result == Result::Solved) {
        solve(!keys.frontPrevious, -1);
      } else {
        grade({keys.frontPrevious ? fsrs::Grade::Again : fsrs::Grade::Good});
      }
      return next();
    case Move::SelectNext:
    case Move::SelectPrevious: {
      const uint8_t count = item.optionCount;
      selected = static_cast<uint8_t>(keys.next ? (selected + 1) % count : (selected + count - 1) % count);
      return After::Stay;
    }
    case Move::Pick:
      if (item.shape == Shape::Forecast) {
        // How likely, and on: what happens is told when it has happened.
        writeSeenForecast(forecast.id, forecastPercent(selected), elapsedMs(), state().day, wallClock(), article.id);
        guess.forecast = true;
        return finishPiece();
      }
      if (item.shape == Shape::Predict) {
        // The guess is kept: the article has the answer. Logged at once: a piece guessed and
        // never read still tells what was guessed.
        guess.pick = static_cast<int8_t>(selected);
        guess.ms = elapsedMs();
        saveGuess(article, text(buffer->text, article.book), guess);
        ArticleSeen seen;
        seen.pick = guess.pick;
        seen.correct = guess.pick == article.answer;
        seen.ms = guess.ms;
        writeSeenArticle(article.id, seen, state().day, wallClock());
        return After::Open;
      }
      if (item.shape == Shape::Link) {
        // Kept at once, so a sleep on its answer does not ask again.
        guess.link = static_cast<int8_t>(selected);
        guess.linkMs = elapsedMs();
        saveGuess(article, text(buffer->text, article.book), guess);
        ArticleSeen seen;
        seen.opened = true;
        seen.link = guess.link;
        seen.linkCorrect = guess.link == article.linkAnswer;
        seen.linkMs = guess.linkMs;
        writeSeenArticle(article.id, seen, state().day, wallClock());
        showPiecePart();
        return After::Chosen;
      }
      pick();
      return After::Chosen;
    case Move::Pass:
      return pass();
    case Move::Continue:
      if (piece()) {
        // The answer was read on: one more thing may follow it. A link's answer ends the piece.
        if (item.shape != Shape::Predict) return finishPiece();
        answerShown = true;
        return showPiecePart() ? After::Step : finishPiece();
      }
      return next();
    case Move::Lift:
    case Move::Drop:
      holding = move == Move::Lift;
      return After::Stay;
    case Move::CarryNext:
    case Move::CarryPrevious: {
      const uint8_t to = static_cast<uint8_t>(move == Move::CarryNext ? selected + 1 : selected - 1);
      orderMove(rows, item.optionCount, selected, to);
      selected = to;
      return After::Selected;
    }
    case Move::Check:
      solve(orderSolved(rows, item.optionCount), -1);
      phase = StreamPhase::Answered;
      chosen = 0;
      return After::Chosen;
  }
  return After::Stay;
}

void StepRun::describe(JsonObject out) const {
  static constexpr const char* PHASES[] = {"prompt", "revealed", "answered", "empty"};
  out["phase"] = PHASES[static_cast<uint8_t>(phase)];
  out["done"] = progress.done;
  out["total"] = progress.total;
  if (phase == StreamPhase::Empty) return;
  out["kind"] = kindOf(item.shape).name;
  // A stream item by its id; a card by its card key.
  char id[2 * ID_MAX + 2];
  if (isCard()) {
    snprintf(id, sizeof(id), "%s/%s", step.entry.deck, item.id);
  } else {
    snprintf(id, sizeof(id), "%s", item.id);
  }
  out["id"] = id;
  const Layout shown = layout();
  const bool rowsShown = shown == Layout::Options || shown == Layout::Order;
  if (rowsShown && phase == StreamPhase::Prompt) out["selected"] = selected;
  if (shown == Layout::Order && holding) out["holding"] = true;
  if (shown == Layout::Options && phase == StreamPhase::Answered && chosen >= 0) out["picked"] = chosen;
}

}  // namespace x4::learn
