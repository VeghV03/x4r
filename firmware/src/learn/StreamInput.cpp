#include "StreamInput.h"

namespace x4 {

namespace {

// SPEC "Buttons": Right goes forward (the next page, else on), Left back (the previous page),
// the side keys move a selection and otherwise act as Left and Right, Confirm acts.
bool sideNext(const StreamKeys& keys) { return keys.next && !keys.frontNext; }
bool sidePrevious(const StreamKeys& keys) { return keys.previous && !keys.frontPrevious; }

StreamMove pageBack(const StreamView& view) {
  return view.earlierPages ? StreamMove::PagePrevious : StreamMove::None;
}

// Rows on the screen: the side keys move the selection; side Up on the first row pages back
// to the prompt. Left pages back.
StreamMove rowSelect(const StreamView& view, const StreamKeys& keys) {
  if (sideNext(keys)) return StreamMove::SelectNext;
  if (sidePrevious(keys)) {
    if (view.earlierPages && (!view.selectionVisible || view.firstOption)) return StreamMove::PagePrevious;
    return StreamMove::SelectPrevious;
  }
  return keys.frontPrevious ? pageBack(view) : StreamMove::None;
}

bool rowsHidden(const StreamView& view) { return !view.selectionVisible && view.morePages; }

// Rows below a prompt longer than the screen: every key pages until the rows show.
StreamMove pageToRows(const StreamView& view, const StreamKeys& keys) {
  if (keys.confirm || keys.next) return StreamMove::PageNext;
  return keys.previous ? pageBack(view) : StreamMove::None;
}

// Order: Confirm picks a row up and puts it down, the arrows carry it, Right checks.
StreamMove orderKeys(const StreamView& view, const StreamKeys& keys) {
  if (view.holding) {
    if (keys.confirm) return StreamMove::Drop;
    if (keys.next) return view.lastOption ? StreamMove::None : StreamMove::CarryNext;
    if (keys.previous) return view.firstOption ? StreamMove::None : StreamMove::CarryPrevious;
    return StreamMove::None;
  }
  if (rowsHidden(view)) return pageToRows(view, keys);
  if (keys.confirm) return StreamMove::Lift;
  if (keys.frontNext) return StreamMove::Check;
  return rowSelect(view, keys);
}

// Choice, cloze and the option puzzles: Confirm or Right answers with the selected option.
StreamMove optionKeys(const StreamView& view, const StreamKeys& keys) {
  if (rowsHidden(view)) return pageToRows(view, keys);
  if (keys.confirm || keys.frontNext) return StreamMove::Pick;
  return rowSelect(view, keys);
}

// What the keys do on the screen.
StreamMove keyMove(const StreamView& view, const StreamKeys& keys) {
  const bool forward = keys.confirm || keys.next;
  switch (view.phase) {
    case StreamPhase::Prompt:
      switch (kindOf(view.shape).layout) {
        case Layout::Answer:
        case Layout::Points:
          if (forward) return view.morePages ? StreamMove::PageNext : StreamMove::Reveal;
          return keys.previous ? pageBack(view) : StreamMove::None;
        case Layout::Options:
          return optionKeys(view, keys);
        case Layout::Order:
          return orderKeys(view, keys);
        case Layout::Text:
        case Layout::Quote:
          // Page through, then on.
          if (forward) return view.morePages ? StreamMove::PageNext : StreamMove::Pass;
          return keys.previous ? pageBack(view) : StreamMove::None;
      }
      return StreamMove::None;
    case StreamPhase::Revealed:
      // A back longer than a screen is read to its last page before either grade. There Left
      // grades Again: the one place it is not the previous page.
      if (view.morePages) {
        if (forward) return StreamMove::PageNext;
        return keys.previous ? pageBack(view) : StreamMove::None;
      }
      if (keys.frontPrevious) return StreamMove::Again;
      if (forward) return StreamMove::Good;
      return keys.previous ? pageBack(view) : StreamMove::None;
    case StreamPhase::Answered:
      if (forward) return view.morePages ? StreamMove::PageNext : StreamMove::Continue;
      return keys.previous ? pageBack(view) : StreamMove::None;
    default:
      return StreamMove::None;
  }
}

}  // namespace

StreamMove streamMove(const StreamView& view, const StreamKeys& keys) { return keyMove(view, keys); }

}  // namespace x4
