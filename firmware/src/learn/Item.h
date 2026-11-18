#pragma once

#include <cstddef>
#include <cstdint>

#include "LearnTypes.h"

namespace x4 {

// What a stream step shows. Cards and stream items share one shape so the
// stream screen renders them through one path. The four card types come first, in
// CardType's order; a new kind goes last, with its row in Item.cpp's table.
enum class Shape : uint8_t {
  Flip,
  Choice,
  Cloze,
  Explain,
  Bite,
  Highlight,
  Riddle,
  Order,
  Odd,
  Logic,
  // A piece of the paper (parsePiece), never a stream line: shown as its parts.
  Article,
  Predict,  // its predict question
  Link,     // its link question
  Forecast, // the forecast it carries
};

// A NUL-terminated string inside the arena (LineBuffer::text).
struct Slice {
  uint16_t offset = 0;
  uint16_t length = 0;
};

// One parsed step. Every Slice points into the arena it was parsed into.
//   Flip:      title = front, body = back
//   Choice:    title = prompt, options, answer
//   Cloze:     title = text with a blank, body = text filled in, options, answer
//   Explain:   title = prompt, options = points
//   Bite:      title, body, book
//   Highlight: body = text, title = book title
//   Riddle:    title = prompt, body = answer, book
//   Order:     title = prompt, options = items in the solved order, why, book
//   Odd/Logic: title = prompt, options, answer, why, book
//   Article:   title, book; with a predict question also body = its prompt, options, answer,
//              why = the article's sentence that settles it
//              with a link question also links = its three options, linkPrompt, linkWhy,
//              linkAnswer
//   Predict:   an Article shown as its predict question: title = the prompt
//   Link:      an Article shown as its link question: title = linkPrompt, options = links,
//              answer = linkAnswer, why = linkWhy (StepRun builds both from the Article)
//   Forecast:  title = the statement and the day it is settled, options = the five rows
struct Item {
  Shape shape = Shape::Bite;
  Slice title;
  Slice body;
  Slice why;
  Slice book;
  Slice options[LIST_MAX];
  Slice links[LIST_MAX];
  Slice linkPrompt;
  Slice linkWhy;
  uint8_t optionCount = 0;
  uint8_t answer = 0;
  uint8_t linkAnswer = 0;
  char id[ID_MAX + 1] = {};
  char highlightId[ID_MAX + 1] = {};  // highlight item: id to resolve
  uint32_t settles = 0;               // a piece's forecast: the day that settles it, as YYYYMMDD
};

inline const char* text(const char* arena, const Slice slice) { return arena + slice.offset; }

// How a step's screen is built. Its keys (StreamInput) and the text that can be
// highlighted on it (FeedText) follow from it.
enum class Layout : uint8_t {
  Answer,   // a prompt; once revealed, the answer under a rule
  Points,   // a prompt; once revealed, the points
  Options,  // a prompt and options to pick one of
  Order,    // a prompt and rows to put in order
  Text,     // a title and a body over as many screens as it needs
  Quote,    // a highlight behind its bar
};

// What finishing a step writes.
enum class Result : uint8_t {
  Seen,    // a seen.jsonl line
  Solved,  // a seen.jsonl line with right or wrong: the book puzzles
  Graded,  // a reviews.jsonl line and the card's schedule
};

// One row per kind of step: all that the step runner, the keys and the screen need to know of
// it. A new kind is a Shape, a row and its parser (Item.cpp).
struct StepKind {
  const char* name;  // the card type or stream kind as docs/DATA.md spells it
  Layout layout;
  Result result;
};
const StepKind& kindOf(Shape shape);
// An article's link question has this many things to choose from.
inline constexpr uint8_t LINK_OPTIONS = 3;
// A forecast's rows: how likely, in percent. The selection starts in the middle, so a press
// without thought says "no idea".
inline constexpr uint8_t FORECAST_ROWS = 5;
inline constexpr uint8_t FORECAST_START = 2;
inline constexpr uint8_t forecastPercent(const uint8_t row) { return static_cast<uint8_t>(10 + 20 * row); }
// Outside the set (SPEC: Book, Play): what the bite may be, and the book puzzles.
inline bool isBite(const Shape shape) { return shape == Shape::Bite || shape == Shape::Highlight; }
inline bool isBookPuzzle(const Shape shape) {
  return shape == Shape::Riddle || shape == Shape::Order || shape == Shape::Odd || shape == Shape::Logic;
}


// Parses one deck card line held in arena[0..length) and copies its strings
// into the same arena. Returns false (and leaves `out` unusable) for an invalid
// card: unknown type, missing field, or over a DATA.md limit.
bool parseCard(char* arena, size_t length, Item& out);
// Like parseCard, but only validates and reports the id and type (for indexing).
bool scanCard(const char* line, size_t length, char (&id)[ID_MAX + 1], CardType& type);
// Parses a stream.jsonl line. Highlight items carry only highlightId unless the
// line also has `text` (then title/body come from the line itself).
bool parseStreamItem(char* arena, size_t length, Item& out);
// A piece of the paper (a /x4/read.jsonl line) as the article step it is: its `predict` and
// `link` as on a stream article. `forecast` gets the forecast it carries (its id the
// forecast's), or an empty id; `thread` its thread's id, or an empty one.
bool parsePiece(char* arena, size_t length, Item& article, Item& forecast, char (&thread)[ID_MAX + 1]);
// Reads the id of a stream.jsonl or highlights.jsonl line.
bool scanId(const char* line, size_t length, char (&id)[ID_MAX + 1]);
// Fills a highlight item's body and title from a highlights.jsonl line.
bool parseHighlightInto(char* arena, size_t length, Item& item);

}  // namespace x4
