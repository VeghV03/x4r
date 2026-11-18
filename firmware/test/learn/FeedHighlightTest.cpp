#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "FeedHighlights.h"
#include "FeedText.h"
#include "HalStorage.h"
#include "Item.h"
#include "JsonlReader.h"
#include "Learn.h"
#include "LearnPaths.h"

using namespace x4;

namespace {

struct Feed : ::testing::Test {
  void SetUp() override {
    fake::reset();
    learn::forgetPowerOn();
    buffer = std::make_unique<LineBuffer>();
  }

  Item parse(const std::string& line) {
    std::memcpy(buffer->text, line.data(), line.size());
    buffer->text[line.size()] = '\0';
    Item item;
    EXPECT_TRUE(parseStreamItem(buffer->text, line.size(), item)) << line;
    return item;
  }
  // An explain card, revealed: its points are fields one after another.
  Item points(const std::string& a, const std::string& b) {
    const std::string line = R"({"id":"c1","type":"explain","prompt":"P","points":[")" + a + R"(",")" + b + R"("]})";
    std::memcpy(buffer->text, line.data(), line.size());
    buffer->text[line.size()] = '\0';
    Item item;
    EXPECT_TRUE(parseCard(buffer->text, line.size(), item)) << line;
    return item;
  }

  std::vector<feed::Word> split(const feed::Fields& fields) {
    std::vector<feed::Word> words(feed::splitWords(buffer->text, fields, nullptr, 0));
    feed::splitWords(buffer->text, fields, words.data(), words.size());
    return words;
  }

  std::string text(const feed::Fields& fields, const feed::Word& w) {
    return {feed::wordText(buffer->text, fields, w), w.length};
  }

  std::unique_ptr<LineBuffer> buffer;
};

std::vector<std::string> lines(const std::string& path) {
  const auto it = fake::files.find(path);
  std::vector<std::string> out;
  if (it == fake::files.end()) return out;
  std::string all(it->second->bytes.begin(), it->second->bytes.end());
  size_t start = 0;
  while (start < all.size()) {
    const size_t end = all.find('\n', start);
    out.push_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

}  // namespace

TEST_F(Feed, TextStepsOfferTheirReadingTextOnly) {
  Item bite = parse(R"({"id":"s1","kind":"bite","title":"T","body":"One. Two.","book":"books/m.epub"})");
  auto f = feed::fieldsFor(bite, StreamPhase::Prompt);
  ASSERT_EQ(f.count, 1);
  EXPECT_EQ(f.slice[0].offset, bite.body.offset);

  Item card = points("Első pont.", "Második.");
  EXPECT_EQ(feed::fieldsFor(card, StreamPhase::Prompt).count, 0);
  f = feed::fieldsFor(card, StreamPhase::Revealed);
  ASSERT_EQ(f.count, 2);
  EXPECT_EQ(f.base[0], 0);
  EXPECT_EQ(f.base[1], card.options[0].length + 1);

  Item riddle = parse(R"({"id":"s3","kind":"riddle","prompt":"What?","answer":"This."})");
  EXPECT_EQ(feed::fieldsFor(riddle, StreamPhase::Prompt).count, 0);
  EXPECT_EQ(feed::fieldsFor(riddle, StreamPhase::Revealed).count, 1);

  Item odd = parse(R"({"id":"s4","kind":"odd","prompt":"P","options":["a","b","c"],"answer":1,"why":"Because."})");
  EXPECT_EQ(feed::fieldsFor(odd, StreamPhase::Prompt).count, 0);
  f = feed::fieldsFor(odd, StreamPhase::Answered);
  ASSERT_EQ(f.count, 1);
  EXPECT_EQ(f.slice[0].offset, odd.why.offset);
  EXPECT_EQ(feed::fieldOf(f, odd.why), 0);
  EXPECT_EQ(feed::fieldOf(f, odd.title), -1);
}

TEST_F(Feed, WordsCarryJoinedOffsetsAndParagraphs) {
  Item card = points("Ab cd.", "Ef\\ngh.");
  const auto f = feed::fieldsFor(card, StreamPhase::Revealed);
  const auto words = split(f);
  ASSERT_EQ(words.size(), 4u);
  EXPECT_EQ(text(f, words[0]), "Ab");
  EXPECT_TRUE(words[0].paragraphStart);
  EXPECT_FALSE(words[1].paragraphStart);
  EXPECT_EQ(text(f, words[2]), "Ef");
  EXPECT_TRUE(words[2].paragraphStart);  // the next point
  EXPECT_EQ(words[2].start, 7);          // "Ab cd." and one joining byte
  EXPECT_TRUE(words[3].paragraphStart);  // after a line end
  EXPECT_EQ(words[0].screen, feed::SCREEN_UNKNOWN);

  EXPECT_EQ(feed::findWord(words.data(), words.size(), 0), 0);
  EXPECT_EQ(feed::findWord(words.data(), words.size(), 4), 1);
  EXPECT_EQ(feed::findWord(words.data(), words.size(), 2), -1);  // the space
  EXPECT_EQ(feed::findWord(words.data(), words.size(), 8), 2);
}

TEST_F(Feed, SelectionStartsAtTheScreensFirstSentence) {
  Item bite = parse(R"({"id":"s1","kind":"bite","title":"T","body":"end of one. Two is here. Three."})");
  const auto f = feed::fieldsFor(bite, StreamPhase::Prompt);
  auto words = split(f);
  // Screen 0 shows nothing; the text starts mid-sentence on screen 1 (a later page).
  for (auto& w : words) w.screen = 1;
  words[0].paragraphStart = false;
  feed::Run run{buffer->text, &f, words.data(), words.size()};
  auto range = feed::firstSentence(run, 1);
  EXPECT_EQ(text(f, words[range.first]), "Two");
  EXPECT_EQ(text(f, words[range.last]), "here.");
  range = feed::firstSentence(run, 0);  // nothing there: the first word laid out later
  EXPECT_EQ(text(f, words[range.first]), "Two");

  ASSERT_TRUE(run.steps().grow(range));
  EXPECT_EQ(feed::selectedText(run, range.first, range.last), "Two is here. Three.");
  EXPECT_EQ(feed::rangeStart(run, range.first), 12);
  EXPECT_EQ(feed::rangeEnd(run, range.last), 31);
}

TEST_F(Feed, SelectedTextKeepsParagraphs) {
  Item card = points("One.", "Two.");
  const auto f = feed::fieldsFor(card, StreamPhase::Revealed);
  const auto words = split(f);
  feed::Run run{buffer->text, &f, words.data(), words.size()};
  EXPECT_EQ(feed::selectedText(run, 0, 1), "One.\n\nTwo.");
}

TEST_F(Feed, SavedHighlightsAreLoggedForTheHostAndMarkedOnTheirStep) {
  feed::Source source;
  source.key = "s-1";
  source.kind = "bite";
  source.title = "Why Marcus wrote";
  source.book = "books/m.epub";
  feed::Mark mark;
  ASSERT_TRUE(feed::save(source, 4, 20, "Some words here.", HighlightKind::Highlight, mark));
  EXPECT_EQ(mark.start, 4);
  EXPECT_EQ(mark.end, 20);

  feed::Source bite;
  bite.key = "s-w45";
  bite.kind = "bite";
  bite.title = "Week 45";
  feed::Mark other;
  ASSERT_TRUE(feed::save(bite, 0, 5, "Hello", HighlightKind::Question, other));

  auto all = lines(paths::HIGHLIGHTS);
  ASSERT_EQ(all.size(), 2u);
  JsonDocument h;
  ASSERT_FALSE(deserializeJson(h, all[0]));
  EXPECT_STREQ(h["id"], mark.id);
  EXPECT_STREQ(h["kind"], "highlight");
  EXPECT_STREQ(h["book"], "books/m.epub");
  EXPECT_TRUE(h["title"].isNull());  // the device does not know the book's title
  EXPECT_STREQ(h["item"]["id"], "s-1");
  EXPECT_STREQ(h["item"]["kind"], "bite");
  EXPECT_STREQ(h["item"]["title"], "Why Marcus wrote");
  EXPECT_STREQ(h["text"], "Some words here.");
  EXPECT_EQ(h["pos"]["start"], 4);
  EXPECT_EQ(h["pos"]["end"], 20);
  EXPECT_TRUE(h["pos"]["spine"].isNull());
  EXPECT_TRUE(h["chapter"].isNull());

  JsonDocument q;
  ASSERT_FALSE(deserializeJson(q, all[1]));
  EXPECT_STREQ(q["kind"], "question");
  EXPECT_STREQ(q["book"], "stream/s-w45");  // no book: the step names itself
  EXPECT_STREQ(q["title"], "Week 45");

  feed::Mark marks[feed::MARKS_MAX];
  ASSERT_EQ(feed::loadMarks("s-1", marks, feed::MARKS_MAX), 1);
  EXPECT_STREQ(marks[0].id, mark.id);
  EXPECT_EQ(feed::loadMarks("s-w45", marks, feed::MARKS_MAX), 1);
  EXPECT_EQ(feed::loadMarks("s-2", marks, feed::MARKS_MAX), 0);

  // The same words of another step are another highlight.
  feed::Source again = source;
  again.key = "s-2";
  feed::Mark twin;
  ASSERT_TRUE(feed::save(again, 4, 20, "Some words here.", HighlightKind::Highlight, twin));
  EXPECT_STRNE(twin.id, mark.id);
}

TEST_F(Feed, DeletingOverlappingHighlightsLogsEachAndClearsTheirMarks) {
  feed::Source source;
  source.key = "deck/c1";
  source.kind = "flip";
  feed::Mark a;
  feed::Mark b;
  feed::Mark c;
  ASSERT_TRUE(feed::save(source, 0, 10, "First one.", HighlightKind::Highlight, a));
  ASSERT_TRUE(feed::save(source, 11, 20, "Second.", HighlightKind::Highlight, b));
  ASSERT_TRUE(feed::save(source, 30, 40, "Third.", HighlightKind::Highlight, c));

  EXPECT_FALSE(feed::removeOverlapping("deck/c1", 21, 29));
  ASSERT_TRUE(feed::removeOverlapping("deck/c1", 5, 15));
  feed::Mark marks[feed::MARKS_MAX];
  ASSERT_EQ(feed::loadMarks("deck/c1", marks, feed::MARKS_MAX), 1);
  EXPECT_STREQ(marks[0].id, c.id);

  const auto all = lines(paths::HIGHLIGHTS);
  ASSERT_EQ(all.size(), 5u);
  EXPECT_EQ(all[0].find_first_not_of(' '), std::string::npos);  // blanked in place
  EXPECT_EQ(all[1].find_first_not_of(' '), std::string::npos);
  EXPECT_NE(all[3].find(a.id), std::string::npos);
  EXPECT_NE(all[3].find("\"deleted\":true"), std::string::npos);
  EXPECT_NE(all[4].find(b.id), std::string::npos);
}

TEST_F(Feed, DeckBookComesFromTheHeader) {
  fake::add("/x4/decks/d.jsonl", R"({"deck":"d","title":"Deck","book":"books/a.epub"})"
                                 "\n"
                                 R"({"id":"c1","type":"flip","front":"F","back":"B"})"
                                 "\n");
  fake::add("/x4/decks/e.jsonl", R"({"deck":"e","title":"No book"})"
                                 "\n");
  char book[64];
  ASSERT_TRUE(feed::deckBook("d", book, sizeof(book)));
  EXPECT_STREQ(book, "books/a.epub");
  EXPECT_FALSE(feed::deckBook("e", book, sizeof(book)));
  EXPECT_FALSE(feed::deckBook("missing", book, sizeof(book)));
}
