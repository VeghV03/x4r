#pragma once

#include <cstdint>

#include "Item.h"

// What one key does on a step screen. Pure, so the input rules are
// host-tested apart from the screen.
namespace x4 {

enum class StreamPhase : uint8_t {
  Prompt,    // card front, or an item
  Revealed,  // flip back or explain points, waiting for a grade
  Answered,  // choice or cloze result
  Empty,     // nothing to show (out of memory)
};

// One event per key: Confirm and the direction keys on press, front Left on release (its hold
// starts a highlight). The front button
// inside next/previous is also reported on its own, because a revealed card grades with the
// front buttons and pages with the side ones.
struct StreamKeys {
  bool confirm = false;
  bool next = false;
  bool previous = false;
  bool frontNext = false;
  bool frontPrevious = false;
};

// Where the screen stands.
struct StreamView {
  StreamPhase phase = StreamPhase::Prompt;
  Shape shape = Shape::Bite;
  bool morePages = false;         // a later page of this screen exists
  bool earlierPages = false;      // an earlier page exists
  bool selectionVisible = true;   // choice/cloze: the selected option is on this page
  bool firstOption = true;        // choice/cloze: option 0 is selected
  bool lastOption = false;        // the last row is selected
  bool holding = false;           // order: the selected row is picked up
};

enum class StreamMove : uint8_t {
  None,
  PageNext,
  PagePrevious,
  Reveal,          // flip/explain: show the answer
  Good,
  Again,
  SelectNext,      // choice/cloze: the page follows the selection
  SelectPrevious,
  Pick,            // choice/cloze: answer with the selected option
  Pass,            // item seen, next step
  Continue,        // after a choice result: next step
  Lift,            // order: pick up the selected row
  Drop,            // order: put it down
  CarryNext,       // order: move the picked-up row down one place
  CarryPrevious,
  Check,           // order: check the arrangement
  Back,            // the Back button: leaves the step
};

// Forward keys page through a long screen first, then do what the screen does.
StreamMove streamMove(const StreamView& view, const StreamKeys& keys);

}  // namespace x4
