// The stream's book puzzles, a piece's forecast, the order shuffle, the Read list and its
// read-log (docs/DATA.md).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "Item.h"
#include "Learn.h"
#include "LearnPaths.h"
#include "Puzzle.h"
#include "ReadList.h"
#include "StreamInput.h"

using namespace x4;

namespace {

std::string fileText(const std::string& path) {
  const auto it = fake::files.find(path);
  if (it == fake::files.end()) return {};
  return {it->second->bytes.begin(), it->second->bytes.end()};
}

std::vector<std::string> fileLines(const std::string& path) {
  std::vector<std::string> out;
  const std::string all = fileText(path);
  size_t start = 0;
  while (start < all.size()) {
    const size_t end = all.find('\n', start);
    out.push_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

JsonDocument json(const std::string& line) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, line));
  return doc;
}

std::string quoted(const std::vector<std::string>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) out += (i ? ",\"" : "\"") + values[i] + "\"";
  return out + "]";
}

std::vector<std::string> names(const size_t n) {
  std::vector<std::string> out;
  for (size_t i = 0; i < n; ++i) out.push_back("o" + std::to_string(i));
  return out;
}

struct V2 : ::testing::Test {
  void SetUp() override {
    fake::reset();
    learn::forgetPowerOn();
    buffer = std::make_unique<LineBuffer>();
  }
  bool stream(const std::string& line) {
    std::memcpy(buffer->text, line.data(), line.size());
    return parseStreamItem(buffer->text, line.size(), item);
  }
  const char* at(const Slice slice) const { return text(buffer->text, slice); }

  std::unique_ptr<LineBuffer> buffer;
  Item item;
};

}  // namespace

// ---- parsing ---------------------------------------------------------------------

// The kinds of the daily set are gone (D55): a line of one is skipped.
TEST_F(V2, RefusesTheSetsKinds) {
  EXPECT_FALSE(stream(R"({"id":"b1","kind":"brief","title":"T","lines":[{"text":"L","read":"r-1"}]})"));
  EXPECT_FALSE(stream(R"({"id":"l1","kind":"letter","title":"T","body":"B"})"));
  EXPECT_FALSE(stream(R"({"id":"a1","kind":"article","title":"T","book":"books/read/r-1.epub"})"));
  EXPECT_FALSE(stream(R"({"id":"g1","kind":"game","game":"sudoku"})"));
  EXPECT_FALSE(stream(R"({"id":"f1","kind":"forecast","text":"T","by":"2026-12-06"})"));
  EXPECT_FALSE(stream(R"({"id":"o1","kind":"outcome","text":"T","p":70,"happened":true,"body":"B"})"));
  EXPECT_FALSE(stream(R"({"id":"q1","kind":"test","prompt":"P","options":["a","b","dk"],"answer":0,"quote":"Q"})"));
  EXPECT_TRUE(stream(R"({"id":"s1","kind":"bite","title":"T","body":"B"})"));
}

TEST_F(V2, ParsesRiddle) {
  ASSERT_TRUE(stream(R"({"id":"q1","kind":"riddle","prompt":"What?","answer":"This.","book":"books/a.epub"})"));
  EXPECT_EQ(item.shape, Shape::Riddle);
  EXPECT_STREQ(at(item.title), "What?");
  EXPECT_STREQ(at(item.body), "This.");
  EXPECT_STREQ(at(item.book), "books/a.epub");
  EXPECT_EQ(kindOf(item.shape).result, Result::Solved);
  EXPECT_STREQ(kindOf(item.shape).name, "riddle");
  EXPECT_FALSE(stream(R"({"id":"q1","kind":"riddle","prompt":"What?"})"));
  EXPECT_TRUE(stream(R"({"id":"q1","kind":"riddle","prompt":")" + std::string(BODY_MAX, 'p') + R"(","answer":"A"})"));
  EXPECT_FALSE(
      stream(R"({"id":"q1","kind":"riddle","prompt":")" + std::string(BODY_MAX + 1, 'p') + R"(","answer":"A"})"));
}

TEST_F(V2, ParsesOrder) {
  ASSERT_TRUE(stream(R"({"id":"o1","kind":"order","prompt":"Sort","items":["a","b","c"],"why":"Because"})"));
  EXPECT_EQ(item.shape, Shape::Order);
  ASSERT_EQ(item.optionCount, 3);
  EXPECT_STREQ(at(item.options[2]), "c");
  EXPECT_STREQ(at(item.why), "Because");
  EXPECT_TRUE(stream(R"({"id":"o1","kind":"order","prompt":"Sort","items":)" + quoted(names(6)) + "}"));
  EXPECT_EQ(item.why.length, 0);
  EXPECT_FALSE(stream(R"({"id":"o1","kind":"order","prompt":"Sort","items":)" + quoted(names(2)) + "}"));
  EXPECT_FALSE(stream(R"({"id":"o1","kind":"order","prompt":"Sort","items":)" + quoted(names(7)) + "}"));
  EXPECT_FALSE(stream(R"({"id":"o1","kind":"order","prompt":"Sort","items":["a","b",")" +
                      std::string(SHORT_MAX + 1, 'c') + "\"]}"));
  EXPECT_FALSE(stream(R"({"id":"o1","kind":"order","prompt":"Sort","items":["a","b",3]})"));
}

TEST_F(V2, ParsesOddAndLogic) {
  ASSERT_TRUE(stream(R"({"id":"d1","kind":"odd","prompt":"Which?","options":["a","b","c"],"answer":2,"why":"W"})"));
  EXPECT_EQ(item.shape, Shape::Odd);
  EXPECT_EQ(item.answer, 2);
  EXPECT_EQ(item.optionCount, 3);
  EXPECT_STREQ(at(item.why), "W");
  EXPECT_TRUE(stream(R"({"id":"d1","kind":"odd","prompt":"P","options":)" + quoted(names(5)) + R"(,"answer":4})"));
  EXPECT_FALSE(stream(R"({"id":"d1","kind":"odd","prompt":"P","options":)" + quoted(names(2)) + R"(,"answer":0})"));
  EXPECT_FALSE(stream(R"({"id":"d1","kind":"odd","prompt":"P","options":)" + quoted(names(6)) + R"(,"answer":0})"));
  EXPECT_FALSE(stream(R"({"id":"d1","kind":"odd","prompt":"P","options":["a","b","c"],"answer":3})"));
  EXPECT_FALSE(stream(R"({"id":"d1","kind":"odd","prompt":"P","options":["a","b","c"]})"));

  const std::string longPrompt(BODY_MAX, 'l');
  ASSERT_TRUE(stream(R"({"id":"g1","kind":"logic","prompt":")" + longPrompt +
                     R"(","options":["yes","no"],"answer":1,"why":"W"})"));
  EXPECT_EQ(item.shape, Shape::Logic);
  EXPECT_EQ(item.title.length, BODY_MAX);
  EXPECT_EQ(item.answer, 1);
  EXPECT_TRUE(stream(R"({"id":"g1","kind":"logic","prompt":"P","options":)" + quoted(names(4)) + R"(,"answer":0})"));
  EXPECT_FALSE(stream(R"({"id":"g1","kind":"logic","prompt":"P","options":)" + quoted(names(5)) + R"(,"answer":0})"));
  EXPECT_FALSE(stream(R"({"id":"g1","kind":"logic","prompt":"P","options":["a"],"answer":0})"));
  EXPECT_FALSE(stream(R"({"id":"g1","kind":"logic","prompt":"P","options":["a","b"],"answer":-1})"));
}

// ---- the book puzzles ------------------------------------------------------------

TEST_F(V2, BookPuzzlesLogResultsInSeen) {
  fake::add("/x4/stream.jsonl",
            R"({"id":"s1","kind":"bite","title":"T","body":"B"})"
            "\n"
            R"({"id":"q1","kind":"riddle","prompt":"P","answer":"A"})"
            "\n"
            R"({"id":"o1","kind":"order","prompt":"P","items":["a","b","c"]})"
            "\n");
  learn::ensureReady();

  learn::Step step;
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBookPuzzle, item, step));
  EXPECT_EQ(item.shape, Shape::Riddle);
  learn::markSolved(item, false, -1, 4200);
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBookPuzzle, item, step));
  EXPECT_EQ(item.shape, Shape::Order);
  learn::markSolved(item, true, -1, 9000);
  // Solved puzzles do not come back, and the bite waits for its book.
  EXPECT_FALSE(learn::loadUnseen(*buffer, &isBookPuzzle, item, step));
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBite, item, step));
  EXPECT_EQ(item.shape, Shape::Bite);

  const auto seen = fileLines(paths::SEEN_LOG);
  ASSERT_EQ(seen.size(), 2u);
  auto riddle = json(seen[0]);
  EXPECT_STREQ(riddle["id"], "q1");
  EXPECT_EQ(riddle["correct"], false);
  EXPECT_EQ(riddle["ms"], 4200);
  EXPECT_EQ(riddle["day"], 1);
  EXPECT_TRUE(riddle["at"].isNull());
  auto order = json(seen[1]);
  EXPECT_EQ(order["correct"], true);
  EXPECT_EQ(order["ms"], 9000);
}

// ---- order -----------------------------------------------------------------------

TEST(Order, ShuffleIsAStablePermutationNeverSolved) {
  std::set<std::vector<uint8_t>> seen;
  for (uint8_t count = ORDER_MIN; count <= ORDER_MAX; ++count) {
    for (int n = 0; n < 50; ++n) {
      const std::string id = "o-" + std::to_string(n);
      uint8_t rows[LIST_MAX];
      orderShuffle(id.c_str(), count, rows);
      std::vector<uint8_t> sorted(rows, rows + count);
      std::sort(sorted.begin(), sorted.end());
      for (uint8_t i = 0; i < count; ++i) EXPECT_EQ(sorted[i], i);
      EXPECT_FALSE(orderSolved(rows, count));
      uint8_t again[LIST_MAX];
      orderShuffle(id.c_str(), count, again);
      EXPECT_TRUE(std::equal(rows, rows + count, again));
      if (count == 4) seen.insert(std::vector<uint8_t>(rows, rows + count));
    }
  }
  // Different ids open differently.
  EXPECT_GT(seen.size(), 8u);
}

TEST(Order, MoveShiftsTheRowsBetween) {
  uint8_t rows[] = {0, 1, 2, 3, 4};
  orderMove(rows, 5, 0, 3);
  EXPECT_EQ(std::vector<uint8_t>(rows, rows + 5), (std::vector<uint8_t>{1, 2, 3, 0, 4}));
  orderMove(rows, 5, 3, 0);
  EXPECT_TRUE(orderSolved(rows, 5));
  orderMove(rows, 5, 4, 5);  // out of range: unchanged
  EXPECT_TRUE(orderSolved(rows, 5));
  orderMove(rows, 5, 2, 3);
  EXPECT_EQ(std::vector<uint8_t>(rows, rows + 5), (std::vector<uint8_t>{0, 1, 3, 2, 4}));
  EXPECT_FALSE(orderSolved(rows, 5));
}

// ---- input -----------------------------------------------------------------------

namespace {

StreamView rowsView(const Shape shape) {
  StreamView v;
  v.phase = StreamPhase::Prompt;
  v.shape = shape;
  return v;
}
StreamKeys key(const bool confirm, const bool next, const bool previous, const bool front) {
  StreamKeys k;
  k.confirm = confirm;
  k.next = next;
  k.previous = previous;
  k.frontNext = next && front;
  k.frontPrevious = previous && front;
  return k;
}
const StreamKeys CONFIRM = key(true, false, false, false);
const StreamKeys SIDE_DOWN = key(false, true, false, false);
const StreamKeys SIDE_UP = key(false, false, true, false);
const StreamKeys RIGHT = key(false, true, false, true);
const StreamKeys LEFT = key(false, false, true, true);

}  // namespace

TEST(StreamV2Input, OrderLiftsCarriesDropsAndChecks) {
  StreamView v = rowsView(Shape::Order);
  EXPECT_EQ(streamMove(v, SIDE_DOWN), StreamMove::SelectNext);
  EXPECT_EQ(streamMove(v, CONFIRM), StreamMove::Lift);
  EXPECT_EQ(streamMove(v, RIGHT), StreamMove::Check);
  EXPECT_EQ(streamMove(v, LEFT), StreamMove::None);

  v.holding = true;
  EXPECT_EQ(streamMove(v, RIGHT), StreamMove::CarryNext);
  EXPECT_EQ(streamMove(v, SIDE_DOWN), StreamMove::CarryNext);
  EXPECT_EQ(streamMove(v, LEFT), StreamMove::None);  // already first
  EXPECT_EQ(streamMove(v, CONFIRM), StreamMove::Drop);
  v.firstOption = false;
  v.lastOption = true;
  EXPECT_EQ(streamMove(v, SIDE_UP), StreamMove::CarryPrevious);
  EXPECT_EQ(streamMove(v, RIGHT), StreamMove::None);

  v = rowsView(Shape::Order);
  v.phase = StreamPhase::Answered;
  EXPECT_EQ(streamMove(v, CONFIRM), StreamMove::Continue);
}

TEST(StreamV2Input, RiddleRevealsThenGotItOrMissed) {
  StreamView v = rowsView(Shape::Riddle);
  EXPECT_EQ(streamMove(v, CONFIRM), StreamMove::Reveal);
  v.morePages = true;
  EXPECT_EQ(streamMove(v, CONFIRM), StreamMove::PageNext);
  v = rowsView(Shape::Riddle);
  v.phase = StreamPhase::Revealed;
  EXPECT_EQ(streamMove(v, CONFIRM), StreamMove::Good);
  EXPECT_EQ(streamMove(v, LEFT), StreamMove::Again);
}

TEST(StreamV2Input, OddAndLogicPickLikeChoice) {
  for (const Shape shape : {Shape::Odd, Shape::Logic}) {
    StreamView v = rowsView(shape);
    EXPECT_EQ(streamMove(v, CONFIRM), StreamMove::Pick);
    EXPECT_EQ(streamMove(v, RIGHT), StreamMove::Pick);
    // A logic prompt over several screens pages before its options.
    v.morePages = true;
    v.selectionVisible = false;
    EXPECT_EQ(streamMove(v, CONFIRM), StreamMove::PageNext);
  }
}

// ---- Read list and read-log --------------------------------------------------------

namespace {

std::string readLine(const std::string& id, const std::string& title, const int minutes) {
  return R"({"id":")" + id + R"(","kind":"news","title":")" + title + R"(","source":"Telex","lang":"hu","minutes":)" +
         std::to_string(minutes) + R"(,"book":"books/read/)" + id + ".epub\"}\n";
}

}  // namespace

TEST_F(V2, ParsesReadEntries) {
  readlist::Entry entry;
  const std::string line = readLine("r-1", "Cím", 4);
  ASSERT_TRUE(readlist::parseEntry(line.data(), line.size() - 1, entry));
  EXPECT_STREQ(entry.title, "Cím");
  EXPECT_STREQ(entry.book, "books/read/r-1.epub");
  EXPECT_EQ(entry.minutes, 4);

  const std::string leading = R"({"id":"r","title":"T","book":"/books/read/r.epub"})";
  ASSERT_TRUE(readlist::parseEntry(leading.data(), leading.size(), entry));
  EXPECT_STREQ(entry.book, "books/read/r.epub");
  EXPECT_EQ(entry.minutes, 0);

  // A long title is cut on a character boundary.
  std::string title;
  while (title.size() < readlist::TITLE_BYTES + 10) title += "é";
  const std::string longTitle = readLine("r-2", title, 1);
  ASSERT_TRUE(readlist::parseEntry(longTitle.data(), longTitle.size() - 1, entry));
  EXPECT_LE(std::strlen(entry.title), readlist::TITLE_BYTES);
  EXPECT_EQ(std::strlen(entry.title) % 2, 0u);

  for (const std::string bad : {R"({"title":"T","book":"books/read/r.epub"})", R"({"id":"r","book":"b.epub"})",
                                R"({"id":"r","title":"T","url":"https://x"})", R"(["r"])", "not json"}) {
    EXPECT_FALSE(readlist::parseEntry(bad.data(), bad.size(), entry)) << bad;
  }
}

TEST_F(V2, ReadListSkipsFinishedMissingAndInvalid) {
  fake::add("/books/read/r-1.epub");
  fake::add("/books/read/r-3.epub");
  fake::add("/books/read/r-4.epub");
  fake::add(paths::READ_LIST, readLine("r-1", "One", 4) + "{bad\n" + readLine("r-2", "Missing", 3) +
                                  readLine("r-3", "Three", 18) + readLine("r-4", "Four", 7));
  readlist::Entry entries[readlist::MAX];
  ASSERT_EQ(readlist::load(*buffer, entries, readlist::MAX), 3);
  EXPECT_STREQ(entries[0].title, "One");
  EXPECT_STREQ(entries[1].title, "Three");
  EXPECT_EQ(entries[1].minutes, 18);
  EXPECT_EQ(readlist::load(*buffer, nullptr, 2), 2);

  // Left half-read: logged, still listed.
  readlist::onBookClosed("/books/read/r-3.epub", 0.4f, false, 60000);
  EXPECT_EQ(readlist::load(*buffer, nullptr, UINT8_MAX), 3);
  // Finished: logged and gone from the list.
  readlist::onBookClosed("/books/read/r-1.epub", 0.96f, false, 120000);
  EXPECT_TRUE(readlist::isFinished("books/read/r-1.epub"));
  ASSERT_EQ(readlist::load(*buffer, entries, readlist::MAX), 2);
  EXPECT_STREQ(entries[0].title, "Three");
  // The Library's Articles tab keeps it, below the unfinished.
  uint8_t unfinished = 0;
  ASSERT_EQ(readlist::loadAll(*buffer, entries, readlist::MAX, unfinished), 3);
  EXPECT_EQ(unfinished, 2);
  EXPECT_STREQ(entries[0].title, "Three");
  EXPECT_STREQ(entries[1].title, "Four");
  EXPECT_STREQ(entries[2].title, "One");
  EXPECT_EQ(readlist::loadAll(*buffer, entries, 2, unfinished), 2);

  // The host pulls the log; the finished state stays on the device.
  fake::files.erase(paths::READ_LOG);
  learn::forgetPowerOn();
  EXPECT_EQ(readlist::load(*buffer, nullptr, UINT8_MAX), 2);
  EXPECT_FALSE(fake::files.count(paths::READ_LOG));

  fake::files.erase(paths::READ_LIST);
  EXPECT_EQ(readlist::load(*buffer, entries, readlist::MAX), 0);
}

TEST_F(V2, ReadListStopsAtItsLimit) {
  std::string list;
  for (int i = 0; i < readlist::MAX + 5; ++i) {
    const std::string id = "r-" + std::to_string(i);
    fake::add("/books/read/" + id + ".epub");
    list += readLine(id, "T" + std::to_string(i), 1);
  }
  fake::add(paths::READ_LIST, list);
  auto entries = std::make_unique<readlist::Entry[]>(readlist::MAX);
  EXPECT_EQ(readlist::load(*buffer, entries.get(), readlist::MAX), readlist::MAX);
  EXPECT_STREQ(entries[readlist::MAX - 1].title, ("T" + std::to_string(readlist::MAX - 1)).c_str());
  EXPECT_EQ(readlist::load(*buffer, nullptr, UINT8_MAX), readlist::MAX + 5);
}

TEST_F(V2, ReadLogWritesTheContractRecord) {
  learn::ensureReady();  // opens set 1, as Today does on the first boot
  readlist::onBookClosed("/books/read/r-7.epub", 0.5f, false, 980000);
  readlist::onBookClosed("/books/read/r-8.epub", 0.6f, true, 1000);
  readlist::onBookClosed("/books/read/r-9.epub", 0.95f, false, 0);
  readlist::onBookClosed("/books/meditations.epub", 1.0f, true, 5000);
  readlist::onBookClosed("/books/reader.epub", 1.0f, true, 5000);
  readlist::onBookClosed("/books/read/", 1.0f, true, 5000);

  const auto log = fileLines(paths::READ_LOG);
  ASSERT_EQ(log.size(), 5u);  // a book's session too (D26); the folder is no book
  auto book = json(log[3]);
  EXPECT_STREQ(book["book"], "books/meditations.epub");
  EXPECT_EQ(book["finished"], true);
  EXPECT_EQ(book["ms"], 5000);
  auto first = json(log[0]);
  EXPECT_STREQ(first["book"], "books/read/r-7.epub");
  EXPECT_NEAR(first["progress"].as<double>(), 0.5, 1e-9);
  EXPECT_EQ(first["finished"], false);
  EXPECT_EQ(first["ms"], 980000);
  EXPECT_EQ(first["day"], 1);
  EXPECT_TRUE(first["at"].isNull());
  auto atEnd = json(log[1]);
  EXPECT_NEAR(atEnd["progress"].as<double>(), 1.0, 1e-9);
  EXPECT_EQ(atEnd["finished"], true);
  auto threshold = json(log[2]);
  EXPECT_EQ(threshold["finished"], true);

  EXPECT_FALSE(readlist::isFinished("books/read/r-7.epub"));
  EXPECT_TRUE(readlist::isFinished("books/read/r-8.epub"));
  EXPECT_TRUE(readlist::isFinished("/books/read/r-9.epub"));
  EXPECT_TRUE(readlist::isReadBook("books/read/x.epub"));
  EXPECT_FALSE(readlist::isReadBook("/books/readme.epub"));
}

TEST_F(V2, FinishedStateKeepsTheNewestWhenFull) {
  for (uint16_t i = 0; i <= readlist::DONE_MAX; ++i) {
    readlist::onBookClosed(("/books/read/r-" + std::to_string(i) + ".epub").c_str(), 1.0f, false, 0);
  }
  EXPECT_FALSE(readlist::isFinished("books/read/r-0.epub"));
  EXPECT_TRUE(readlist::isFinished("books/read/r-1.epub"));
  EXPECT_TRUE(readlist::isFinished(("books/read/r-" + std::to_string(readlist::DONE_MAX) + ".epub").c_str()));
  // Closing a finished article again keeps one entry.
  readlist::onBookClosed("/books/read/r-5.epub", 1.0f, true, 0);
  EXPECT_TRUE(readlist::isFinished("books/read/r-1.epub"));
}

// A piece's forecast: the statement with the day that settles it, over the five rows.
TEST_F(V2, PieceForecastParses) {
  const auto piece = [&](const std::string& forecast, Item& article, Item& out) {
    const std::string line = R"({"id":"r-1","book":"books/read/r-1.epub","title":"T","forecast":)" + forecast + "}";
    std::memcpy(buffer->text, line.data(), line.size());
    char thread[ID_MAX + 1];
    return parsePiece(buffer->text, line.size(), article, out, thread);
  };
  Item article;
  ASSERT_TRUE(piece(R"({"id":"f1","text":"The bill passes.","by":"2026-12-06"})", article, item));
  EXPECT_EQ(item.shape, Shape::Forecast);
  EXPECT_STREQ(item.id, "f1");
  EXPECT_STREQ(at(item.title), "The bill passes.\n\n6 December 2026");
  EXPECT_EQ(item.settles, 20261206u);
  ASSERT_EQ(item.optionCount, FORECAST_ROWS);
  EXPECT_STREQ(at(item.options[0]), "10%");
  EXPECT_STREQ(at(item.options[4]), "90%");
  EXPECT_EQ(forecastPercent(FORECAST_START), 50);
  EXPECT_FALSE(piece(R"({"id":"f1","text":"T","by":"December"})", article, item));
  EXPECT_FALSE(piece(R"({"id":"f1","text":"T","by":"2026-13-01"})", article, item));
  EXPECT_FALSE(piece(R"({"id":"f1","by":"2026-12-06"})", article, item));
}
