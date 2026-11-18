#pragma once

#include <ArduinoJson.h>

#include <cstdint>

#include "Fsrs.h"
#include "Item.h"
#include "Journal.h"
#include "JsonlReader.h"
#include "LearnTypes.h"
#include "SetRules.h"

// The learning core: the day, the questions after a chapter, the bite and the book puzzles,
// the paper's guesses, highlights and stats.
// Main-task only. Activities copy what they render; nothing here is read from
// the render task.
namespace x4::learn {

struct Progress {
  uint16_t done = 0;
  uint16_t total = 0;
};

// A loaded step: where its line is.
struct Step {
  StepLine entry{};
};

// Brings the day and the decks up to date: on the first call of a power-on, and after the host
// changed files, it re-indexes changed decks and counts the cards. Cheap once done.
void ensureReady();

const LearnState& state();

// What the reader answered. `correct` below 0: they graded it themselves. `pick`: the option
// chosen, below 0 when the step has none to choose.
struct Answer {
  fsrs::Grade grade = fsrs::Grade::Good;
  int8_t correct = -1;
  int8_t pick = -1;
};
// Predict and link (SPEC: A piece): where a piece that asks a question has got to. Kept on the
// card, so it outlives a sleep in the middle of the article.
struct Guess {
  int8_t pick = -1;     // predict: the option guessed; below 0: none yet
  bool read = false;    // the article was opened and a page turned, or its end reached
  int8_t link = -1;     // link: the option picked; below 0: none yet
  uint32_t ms = 0;      // from the predict question to the guess
  uint32_t linkMs = 0;  // from the link question to the pick
  bool piece = false;     // saved: the piece waits for its article

  bool done = false;      // what follows the piece was shown: it asks nothing again
  bool forecast = false;  // its forecast was answered
};
Guess guessFor(const Item& article);
// `book` is the article's book key. `guess.read` is the card's to keep.
void saveGuess(const Item& article, const char* book, const Guess& guess);
// The reader finished `bookPath` (its close log says so); turned pages come by addReading().
void noteArticleFinished(const char* bookPath);
// Records a stream item as seen.
void markSeen(const Item& item);
// Records a book puzzle's result as its seen line.
void markSolved(const Item& item, bool correct, int8_t pick, uint32_t ms);
// The first stream item, in file order, not seen yet and of a shape `wanted` accepts. False
// when none waits.
bool loadUnseen(LineBuffer& buffer, bool (*wanted)(Shape), Item& item, Step& step);
// True when loadUnseen() has one. Allocates its own scratch.
bool hasUnseen(bool (*wanted)(Shape));
// The bite (SPEC: Book): once a day, on the first book opened. True until noteBite() today.
bool biteDue();
void noteBite();
// The day (DATA.md "The day"): the date while a clock is known, else counted by reading; 0
// before the first power-on.
uint32_t today();
// Questions (SPEC: Questions): cards asked after a chapter or article, at most
// this many each time. One: a question to think about, not a quiz to get through.
inline constexpr uint8_t QUESTIONS_PER_END = 1;
// The next card for the end of a chapter or article of `bookPath`: its decks'
// due cards, the most likely forgotten first, then their new ones; after a Read
// list article, then the same from the decks that belong to no book. `left`
// gets how many are waiting, this one included. False when none is.
bool loadQuestion(const char* bookPath, LineBuffer& buffer, Item& item, Step& step, uint16_t& left);
// True when loadQuestion() has a card for `bookPath`. Allocates its own scratch.
bool hasQuestion(const char* bookPath);
// Schedules the card and logs the review.
void answerQuestion(const Step& step, const Item& item, const Answer& answer, uint32_t ms);

// For the sleep screen: the due card most likely forgotten, else a highlight, else a new card.
bool pickSleepItem(LineBuffer& buffer, Item& item);

// Reader API: adds a reading session to the book's totals in stats.json. `ms` is already
// capped per page.
void addReading(const char* bookPath, uint32_t ms, uint32_t pages);

// Highlight API for the reader: appends to /x4/highlights.jsonl.
bool appendHighlight(const HighlightInput& input, char (&id)[16]);
// Dictionary API for the reader: a word looked up and found while reading `bookPath`,
// appended to /x4/lookups.jsonl for the crosswords.
bool appendLookup(const char* word, const char* bookPath, const char* title);
// Deletes the highlight with this id from /x4/highlights.jsonl and logs the deletion for the host.
bool forgetHighlight(const char* id);

// USB layer: call right after the system clock was set from the host's
// `hello` or `time`. Dates new records from now on and checks the streak.
void onHostTime(int64_t now, int32_t tzMinutes);
// USB layer: the host changed files under /x4. The next ensureReady() compares deck contents;
// a marker file keeps the change pending across power-offs until that succeeds.
void onHostDataChanged();
// Fields for the `hello` reply.
void helloFields(JsonObject out);
// stats.json is out of date after a change outside the core (a finished game).
void refreshStats();
// stats.json is derived, so a change only marks it stale: leaving a book or a game draws the
// next screen first. Writes it if stale; the main loop calls this at rest and before sleep, the
// USB link before the Mac reads a file.
void flushStats();

// Unix seconds once the host set the clock in this power-on, else 0.
int64_t wallClock();
// The wall clock at the host's last UTC offset; false while wallClock() is 0.
bool localTime(CivilTime& out);

#ifdef X4_LEARN_HOST_TEST
// Drops everything held in RAM, as a power-off does.
void forgetPowerOn();
#endif

// Provided by the platform (LearnPlatform.cpp on the device; host tests
// supply their own).
int64_t platformNow();
uint32_t platformMillis();
// True when the host set the clock since power-on, even before a deep sleep;
// then sets tzMinutes to the host's offset.
bool platformHostClock(int32_t& tzMinutes);
void addBookStats(JsonObject books);
void addPlayStats(JsonObject play);

}  // namespace x4::learn
