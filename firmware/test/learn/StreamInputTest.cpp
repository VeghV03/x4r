#include <gtest/gtest.h>

#include "StreamInput.h"

using x4::Shape;
using x4::StreamKeys;
using x4::StreamMove;
using x4::StreamPhase;
using x4::StreamView;

namespace {

StreamKeys confirm() {
  StreamKeys k;
  k.confirm = true;
  return k;
}
StreamKeys sideNext() {
  StreamKeys k;
  k.next = true;
  return k;
}
StreamKeys sidePrevious() {
  StreamKeys k;
  k.previous = true;
  return k;
}
StreamKeys frontNext() {
  StreamKeys k = sideNext();
  k.frontNext = true;
  return k;
}
StreamKeys frontPrevious() {
  StreamKeys k = sidePrevious();
  k.frontPrevious = true;
  return k;
}

StreamView at(StreamPhase phase, Shape shape, bool more = false, bool earlier = false) {
  StreamView v;
  v.phase = phase;
  v.shape = shape;
  v.morePages = more;
  v.earlierPages = earlier;
  return v;
}

// Applies one key to a paged item the way StreamActivity does. Returns the
// number of steps advanced and updates the page.
int press(StreamView& v, int& page, int pages, const StreamKeys& keys) {
  v.morePages = page + 1 < pages;
  v.earlierPages = page > 0;
  switch (x4::streamMove(v, keys)) {
    case StreamMove::PageNext:
      ++page;
      return 0;
    case StreamMove::PagePrevious:
      --page;
      return 0;
    case StreamMove::Pass:
    case StreamMove::Continue:
      return 1;
    default:
      return 0;
  }
}

}  // namespace

TEST(StreamInputTest, NoKeyDoesNothing) {
  for (const auto phase : {StreamPhase::Prompt, StreamPhase::Revealed, StreamPhase::Answered}) {
    EXPECT_EQ(x4::streamMove(at(phase, Shape::Flip, true, true), StreamKeys{}), StreamMove::None);
  }
}

TEST(StreamInputTest, OneForwardPressMovesOnePage) {
  // A Right press is one `next` event: page 0 to page 1, never to page 2.
  StreamView v = at(StreamPhase::Prompt, Shape::Bite);
  int page = 0;
  EXPECT_EQ(press(v, page, 3, frontNext()), 0);
  EXPECT_EQ(page, 1);
  EXPECT_EQ(press(v, page, 3, frontNext()), 0);
  EXPECT_EQ(page, 2);
  EXPECT_EQ(press(v, page, 3, frontNext()), 1);
}

TEST(StreamInputTest, PreviousOnFirstPageStays) {
  // Left on page 0 must not move forward.
  for (const auto shape : {Shape::Bite, Shape::Highlight, Shape::Flip, Shape::Explain}) {
    EXPECT_EQ(x4::streamMove(at(StreamPhase::Prompt, shape, true, false), frontPrevious()), StreamMove::None);
  }
  EXPECT_EQ(x4::streamMove(at(StreamPhase::Answered, Shape::Choice, true, false), frontPrevious()),
            StreamMove::None);
  EXPECT_EQ(x4::streamMove(at(StreamPhase::Prompt, Shape::Bite, false, true), sidePrevious()),
            StreamMove::PagePrevious);
}

TEST(StreamInputTest, ItemsAdvanceOnlyOnNextOrConfirm) {
  const StreamView highlight = at(StreamPhase::Prompt, Shape::Highlight);
  EXPECT_EQ(x4::streamMove(highlight, confirm()), StreamMove::Pass);
  EXPECT_EQ(x4::streamMove(highlight, sideNext()), StreamMove::Pass);
  EXPECT_EQ(x4::streamMove(highlight, sidePrevious()), StreamMove::None);
}

TEST(StreamInputTest, ChoiceResultContinuesOnce) {
  // After a pick, one key shows the next step; the item after it is not skipped.
  StreamView v = at(StreamPhase::Answered, Shape::Choice);
  EXPECT_EQ(x4::streamMove(v, frontNext()), StreamMove::Continue);
  EXPECT_EQ(x4::streamMove(v, confirm()), StreamMove::Continue);
  v.morePages = true;
  EXPECT_EQ(x4::streamMove(v, confirm()), StreamMove::PageNext);
}

TEST(StreamInputTest, LongCardPagesBeforeReveal) {
  StreamView v = at(StreamPhase::Prompt, Shape::Flip, true);
  EXPECT_EQ(x4::streamMove(v, confirm()), StreamMove::PageNext);
  v.morePages = false;
  EXPECT_EQ(x4::streamMove(v, confirm()), StreamMove::Reveal);
  EXPECT_EQ(x4::streamMove(v, sideNext()), StreamMove::Reveal);  // forward: no dead press
  EXPECT_EQ(x4::streamMove(v, frontNext()), StreamMove::Reveal);
}

TEST(StreamInputTest, RevealedGradesAgainWithLeftAndGoodGoingForward) {
  StreamView v = at(StreamPhase::Revealed, Shape::Flip);
  EXPECT_EQ(x4::streamMove(v, frontPrevious()), StreamMove::Again);
  EXPECT_EQ(x4::streamMove(v, frontNext()), StreamMove::Good);
  EXPECT_EQ(x4::streamMove(v, confirm()), StreamMove::Good);
  EXPECT_EQ(x4::streamMove(v, sideNext()), StreamMove::Good);
  EXPECT_EQ(x4::streamMove(v, sidePrevious()), StreamMove::None);

  v.morePages = true;
  v.earlierPages = true;
  EXPECT_EQ(x4::streamMove(v, sideNext()), StreamMove::PageNext);
  EXPECT_EQ(x4::streamMove(v, confirm()), StreamMove::PageNext);
  EXPECT_EQ(x4::streamMove(v, sidePrevious()), StreamMove::PagePrevious);
}

// Left on a long back pages back, as on every step; it grades only on the last page.
TEST(StreamInputTest, RevealedGradesOnlyOnTheLastPage) {
  StreamView v = at(StreamPhase::Revealed, Shape::Flip, true, false);  // first of several pages
  EXPECT_EQ(x4::streamMove(v, frontPrevious()), StreamMove::None);
  EXPECT_EQ(x4::streamMove(v, frontNext()), StreamMove::PageNext);

  v.earlierPages = true;  // a middle page
  EXPECT_EQ(x4::streamMove(v, frontPrevious()), StreamMove::PagePrevious);
  EXPECT_EQ(x4::streamMove(v, frontNext()), StreamMove::PageNext);

  v.morePages = false;  // the last page
  EXPECT_EQ(x4::streamMove(v, frontPrevious()), StreamMove::Again);
  EXPECT_EQ(x4::streamMove(v, frontNext()), StreamMove::Good);
  EXPECT_EQ(x4::streamMove(v, sidePrevious()), StreamMove::PagePrevious);
}

TEST(StreamInputTest, ChoicePagesToTheOptionsThenSelects) {
  StreamView v = at(StreamPhase::Prompt, Shape::Choice, true);
  v.selectionVisible = false;
  EXPECT_EQ(x4::streamMove(v, sideNext()), StreamMove::PageNext);
  EXPECT_EQ(x4::streamMove(v, confirm()), StreamMove::PageNext);

  v.selectionVisible = true;
  EXPECT_EQ(x4::streamMove(v, sideNext()), StreamMove::SelectNext);
  EXPECT_EQ(x4::streamMove(v, confirm()), StreamMove::Pick);
  EXPECT_EQ(x4::streamMove(v, frontNext()), StreamMove::Pick);  // Right answers, as Order's Check

  // Up from the first option goes back to the prompt page.
  v.earlierPages = true;
  v.firstOption = true;
  EXPECT_EQ(x4::streamMove(v, sidePrevious()), StreamMove::PagePrevious);
  v.firstOption = false;
  EXPECT_EQ(x4::streamMove(v, sidePrevious()), StreamMove::SelectPrevious);
  EXPECT_EQ(x4::streamMove(v, frontPrevious()), StreamMove::PagePrevious);  // Left: the previous page
}

TEST(StreamInputTest, LeftPagesBackOnRows) {
  for (const Shape shape : {Shape::Order, Shape::Odd}) {
    StreamView v = at(StreamPhase::Prompt, shape, false, true);
    EXPECT_EQ(x4::streamMove(v, frontPrevious()), StreamMove::PagePrevious);
    v.earlierPages = false;
    EXPECT_EQ(x4::streamMove(v, frontPrevious()), StreamMove::None);
  }
}

// Left on a first page has nothing to go back to; a row picked up is carried, not left behind.
TEST(StreamInputTest, LeftOnAFirstPageDoesNothing) {
  StreamKeys left;
  left.previous = left.frontPrevious = true;
  for (const Shape shape : {Shape::Bite, Shape::Flip, Shape::Choice}) {
    EXPECT_EQ(x4::streamMove(at(StreamPhase::Prompt, shape), left), StreamMove::None);
  }
  StreamView view = at(StreamPhase::Prompt, Shape::Order);
  view.holding = true;
  EXPECT_EQ(x4::streamMove(view, left), StreamMove::None);
}
