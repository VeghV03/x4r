#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "SentenceSteps.h"

namespace {

// Words split on spaces; a newline starts a paragraph. `page` gives each word's page.
struct Run {
  std::vector<std::string> text;
  std::vector<bool> paragraph;
  std::vector<int> page;

  Run(const std::vector<std::string>& pages) {
    for (size_t p = 0; p < pages.size(); ++p) {
      bool start = text.empty();
      std::string current;
      auto flush = [&] {
        if (current.empty()) return;
        text.push_back(current);
        paragraph.push_back(start);
        page.push_back(static_cast<int>(p));
        start = false;
        current.clear();
      };
      for (const char c : pages[p]) {
        if (c == ' ') {
          flush();
        } else if (c == '\n') {
          flush();
          start = true;
        } else {
          current += c;
        }
      }
      flush();
    }
  }

  static sentences::Word at(const void* ctx, const size_t i) {
    const auto& run = *static_cast<const Run*>(ctx);
    return {run.text[i], run.paragraph[i]};
  }

  sentences::Steps steps(const size_t limit = sentences::SELECTION_TEXT_MAX) const {
    return sentences::Steps(&Run::at, this, text.size(), limit);
  }

  std::string join(const sentences::Range& r) const {
    std::string out;
    for (int i = r.first; i <= r.last; ++i) out += (i > r.first ? " " : "") + text[i];
    return out;
  }
};

// Page 0 ends mid-paragraph and mid-sentence; page 1 goes on with the same paragraph.
const Run BOOK({"First one. Second one here. Third goes", "on. Fourth.\nNew paragraph starts. It ends.\nLast one."});

sentences::Range firstSentence(const Run& run) {
  const auto steps = run.steps();
  return {0, steps.sentenceEnd(0), 0};
}

TEST(SentenceSteps, GrowCrossesThePageBreak) {
  const auto steps = BOOK.steps();
  auto r = firstSentence(BOOK);
  EXPECT_EQ(BOOK.join(r), "First one.");
  ASSERT_TRUE(steps.grow(r));
  ASSERT_TRUE(steps.grow(r));
  EXPECT_EQ(BOOK.join(r), "First one. Second one here. Third goes on.");
  EXPECT_EQ(BOOK.page[r.focus], 1);  // the screen follows the end onto the next page
  EXPECT_EQ(r.first, 0);
}

TEST(SentenceSteps, HoldGrowsToTheParagraphEndAcrossPages) {
  const auto steps = BOOK.steps();
  auto r = firstSentence(BOOK);
  ASSERT_TRUE(steps.growParagraph(r));
  EXPECT_EQ(BOOK.join(r), "First one. Second one here. Third goes on. Fourth.");
  ASSERT_TRUE(steps.growParagraph(r));
  EXPECT_EQ(BOOK.join(r), "First one. Second one here. Third goes on. Fourth. New paragraph starts. It ends.");
  ASSERT_TRUE(steps.growParagraph(r));
  ASSERT_FALSE(steps.growParagraph(r));  // end of the run
  EXPECT_EQ(r.last, static_cast<int>(BOOK.text.size()) - 1);
}

TEST(SentenceSteps, TrimBySentenceAndWord) {
  const auto steps = BOOK.steps();
  auto r = firstSentence(BOOK);
  ASSERT_TRUE(steps.growParagraph(r));
  ASSERT_TRUE(steps.shrink(r));
  EXPECT_EQ(BOOK.join(r), "First one. Second one here. Third goes on.");
  ASSERT_TRUE(steps.shrinkWord(r));
  EXPECT_EQ(BOOK.join(r), "First one. Second one here. Third goes");
  EXPECT_EQ(BOOK.page[r.focus], 0);  // trimming back over the break returns to the first page
  ASSERT_TRUE(steps.shrink(r));
  ASSERT_TRUE(steps.shrink(r));
  EXPECT_EQ(BOOK.join(r), "First one.");
  EXPECT_FALSE(steps.shrink(r));  // never below the first sentence
  ASSERT_TRUE(steps.shrinkWord(r));
  EXPECT_EQ(BOOK.join(r), "First");
  EXPECT_FALSE(steps.shrinkWord(r));
}

TEST(SentenceSteps, NextAndPreviousMoveOneSentence) {
  const auto steps = BOOK.steps();
  auto r = firstSentence(BOOK);
  ASSERT_TRUE(steps.grow(r));
  ASSERT_TRUE(steps.next(r));
  EXPECT_EQ(BOOK.join(r), "Third goes on.");
  ASSERT_TRUE(steps.previous(r));
  EXPECT_EQ(BOOK.join(r), "Second one here.");
  EXPECT_EQ(r.focus, r.first);
}

TEST(SentenceSteps, GrowthStopsAtTheTextLimit) {
  // "First one. Second one here." is 28 bytes with separators.
  const auto steps = BOOK.steps(30);
  auto r = firstSentence(BOOK);
  ASSERT_TRUE(steps.grow(r));
  EXPECT_EQ(BOOK.join(r), "First one. Second one here.");
  EXPECT_FALSE(steps.grow(r));
  // The paragraph does not fit, and no further sentence does either.
  EXPECT_FALSE(steps.growParagraph(r));
  EXPECT_LE(steps.bytes(r.first, r.last), 30u);

  // A paragraph that does not fit grows to its last sentence that does.
  const auto wider = BOOK.steps(45);
  auto w = firstSentence(BOOK);
  ASSERT_TRUE(wider.growParagraph(w));
  EXPECT_EQ(BOOK.join(w), "First one. Second one here. Third goes on.");
}

TEST(SentenceSteps, BytesBoundTheExportedText) {
  const auto steps = BOOK.steps();
  auto r = firstSentence(BOOK);
  while (steps.growParagraph(r)) {
  }
  EXPECT_GE(steps.bytes(r.first, r.last), BOOK.join(r).size());
}

TEST(SentenceSteps, PageWindowHoldsAFullSelectionAfterTheFirstPage) {
  using sentences::wantsPage;
  EXPECT_TRUE(wantsPage(0, 0, 8));
  EXPECT_TRUE(wantsPage(1, 0, 8));
  EXPECT_TRUE(wantsPage(3, 3999, 8));
  EXPECT_FALSE(wantsPage(3, 4000, 8));
  EXPECT_FALSE(wantsPage(8, 100, 8));
}

}  // namespace
