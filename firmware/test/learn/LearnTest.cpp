#include <gtest/gtest.h>

#include <cstring>
#include <tuple>
#include <memory>
#include <string>
#include <vector>

#include "BookStats.h"
#include "DeckStore.h"
#include "Fsrs.h"
#include "HalStorage.h"
#include "Item.h"
#include "Journal.h"
#include "JsonlReader.h"
#include "Learn.h"
#include "LearnPaths.h"
#include "Visits.h"
#include "SetRules.h"
#include "StateFile.h"

using namespace x4;

// ---- platform for x4::learn -------------------------------------------------

namespace {
int64_t testNow = 0;
uint32_t testMillis = 0;
bool testHostClock = false;
int32_t testTz = 0;
}  // namespace

namespace x4::learn {
int64_t platformNow() { return testNow; }
uint32_t platformMillis() { return testMillis += 1000; }
void addBookStats(JsonObject) {}
void addPlayStats(JsonObject) {}
bool platformHostClock(int32_t& tz) {
  if (testHostClock) tz = testTz;
  return testHostClock;
}
}  // namespace x4::learn

namespace {

std::string contents(const std::string& path) {
  const auto it = fake::files.find(path);
  if (it == fake::files.end()) return {};
  return {it->second->bytes.begin(), it->second->bytes.end()};
}

std::vector<std::string> lines(const std::string& path) {
  std::vector<std::string> out;
  std::string all = contents(path);
  size_t start = 0;
  while (start < all.size()) {
    const size_t end = all.find('\n', start);
    out.push_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return out;
}

JsonDocument parse(const std::string& line) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, line));
  return doc;
}

std::string flip(const std::string& id, const std::string& front = "Front", const std::string& back = "Back") {
  return R"({"id":")" + id + R"(","type":"flip","front":")" + front + R"(","back":")" + back + R"("})";
}

std::string deckFile(const std::vector<std::string>& cards) {
  std::string out = R"({"deck":"d","title":"Deck","book":"books/a.epub"})"
                    "\n";
  for (const auto& card : cards) out += card + "\n";
  return out;
}

struct Fresh : ::testing::Test {
  void SetUp() override {
    fake::reset();
    learn::forgetPowerOn();
    testNow = 0;
    testMillis = 0;
    testHostClock = false;
    testTz = 0;
    buffer = std::make_unique<LineBuffer>();
  }
  std::unique_ptr<LineBuffer> buffer;
};

}  // namespace

// ---- FSRS -------------------------------------------------------------------

TEST(Fsrs, InitialMemoryUsesDefaultWeights) {
  const auto good = fsrs::initial(fsrs::Grade::Good);
  EXPECT_NEAR(good.stability, 2.3065f, 1e-4f);
  EXPECT_NEAR(good.difficulty, 2.118f, 1e-2f);
  const auto again = fsrs::initial(fsrs::Grade::Again);
  EXPECT_NEAR(again.stability, 0.212f, 1e-4f);
  EXPECT_NEAR(again.difficulty, 6.4133f, 1e-3f);
  EXPECT_LT(fsrs::initial(fsrs::Grade::Hard).stability, fsrs::initial(fsrs::Grade::Good).stability);
  EXPECT_LT(fsrs::initial(fsrs::Grade::Good).stability, fsrs::initial(fsrs::Grade::Easy).stability);
}

TEST(Fsrs, RetrievabilityIsNinetyPercentAfterOneStability) {
  EXPECT_NEAR(fsrs::retrievability(10.0f, 10.0f), 0.9f, 1e-4f);
  EXPECT_FLOAT_EQ(fsrs::retrievability(0.0f, 10.0f), 1.0f);
  EXPECT_GT(fsrs::retrievability(5.0f, 10.0f), fsrs::retrievability(20.0f, 10.0f));
  EXPECT_EQ(fsrs::intervalFor(10.0f), 10u);
  EXPECT_EQ(fsrs::intervalFor(0.212f), 1u);
  EXPECT_EQ(fsrs::intervalFor(1e9f), fsrs::MAX_INTERVAL);
}

TEST(Fsrs, IntervalsGrowWithEachGoodReview) {
  CardRecord card{};
  uint32_t set = 1;
  uint32_t previous = 0;
  for (int i = 0; i < 6; ++i) {
    fsrs::schedule(card, set, fsrs::Grade::Good);
    const uint32_t interval = card.dueDay - set;
    EXPECT_GT(interval, previous) << "review " << i;
    previous = interval;
    set = card.dueDay;
  }
  EXPECT_EQ(card.reps, 6);
  EXPECT_EQ(card.lapses, 0);
  EXPECT_GT(previous, 30u);
}

TEST(Fsrs, LapseShrinksStabilityAndCounts) {
  CardRecord card{};
  fsrs::schedule(card, 1, fsrs::Grade::Good);
  fsrs::schedule(card, card.dueDay, fsrs::Grade::Good);
  const float before = card.stability;
  const float difficultyBefore = card.difficulty;
  const uint32_t set = card.dueDay;
  fsrs::schedule(card, set, fsrs::Grade::Again);
  EXPECT_LT(card.stability, before);
  EXPECT_GT(card.difficulty, difficultyBefore);
  EXPECT_EQ(card.lapses, 1);
  EXPECT_LE(card.dueDay - set, 2u);
  EXPECT_EQ(card.lastDay, set);
}

TEST(Fsrs, GradesOrderRecallStability) {
  const fsrs::Memory m{10.0f, 5.0f};
  const float again = fsrs::next(m, 10.0f, fsrs::Grade::Again).stability;
  const float hard = fsrs::next(m, 10.0f, fsrs::Grade::Hard).stability;
  const float good = fsrs::next(m, 10.0f, fsrs::Grade::Good).stability;
  const float easy = fsrs::next(m, 10.0f, fsrs::Grade::Easy).stability;
  EXPECT_LT(again, hard);
  EXPECT_LT(hard, good);
  EXPECT_LT(good, easy);
  EXPECT_GT(hard, 10.0f);
}

TEST(Fsrs, RecordRetrievabilityOfNewCardIsZero) {
  CardRecord card{};
  EXPECT_EQ(fsrs::recordRetrievability(card, 5), 0.0f);
  fsrs::schedule(card, 5, fsrs::Grade::Good);
  EXPECT_FLOAT_EQ(fsrs::recordRetrievability(card, 5), 1.0f);
  EXPECT_LT(fsrs::recordRetrievability(card, 9), 0.9f);
}

// ---- JSONL reader -----------------------------------------------------------

TEST_F(Fresh, JsonlReaderHandlesCrlfBlankAndBom) {
  fake::add("/f.jsonl", "\xEF\xBB\xBF{\"a\":1}\r\n\r\n   \n{\"b\":2}\n{\"c\":3}");
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", "/f.jsonl", file));
  JsonlReader reader(file, *buffer);
  size_t length = 0;
  uint32_t offset = 0;
  ASSERT_EQ(reader.next(length, offset), JsonlReader::Line::Ok);
  EXPECT_STREQ(buffer->text, "{\"a\":1}");
  EXPECT_EQ(offset, 3u);
  ASSERT_EQ(reader.next(length, offset), JsonlReader::Line::Ok);
  EXPECT_STREQ(buffer->text, "{\"b\":2}");
  const uint32_t bOffset = offset;
  ASSERT_EQ(reader.next(length, offset), JsonlReader::Line::Ok);
  EXPECT_STREQ(buffer->text, "{\"c\":3}");
  EXPECT_EQ(reader.next(length, offset), JsonlReader::Line::End);
  file.close();

  ASSERT_TRUE(readLineAt("/f.jsonl", bOffset, *buffer, length));
  EXPECT_STREQ(buffer->text, "{\"b\":2}");
  EXPECT_EQ(length, 7u);
}

TEST_F(Fresh, JsonlReaderSkipsOverlongLines) {
  std::string data = "{\"a\":1}\n" + std::string(JSONL_LINE_MAX + 100, 'x') + "\n{\"b\":2}\n";
  fake::add("/f.jsonl", data);
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", "/f.jsonl", file));
  JsonlReader reader(file, *buffer);
  size_t length = 0;
  uint32_t offset = 0;
  EXPECT_EQ(reader.next(length, offset), JsonlReader::Line::Ok);
  EXPECT_EQ(reader.next(length, offset), JsonlReader::Line::TooLong);
  ASSERT_EQ(reader.next(length, offset), JsonlReader::Line::Ok);
  EXPECT_STREQ(buffer->text, "{\"b\":2}");
  EXPECT_EQ(offset, data.size() - 8);
}

TEST_F(Fresh, JsonlReaderKeepsLineJustUnderLimit) {
  const std::string longest(JSONL_LINE_MAX - 1, 'y');
  fake::add("/f.jsonl", longest + "\n");
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("T", "/f.jsonl", file));
  JsonlReader reader(file, *buffer);
  size_t length = 0;
  uint32_t offset = 0;
  ASSERT_EQ(reader.next(length, offset), JsonlReader::Line::Ok);
  EXPECT_EQ(length, JSONL_LINE_MAX - 1);
}

// ---- card parsing ------------------------------------------------------------

namespace {
bool parses(const std::string& line, Item& item, LineBuffer& buffer) {
  std::memcpy(buffer.text, line.data(), line.size());
  buffer.text[line.size()] = '\0';
  return parseCard(buffer.text, line.size(), item);
}
}  // namespace

TEST_F(Fresh, ParsesAllFourCardTypes) {
  Item item;
  ASSERT_TRUE(parses(flip("c1", "Q", "A"), item, *buffer));
  EXPECT_EQ(item.shape, Shape::Flip);
  EXPECT_STREQ(item.id, "c1");
  EXPECT_STREQ(text(buffer->text, item.title), "Q");
  EXPECT_STREQ(text(buffer->text, item.body), "A");

  ASSERT_TRUE(parses(R"({"id":"c2","type":"choice","prompt":"P","options":["a","b","c"],"answer":1,"why":"W"})", item,
                     *buffer));
  EXPECT_EQ(item.shape, Shape::Choice);
  EXPECT_EQ(item.optionCount, 3);
  EXPECT_EQ(item.answer, 1);
  EXPECT_STREQ(text(buffer->text, item.options[1]), "b");
  EXPECT_STREQ(text(buffer->text, item.why), "W");

  const std::string cloze =
      R"({"id":"c3","type":"cloze","text":"The {{impediment}} to action.","options":["fear","impediment"]})";
  ASSERT_TRUE(parses(cloze, item, *buffer));
  EXPECT_EQ(item.shape, Shape::Cloze);
  EXPECT_EQ(item.answer, 1);
  EXPECT_STREQ(text(buffer->text, item.title), "The ______ to action.");
  EXPECT_STREQ(text(buffer->text, item.body), "The impediment to action.");

  ASSERT_TRUE(parses(R"({"id":"c4","type":"explain","prompt":"Explain","points":["one","two"]})", item, *buffer));
  EXPECT_EQ(item.shape, Shape::Explain);
  EXPECT_EQ(item.optionCount, 2);
  EXPECT_STREQ(text(buffer->text, item.options[0]), "one");
}

TEST_F(Fresh, RejectsCardsOutsideTheContract) {
  Item item;
  const std::string tooLong(TEXT_MAX + 1, 'a');
  const std::string longest(TEXT_MAX, 'a');
  const std::vector<std::string> bad = {
      R"({"id":"c","type":"poem","front":"a","back":"b"})",
      R"({"id":"c","type":"flip","front":"a"})",
      R"({"type":"flip","front":"a","back":"b"})",
      R"({"id":"123456789012345678901234567890123","type":"flip","front":"a","back":"b"})",
      flip("c", tooLong),
      R"({"id":"c","type":"choice","prompt":"p","options":["a"],"answer":0})",
      R"({"id":"c","type":"choice","prompt":"p","options":["a","b","c","d","e"],"answer":0})",
      R"({"id":"c","type":"choice","prompt":"p","options":["a","b"],"answer":2})",
      R"({"id":"c","type":"choice","prompt":"p","options":["a","b"]})",
      R"({"id":"c","type":"cloze","text":"no term","options":["a","b"]})",
      R"({"id":"c","type":"cloze","text":"{{a}} and {{b}}","options":["a","b"]})",
      R"({"id":"c","type":"cloze","text":"the {{x}}","options":["a","b"]})",
      R"({"id":"c","type":"explain","prompt":"p","points":[]})",
      R"({"id":"c","type":"explain","prompt":"p","points":["1","2","3","4","5","6"]})",
      R"({"id":"c","type":"flip","front":"a","back":"b","why":")" + tooLong + R"("})",
      R"({"id":"c","type":"flip","front":"a",)",
      "not json",
  };
  for (const auto& line : bad) EXPECT_FALSE(parses(line, item, *buffer)) << line.substr(0, 120);
  EXPECT_TRUE(parses(flip("12345678901234567890123456789012", longest, longest), item, *buffer));
  EXPECT_TRUE(parses(R"({"id":"c","type":"explain","prompt":"p","points":["1","2","3","4","5"]})", item, *buffer));
}

TEST_F(Fresh, ParsesStreamItems) {
  Item item;
  auto stream = [&](const std::string& line) {
    std::memcpy(buffer->text, line.data(), line.size());
    return parseStreamItem(buffer->text, line.size(), item);
  };
  ASSERT_TRUE(stream(R"({"id":"s1","kind":"bite","title":"T","body":"B","book":"books/a.epub","highlight":"h1"})"));
  EXPECT_EQ(item.shape, Shape::Bite);
  EXPECT_STREQ(text(buffer->text, item.body), "B");
  ASSERT_TRUE(stream(R"({"id":"s2","kind":"highlight","highlight":"h-1a2b"})"));
  EXPECT_EQ(item.shape, Shape::Highlight);
  EXPECT_STREQ(item.highlightId, "h-1a2b");
  EXPECT_EQ(item.body.length, 0);
  EXPECT_FALSE(stream(R"({"id":"s3","kind":"article","title":"A","book":"books/x.epub"})"));
  EXPECT_FALSE(stream(R"({"id":"s4","kind":"letter","title":"Week","body":"Dear"})"));
  EXPECT_FALSE(stream(R"({"id":"s5","kind":"video","title":"V"})"));
  EXPECT_FALSE(stream(R"({"id":"s6","kind":"bite","title":"T","body":")" + std::string(BODY_MAX + 1, 'b') + "\"}"));
}

// ---- card state store ----------------------------------------------------------

TEST_F(Fresh, DeckStateRoundTripKeepsHistoryByCardKey) {
  fake::add("/x4/decks/stoic.jsonl", deckFile({flip("c1"), flip("c2"), "garbage", flip("c3")}));
  decks::DeckId ids[decks::MAX_DECKS];
  ASSERT_EQ(decks::list(ids, decks::MAX_DECKS), 1);
  EXPECT_STREQ(ids[0].id, "stoic");
  EXPECT_TRUE(decks::sync(ids, 1, *buffer, false));
  EXPECT_FALSE(decks::sync(ids, 1, *buffer, false));

  CardRecord record{};
  ASSERT_TRUE(decks::readRecord("stoic", 1, record));
  EXPECT_EQ(record.key, cardKeyHash("stoic", "c2"));
  fsrs::schedule(record, 3, fsrs::Grade::Good);
  ASSERT_TRUE(decks::writeRecord("stoic", 1, record));

  // The host rewrites the deck: c2 moves, c1 is retired, c4 is new.
  fake::add("/x4/decks/stoic.jsonl", deckFile({flip("c4"), flip("c3"), flip("c2", "Edited")}), 2);
  EXPECT_TRUE(decks::sync(ids, 1, *buffer, false));
  uint8_t chunk[READ_CHUNK];
  decks::RecordCursor cursor("stoic", chunk, sizeof(chunk));
  ASSERT_TRUE(cursor.ok());
  EXPECT_EQ(cursor.count(), 3u);
  std::vector<CardRecord> records;
  uint16_t index = 0;
  while (cursor.next(record, index)) records.push_back(record);
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].key, cardKeyHash("stoic", "c4"));
  EXPECT_EQ(records[0].reps, 0);
  EXPECT_EQ(records[2].key, cardKeyHash("stoic", "c2"));
  EXPECT_EQ(records[2].reps, 1);
  EXPECT_EQ(records[2].lastDay, 3u);

  // Same size and time, new content: only a deep sync notices.
  std::string same = contents("/x4/decks/stoic.jsonl");
  same[same.find("Edited")] = 'X';
  fake::add("/x4/decks/stoic.jsonl", same, 2);
  EXPECT_FALSE(decks::sync(ids, 1, *buffer, false));
  EXPECT_TRUE(decks::sync(ids, 1, *buffer, true));

  // A deck that disappears takes its state with it.
  fake::files.erase("/x4/decks/stoic.jsonl");
  EXPECT_EQ(decks::list(ids, decks::MAX_DECKS), 0);
  EXPECT_TRUE(decks::sync(ids, 0, *buffer, false));
  EXPECT_FALSE(Storage.exists("/x4/state/cards/stoic.bin"));
}

TEST_F(Fresh, DeckListSkipsBadIdsAndHiddenFiles) {
  fake::add("/x4/decks/b.jsonl", deckFile({flip("c1")}));
  fake::add("/x4/decks/a-1.jsonl", deckFile({flip("c1")}));
  fake::add("/x4/decks/._a.jsonl", "junk");
  fake::add("/x4/decks/bad id.jsonl", deckFile({flip("c1")}));
  fake::add("/x4/decks/notes.txt", "x");
  decks::DeckId ids[decks::MAX_DECKS];
  ASSERT_EQ(decks::list(ids, decks::MAX_DECKS), 2);
  EXPECT_STREQ(ids[0].id, "a-1");
  EXPECT_STREQ(ids[1].id, "b");
}

TEST_F(Fresh, DeckListPastCapacityKeepsFirstNamesAndTheirState) {
  for (const char* id : {"c", "a", "d", "b"}) fake::add(std::string("/x4/decks/") + id + ".jsonl", deckFile({flip("x")}));
  decks::DeckId all[4];
  ASSERT_EQ(decks::list(all, 4), 4);
  ASSERT_TRUE(decks::sync(all, 4, *buffer, false));
  CardRecord record{};
  ASSERT_TRUE(decks::readRecord("d", 0, record));
  fsrs::schedule(record, 1, fsrs::Grade::Good);
  ASSERT_TRUE(decks::writeRecord("d", 0, record));

  decks::DeckId ids[2];
  ASSERT_EQ(decks::list(ids, 2), 2);
  EXPECT_STREQ(ids[0].id, "a");
  EXPECT_STREQ(ids[1].id, "b");
  decks::sync(ids, 2, *buffer, false);
  // Left out, but its deck file is still there: the history stays.
  ASSERT_TRUE(decks::readRecord("d", 0, record));
  EXPECT_EQ(record.reps, 1);

  fake::files.erase("/x4/decks/d.jsonl");
  EXPECT_TRUE(decks::sync(ids, 2, *buffer, false));
  EXPECT_FALSE(Storage.exists("/x4/state/cards/d.bin"));
  EXPECT_TRUE(Storage.exists("/x4/state/cards/c.bin"));

  // The directory walk meets "a-1.jsonl" before "a.jsonl"; name order puts "a" first.
  fake::add("/x4/decks/a-1.jsonl", deckFile({flip("x")}));
  ASSERT_EQ(decks::list(ids, 1), 1);
  EXPECT_STREQ(ids[0].id, "a");
}

TEST_F(Fresh, DeckStateComesBackFromTmpAfterPowerCut) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a"), flip("b")}));
  decks::DeckId ids[decks::MAX_DECKS];
  ASSERT_EQ(decks::list(ids, decks::MAX_DECKS), 1);
  ASSERT_TRUE(decks::sync(ids, 1, *buffer, false));
  CardRecord record{};
  ASSERT_TRUE(decks::readRecord("d", 1, record));
  fsrs::schedule(record, 1, fsrs::Grade::Good);
  ASSERT_TRUE(decks::writeRecord("d", 1, record));

  // Power cut inside replaceFile: the state file is gone, the complete tmp is left.
  ASSERT_TRUE(Storage.rename("/x4/state/cards/d.bin", "/x4/state/cards/d.bin.tmp"));
  EXPECT_FALSE(decks::sync(ids, 1, *buffer, false));
  EXPECT_FALSE(Storage.exists("/x4/state/cards/d.bin.tmp"));
  ASSERT_TRUE(decks::readRecord("d", 1, record));
  EXPECT_EQ(record.reps, 1);

  // A deck change goes through rebuild, which recovers before it writes the tmp.
  ASSERT_TRUE(Storage.rename("/x4/state/cards/d.bin", "/x4/state/cards/d.bin.tmp"));
  fake::add("/x4/decks/d.jsonl", deckFile({flip("c"), flip("b")}), 2);
  EXPECT_TRUE(decks::sync(ids, 1, *buffer, false));
  ASSERT_TRUE(decks::readRecord("d", 1, record));
  EXPECT_EQ(record.key, cardKeyHash("d", "b"));
  EXPECT_EQ(record.reps, 1);
}

TEST_F(Fresh, TornDeckStateTmpIsNotAdopted) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a"), flip("b")}));
  // A first rebuild cut short after its placeholder header (magic 0).
  fake::add("/x4/state/cards/d.bin.tmp", std::string(sizeof(decks::StateHeader), '\0'));
  EXPECT_FALSE(decks::recoverState("d"));
  EXPECT_FALSE(Storage.exists("/x4/state/cards/d.bin"));
  EXPECT_TRUE(decks::rebuild("d", *buffer));
  uint8_t chunk[READ_CHUNK];
  decks::RecordCursor cursor("d", chunk, sizeof(chunk));
  EXPECT_EQ(cursor.count(), 2u);
}

TEST_F(Fresh, DeckStopsAtCardLimit) {
  std::vector<std::string> cards;
  for (int i = 0; i < DECK_CARDS_MAX + 5; ++i) cards.push_back(flip("c" + std::to_string(i)));
  fake::add("/x4/decks/big.jsonl", deckFile(cards));
  ASSERT_TRUE(decks::rebuild("big", *buffer));
  uint8_t chunk[READ_CHUNK];
  decks::RecordCursor cursor("big", chunk, sizeof(chunk));
  EXPECT_EQ(cursor.count(), DECK_CARDS_MAX);
}

// ---- the day -----------------------------------------------------------------

TEST(SetRules, TheFirstDayIsOneAndEachDayStartsAfresh) {
  LearnState s{};
  EXPECT_TRUE(openFirstDay(s));
  EXPECT_EQ(s.day, 1u);
  EXPECT_FALSE(openFirstDay(s));
  s.newToday = 4;
  startDay(s, 2);
  EXPECT_EQ(s.day, 2u);
  EXPECT_EQ(s.newToday, 0);
}

TEST(SetRules, CalendarDayFollowsTimezone) {
  // 2026-11-03 23:30 UTC is already 2026-11-04 in UTC+1.
  const int64_t t = 1793748600;
  EXPECT_EQ(civilDay(t, 60), civilDay(t, 0) + 1);
  EXPECT_EQ(civilDay(-1, 0), -1);
}

// ---- the cards ---------------------------------------------------------------

namespace {

// A day passes without a clock (D1): twenty minutes in the reader, in another book, then a power-on.
void nextDay() {
  learn::addReading("/books/elsewhere.epub", DAY_READ_MS, 0);
  learn::forgetPowerOn();
  learn::ensureReady();
}

// Answers the questions of deckFile()'s book, as the ends of its chapters do. Returns how many.
size_t answerCards(LineBuffer& buffer, const fsrs::Grade grade = fsrs::Grade::Good) {
  size_t answered = 0;
  Item item;
  learn::Step step;
  uint16_t left = 0;
  while (answered < 1000 && learn::loadQuestion("/books/a.epub", buffer, item, step, left)) {
    learn::answerQuestion(step, item, {grade}, 1200);
    ++answered;
  }
  return answered;
}

}  // namespace

TEST_F(Fresh, TheFirstPowerOnCountsTheCards) {
  std::vector<std::string> cards;
  for (int i = 0; i < 14; ++i) cards.push_back(flip("c" + std::to_string(i)));
  fake::add("/x4/decks/stoic.jsonl", deckFile(cards));
  learn::ensureReady();
  EXPECT_EQ(learn::state().day, 1u);
  auto stats = parse(contents(paths::STATS));
  EXPECT_EQ(stats["day"], 1);
  EXPECT_TRUE(stats["streak"].isNull());
  EXPECT_EQ(stats["cards"], 14);
  EXPECT_EQ(stats["due"], 0);
  EXPECT_TRUE(stats["retention"].isNull());
  EXPECT_FALSE(Storage.exists("/x4/state/queue.bin"));
}

TEST_F(Fresh, NewCardsComeTenADay) {
  std::vector<std::string> cards;
  for (int i = 0; i < 14; ++i) cards.push_back(flip("c" + std::to_string(i)));
  fake::add("/x4/decks/stoic.jsonl", deckFile(cards));
  learn::ensureReady();
  EXPECT_EQ(answerCards(*buffer), NEW_PER_DAY);
  const auto reviews = lines(paths::REVIEWS);
  ASSERT_EQ(reviews.size(), NEW_PER_DAY);
  auto review = parse(reviews[0]);
  EXPECT_STREQ(review["card"], "stoic/c0");
  EXPECT_STREQ(review["type"], "flip");
  EXPECT_EQ(review["grade"], 3);
  EXPECT_TRUE(review["correct"].isNull());
  EXPECT_EQ(review["ms"], 1200);
  EXPECT_EQ(review["day"], 1);
  EXPECT_TRUE(review["at"].isNull());
  // The answers reach stats.json at the next flush.
  learn::flushStats();
  EXPECT_NEAR(parse(contents(paths::STATS))["retention"].as<double>(), 1.0, 1e-6);

  // The next day has the four left.
  nextDay();
  EXPECT_EQ(answerCards(*buffer), 4u);
}

namespace {

std::string deckFor(const std::string& id, const std::string& book, const std::vector<std::string>& cards) {
  std::string out = R"({"deck":")" + id + R"(","title":"T")";
  if (!book.empty()) out += R"(,"book":")" + book + R"(")";
  out += "}\n";
  for (const auto& card : cards) out += card + "\n";
  return out;
}

// Makes card `index` of `deck` due in set 1 with this stability.
void makeDue(const char* deck, const uint16_t index, const float stability) {
  CardRecord record{};
  ASSERT_TRUE(decks::readRecord(deck, index, record));
  record.reps = 1;
  record.difficulty = 5.0f;
  record.stability = stability;
  record.lastDay = 0;
  record.dueDay = 1;
  ASSERT_TRUE(decks::writeRecord(deck, index, record));
}

// Asks every question for `book`, answering Good. Returns the card ids in order.
std::string askAll(const char* book, LineBuffer& buffer) {
  std::string asked;
  Item item;
  learn::Step step;
  uint16_t left = 0;
  while (asked.size() < 100 && learn::loadQuestion(book, buffer, item, step, left)) {
    asked += item.id;
    learn::answerQuestion(step, item, {fsrs::Grade::Good}, 500);
  }
  return asked;
}

}  // namespace

TEST_F(Fresh, QuestionsComeFromTheBookDueFirst) {
  fake::add("/books/a.epub");
  fake::add("/x4/decks/a.jsonl", deckFor("a", "books/a.epub", {flip("n"), flip("s"), flip("w")}));
  fake::add("/x4/decks/b.jsonl", deckFor("b", "books/b.epub", {flip("x")}));
  fake::add("/x4/decks/t.jsonl", deckFor("t", "", {flip("t")}));
  fake::add("/x4/stream.jsonl", R"({"id":"s1","kind":"bite","title":"T","body":"B"})"
                                "\n");
  learn::ensureReady();
  makeDue("a", 1, 20.0f);
  makeDue("a", 2, 1.0f);

  EXPECT_TRUE(learn::hasQuestion("/books/a.epub"));
  EXPECT_FALSE(learn::hasQuestion("/books/c.epub"));
  Item item;
  learn::Step step;
  uint16_t left = 0;
  ASSERT_TRUE(learn::loadQuestion("/books/a.epub", *buffer, item, step, left));
  EXPECT_STREQ(item.id, "w");  // the weakest due card
  EXPECT_EQ(left, 3);
  // Book cards only: "x" is another book's, "t" waits for an article.
  EXPECT_EQ(askAll("/books/a.epub", *buffer), "wsn");
  EXPECT_FALSE(learn::hasQuestion("/books/a.epub"));
  EXPECT_EQ(lines(paths::REVIEWS).size(), 3u);
}

TEST_F(Fresh, ArticlesAlsoAskTheCardsOfNoBook) {
  fake::add("/books/read/r-1.epub");
  fake::add("/x4/decks/r.jsonl", deckFor("r", "books/read/r-1.epub", {flip("r")}));
  fake::add("/x4/decks/t.jsonl", deckFor("t", "", {flip("t")}));
  fake::add("/x4/decks/u.jsonl", deckFor("u", "books/u.epub", {flip("u")}));
  fake::add("/x4/stream.jsonl", R"({"id":"s1","kind":"bite","title":"T","body":"B"})"
                                "\n");
  learn::ensureReady();
  makeDue("t", 0, 1.0f);
  makeDue("r", 0, 30.0f);
  makeDue("u", 0, 1.0f);
  // The article's own card first, though the bookless one is weaker.
  EXPECT_EQ(askAll("/books/read/r-1.epub", *buffer), "rt");
  EXPECT_FALSE(learn::hasQuestion("/books/u2.epub"));
}

TEST_F(Fresh, AgainComesBackTheNextDay) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a"), flip("b")}));
  learn::ensureReady();
  Item item;
  learn::Step step;
  uint16_t left = 0;
  ASSERT_TRUE(learn::loadQuestion("/books/a.epub", *buffer, item, step, left));
  EXPECT_STREQ(item.id, "a");
  learn::answerQuestion(step, item, {fsrs::Grade::Again}, 100);
  EXPECT_EQ(answerCards(*buffer), 1u);

  // Day 2: only "a" (Again, interval 1) is due; "b" waits for day 3.
  nextDay();
  EXPECT_LT(learn::state().retention, 1.0f);
  ASSERT_TRUE(learn::loadQuestion("/books/a.epub", *buffer, item, step, left));
  EXPECT_STREQ(item.id, "a");
  EXPECT_EQ(left, 1);
}

TEST_F(Fresh, DueCardsComeWeakestFirst) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("strong"), flip("weak")}));
  decks::DeckId ids[decks::MAX_DECKS];
  ASSERT_EQ(decks::list(ids, decks::MAX_DECKS), 1);
  ASSERT_TRUE(decks::sync(ids, 1, *buffer, false));
  for (uint16_t i = 0; i < 2; ++i) {
    CardRecord record{};
    ASSERT_TRUE(decks::readRecord("d", i, record));
    record.reps = 1;
    record.difficulty = 5.0f;
    record.stability = i == 0 ? 20.0f : 1.0f;
    record.lastDay = 0;
    record.dueDay = 1;
    ASSERT_TRUE(decks::writeRecord("d", i, record));
  }
  learn::ensureReady();
  Item item;
  learn::Step step;
  uint16_t left = 0;
  ASSERT_TRUE(learn::loadQuestion("/books/a.epub", *buffer, item, step, left));
  EXPECT_STREQ(item.id, "weak");
  EXPECT_EQ(learn::state().due, 2u);
  EXPECT_NEAR(learn::state().retention, (fsrs::retrievability(1, 20) + fsrs::retrievability(1, 1)) / 2, 1e-3);
}

TEST_F(Fresh, HostDataChangeRecountsTheCards) {
  learn::ensureReady();
  EXPECT_EQ(learn::state().cards, 0u);
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a"), flip("b")}));
  learn::ensureReady();  // no new power-on and no hook: nothing changes
  EXPECT_EQ(learn::state().cards, 0u);
  learn::onHostDataChanged();
  learn::ensureReady();
  EXPECT_EQ(learn::state().cards, 2u);
  EXPECT_EQ(learn::state().day, 1u);
}

TEST_F(Fresh, HostChangeSurvivesPowerOffUntilTheRebuild) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a")}));
  learn::ensureReady();
  // Same size, same (constant FAT) time: only the content tells it apart.
  fake::add("/x4/decks/d.jsonl", deckFile({flip("b")}));
  learn::onHostDataChanged();
  EXPECT_TRUE(Storage.exists(paths::HOST_CHANGED));

  // No room for the build: the change stays pending, on the card too.
  learn::forgetPowerOn();
  fake::failAlloc = 0;
  learn::ensureReady();
  EXPECT_TRUE(Storage.exists(paths::HOST_CHANGED));

  learn::forgetPowerOn();
  learn::ensureReady();
  EXPECT_FALSE(Storage.exists(paths::HOST_CHANGED));
  Item item;
  learn::Step step;
  uint16_t left = 0;
  ASSERT_TRUE(learn::loadQuestion("/books/a.epub", *buffer, item, step, left));
  EXPECT_STREQ(item.id, "b");
}

TEST_F(Fresh, BitesSkipSeenItemsAndResolveHighlights) {
  fake::add("/x4/highlights.jsonl",
            R"({"id":"h1","kind":"highlight","book":"books/m.epub","title":"Meditations","text":"Mind over events."})"
            "\n");
  fake::add("/x4/stream.jsonl",
            R"({"id":"s1","kind":"highlight","highlight":"h1"})"
            "\n"
            R"({"id":"s2","kind":"highlight","highlight":"missing"})"
            "\n"
            R"({"id":"s5","kind":"highlight","highlight":"h9","text":"Inline","title":"Book"})"
            "\n");
  learn::ensureReady();
  Item item;
  learn::Step step;
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBite, item, step));
  EXPECT_EQ(item.shape, Shape::Highlight);
  EXPECT_STREQ(item.id, "s1");
  EXPECT_STREQ(text(buffer->text, item.body), "Mind over events.");
  EXPECT_STREQ(text(buffer->text, item.title), "Meditations");
  learn::markSeen(item);
  // A highlight whose text is gone is passed over.
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBite, item, step));
  EXPECT_STREQ(item.id, "s5");
  EXPECT_STREQ(text(buffer->text, item.body), "Inline");

  // The host replaces the stream: the seen item stays out, new ones come in.
  fake::add("/x4/stream.jsonl",
            R"({"id":"s1","kind":"highlight","highlight":"h1"})"
            "\n"
            R"({"id":"s6","kind":"bite","title":"Week","body":"Dear"})"
            "\n",
            5);
  learn::onHostDataChanged();
  learn::ensureReady();
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBite, item, step));
  EXPECT_STREQ(item.id, "s6");
}

TEST_F(Fresh, BitesAndPuzzlesComeInFileOrder) {
  fake::add("/x4/stream.jsonl",
            R"({"id":"b1","kind":"bite","title":"T","body":"B"})"
            "\n"
            R"({"id":"q1","kind":"riddle","prompt":"R","answer":"A"})"
            "\n"
            R"({"id":"b2","kind":"bite","title":"T","body":"B"})"
            "\n"
            R"({"id":"q2","kind":"riddle","prompt":"R","answer":"A"})"
            "\n");
  learn::ensureReady();
  Item item;
  learn::Step step;
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBite, item, step));
  EXPECT_STREQ(item.id, "b1");
  learn::markSeen(item);
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBite, item, step));
  EXPECT_STREQ(item.id, "b2");

  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBookPuzzle, item, step));
  EXPECT_STREQ(item.id, "q1");
  learn::markSolved(item, true, -1, 10);
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBookPuzzle, item, step));
  EXPECT_STREQ(item.id, "q2");
  learn::markSolved(item, false, -1, 10);
  EXPECT_FALSE(learn::hasUnseen(&isBookPuzzle));
  EXPECT_TRUE(learn::hasUnseen(&isBite));

  EXPECT_EQ(lines(paths::SEEN_LOG).size(), 3u);
}

TEST_F(Fresh, TheBiteShowsOnceADay) {
  EXPECT_TRUE(learn::biteDue());
  // With the clock unknown, one bite and then none until the clock is known.
  learn::noteBite();
  EXPECT_FALSE(learn::biteDue());
  testNow = 1793714400;
  learn::onHostTime(testNow, 60);
  EXPECT_TRUE(learn::biteDue());
  learn::noteBite();
  EXPECT_FALSE(learn::biteDue());
  testNow += 86400;
  EXPECT_TRUE(learn::biteDue());
}

TEST_F(Fresh, SeenIdsPastTheScanCapStaySeen) {
  const std::string seen = R"({"id":"s1","kind":"bite","title":"Week","body":"Dear"})"
                           "\n";
  const std::string fresh = R"({"id":"n1","kind":"bite","title":"Week","body":"Dear"})"
                            "\n";
  fake::add("/x4/stream.jsonl", seen);
  learn::ensureReady();
  Item item;
  learn::Step step;
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBite, item, step));
  learn::markSeen(item);

  // The device reads 512 lines; the seen item sits past them.
  std::string filler;
  for (int i = 0; i < 512; ++i) filler += R"({"id":"f)" + std::to_string(i) + R"(","kind":"bad"})" "\n";
  fake::add("/x4/stream.jsonl", filler + seen, 5);
  learn::onHostDataChanged();
  learn::ensureReady();

  fake::add("/x4/stream.jsonl", seen + fresh, 9);
  learn::onHostDataChanged();
  learn::ensureReady();
  ASSERT_TRUE(learn::loadUnseen(*buffer, &isBite, item, step));
  EXPECT_STREQ(item.id, "n1");
}

TEST_F(Fresh, TheClockDatesTheDayAndMovesTheCardsWithIt) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a")}));
  learn::ensureReady();
  answerCards(*buffer);  // "a" is due on day 1 + its interval
  CardRecord before{};
  ASSERT_TRUE(decks::readRecord("d", 0, before));
  const uint32_t interval = before.dueDay - before.lastDay;

  testNow = 1793714400;  // 2026-11-03 09:00 UTC
  learn::onHostTime(testNow, 60);
  const auto date = static_cast<uint32_t>(civilDay(testNow, 60));
  EXPECT_EQ(learn::state().day, date);
  EXPECT_EQ(learn::today(), date);
  CardRecord after{};
  ASSERT_TRUE(decks::readRecord("d", 0, after));
  EXPECT_EQ(after.lastDay, date);
  EXPECT_EQ(after.dueDay - after.lastDay, interval);

  JsonDocument hello;
  learn::helloFields(hello.to<JsonObject>());
  EXPECT_EQ(hello["day"], date);
  EXPECT_TRUE(hello["streak"].isNull());
  EXPECT_TRUE(hello["earned"].isNull());

  // Two days later the day is the date again, and the cards stay where they are.
  learn::forgetPowerOn();
  testNow += 2 * 86400;
  learn::onHostTime(testNow, 60);
  EXPECT_EQ(learn::state().day, date + 2);
  ASSERT_TRUE(decks::readRecord("d", 0, after));
  EXPECT_EQ(after.lastDay, date);
}

TEST_F(Fresh, ReadingMakesTheDaysWithoutAClock) {
  learn::ensureReady();
  EXPECT_EQ(learn::today(), 1u);
  learn::addReading("/books/a.epub", DAY_READ_MS - 1000, 3);
  EXPECT_EQ(learn::today(), 1u);
  learn::addReading("/books/a.epub", 1000, 1);
  EXPECT_EQ(learn::today(), 2u);
  // Long reading makes several.
  learn::addReading("/books/a.epub", 2 * DAY_READ_MS, 9);
  EXPECT_EQ(learn::today(), 4u);
  // With a clock, reading makes none.
  testNow = 1793714400;
  learn::onHostTime(testNow, 0);
  const uint32_t date = learn::today();
  learn::addReading("/books/a.epub", 3 * DAY_READ_MS, 9);
  EXPECT_EQ(learn::today(), date);
}

TEST_F(Fresh, AClockBehindTheCountedDayMovesTheCardsBack) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a")}));
  testNow = 1793714400;
  learn::ensureReady();
  learn::onHostTime(testNow, 0);
  const uint32_t date = learn::today();
  answerCards(*buffer);
  CardRecord answered{};
  ASSERT_TRUE(decks::readRecord("d", 0, answered));

  // Offline, a long day of reading counts three days.
  learn::forgetPowerOn();
  testNow += 3600;
  learn::ensureReady();
  learn::addReading("/books/a.epub", 3 * DAY_READ_MS, 30);
  EXPECT_EQ(learn::today(), date + 3);

  // The clock returns the same day (X26): the day is the date, the cards move back three days,
  // and the bite is not held up.
  learn::noteBite();
  learn::onHostTime(testNow, 0);
  EXPECT_EQ(learn::today(), date);
  CardRecord back{};
  ASSERT_TRUE(decks::readRecord("d", 0, back));
  EXPECT_EQ(back.dueDay, answered.dueDay - 3);
  EXPECT_EQ(back.lastDay, answered.lastDay - 3);
  EXPECT_TRUE(learn::biteDue());
}

TEST_F(Fresh, AWakeWithTheHostsClockIsDated) {
  learn::ensureReady();
  EXPECT_EQ(learn::today(), 1u);
  // Deep sleep on USB power keeps the clock the host set, at its offset.
  learn::forgetPowerOn();
  testHostClock = true;
  testTz = 60;
  testNow = 1793714400;
  learn::ensureReady();
  EXPECT_EQ(learn::state().day, static_cast<uint32_t>(civilDay(testNow, 60)));
  EXPECT_TRUE(learn::state().dated);
  // Past midnight while awake, the next day begins.
  testNow += 86400;
  EXPECT_EQ(learn::today(), static_cast<uint32_t>(civilDay(testNow, 60)));
}

TEST_F(Fresh, AVisitCountsUpToTheLastPress) {
  learn::ensureReady();
  visits::noteIssue("2026-11-12");
  testMillis = 1000;
  visits::enter("world");
  testMillis = 5000;
  visits::press();
  testMillis = 41000;
  visits::press();
  testMillis = 600000;  // ten idle minutes before it is left count for nothing (D24)
  visits::enter("football");  // leafing on ends the visit
  testMillis = 602000;
  visits::press();
  visits::enter("football");  // its tables are the same visit
  testMillis = 603000;
  visits::press();
  visits::leave();
  visits::leave();  // nothing open: nothing written
  const auto out = lines(paths::VISITS);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0], R"({"section":"world","issue":"2026-11-12","ms":40000,"day":1,"at":null})");
  EXPECT_EQ(out[1], R"({"section":"football","issue":"2026-11-12","ms":3000,"day":1,"at":null})");
}

TEST_F(Fresh, AStateOfSetsMovesToDays) {
  // Version 1 of learn.bin (X27): set 7 open, the last clock seen on 2026-11-03; a card answered
  // in set 5 and due in set 9 is due two days after the day it becomes.
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a")}));
  learn::ensureReady();
  CardRecord record{};
  ASSERT_TRUE(decks::readRecord("d", 0, record));
  record.reps = 1;
  record.stability = 4.0f;
  record.difficulty = 5.0f;
  record.lastDay = 5;
  record.dueDay = 9;
  ASSERT_TRUE(decks::writeRecord("d", 0, record));
  learn::forgetPowerOn();

  struct Slot {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t seq;
    LearnState state;
    uint32_t crc;
  } slot{0x53433458, 1, sizeof(LearnState), 9, {}, 0};
  slot.state.day = 7;
  slot.state.unused2 = 1;  // version 1's open set
  slot.state.countMs = 4;       // version 1's streak
  slot.state.unused1 = 1793714400;  // version 1's streak anchor
  slot.state.tzMinutes = 60;
  slot.state.retention = -1.0f;
  uint32_t crc = 0xFFFFFFFFu;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&slot);
  for (size_t i = 0; i < offsetof(Slot, crc); ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  slot.crc = ~crc;
  std::string state(reinterpret_cast<const char*>(&slot), sizeof(slot));
  fake::add("/x4/state/learn.bin", state + state);

  learn::ensureReady();
  const auto date = static_cast<uint32_t>(civilDay(1793714400, 60));
  EXPECT_EQ(learn::state().day, date);
  EXPECT_EQ(learn::state().countMs, 0u);
  ASSERT_TRUE(decks::readRecord("d", 0, record));
  EXPECT_EQ(record.lastDay, date - 2);
  EXPECT_EQ(record.dueDay, date + 2);
  // Moved once: a second power-on leaves it.
  learn::forgetPowerOn();
  learn::ensureReady();
  ASSERT_TRUE(decks::readRecord("d", 0, record));
  EXPECT_EQ(record.dueDay, date + 2);
}

TEST(SetRules, CivilTimeBreaksDownAtTheOffset) {
  // 2026-11-03 13:05:00 UTC, a Tuesday.
  const int64_t at = 1793711100;
  CivilTime t = civilTime(at, 0);
  EXPECT_EQ(t.year, 2026);
  EXPECT_EQ(t.month, 11);
  EXPECT_EQ(t.day, 3);
  EXPECT_EQ(t.weekday, 2);
  EXPECT_EQ(t.hour, 13);
  EXPECT_EQ(t.minute, 5);
  // Budapest in winter: one hour later. Eleven hours later is the next day.
  EXPECT_EQ(civilTime(at, 60).hour, 14);
  t = civilTime(at, 11 * 60);
  EXPECT_EQ(t.day, 4);
  EXPECT_EQ(t.weekday, 3);
  EXPECT_EQ(t.hour, 0);
  // Leap day, and the epoch itself (a Thursday).
  t = civilTime(1709164800, 0);  // 2024-02-29
  EXPECT_EQ(t.month, 2);
  EXPECT_EQ(t.day, 29);
  t = civilTime(0, 0);
  EXPECT_EQ(t.year, 1970);
  EXPECT_EQ(t.weekday, 4);
  // West of UTC, before the epoch.
  t = civilTime(0, -60);
  EXPECT_EQ(t.year, 1969);
  EXPECT_EQ(t.month, 12);
  EXPECT_EQ(t.day, 31);
  EXPECT_EQ(t.hour, 23);
  EXPECT_EQ(t.weekday, 3);
}

TEST_F(Fresh, LocalTimeOnlyFromTheHostClock) {
  learn::ensureReady();
  CivilTime t{};
  // A clock restored from the trusted-time floor is plausible but stale: not shown.
  testNow = 1793711100;
  EXPECT_FALSE(learn::localTime(t));

  learn::onHostTime(testNow, 60);
  ASSERT_TRUE(learn::localTime(t));
  EXPECT_EQ(t.hour, 14);
  EXPECT_EQ(t.minute, 5);

  // A power cut forgets it; a deep sleep on USB power keeps it, at the last offset.
  learn::forgetPowerOn();
  EXPECT_FALSE(learn::localTime(t));
  testHostClock = true;
  testNow += 60;
  ASSERT_TRUE(learn::localTime(t));
  EXPECT_EQ(t.hour, 14);
  EXPECT_EQ(t.minute, 6);
}

TEST_F(Fresh, SleepPicksNextDueCardFront) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a", "Inner citadel?", "The mind.")}));
  learn::ensureReady();
  Item item;
  ASSERT_TRUE(learn::pickSleepItem(*buffer, item));
  EXPECT_STREQ(text(buffer->text, item.title), "Inner citadel?");
  // Answered today, it gives way to a highlight; the next day it is the card again, due or not.
  answerCards(*buffer);
  EXPECT_FALSE(learn::pickSleepItem(*buffer, item));
  fake::add("/x4/highlights.jsonl", R"({"id":"h1","text":"Only this.","title":"Book"})"
                                    "\n");
  ASSERT_TRUE(learn::pickSleepItem(*buffer, item));
  EXPECT_EQ(item.shape, Shape::Highlight);
  fake::files.erase("/x4/highlights.jsonl");
  nextDay();
  ASSERT_TRUE(learn::pickSleepItem(*buffer, item));
  EXPECT_EQ(item.shape, Shape::Flip);
}

TEST_F(Fresh, SleepFallsBackToHighlights) {
  fake::add("/x4/highlights.jsonl", R"({"id":"h1","text":"Only this.","title":"Book"})"
                                    "\n");
  learn::ensureReady();
  Item item;
  ASSERT_TRUE(learn::pickSleepItem(*buffer, item));
  EXPECT_EQ(item.shape, Shape::Highlight);
  EXPECT_STREQ(text(buffer->text, item.body), "Only this.");
  fake::files.erase("/x4/highlights.jsonl");
  EXPECT_FALSE(learn::pickSleepItem(*buffer, item));
}

// ---- journals ------------------------------------------------------------------

TEST_F(Fresh, AppendLookupWritesTheContractRecord) {
  learn::ensureReady();
  ASSERT_TRUE(learn::appendLookup("ataraxia", "/books/meditations.epub", "Meditations"));
  const std::string longWord = std::string(63, 'a') + "\xC3\xA9";  // é straddles 64 bytes
  ASSERT_TRUE(learn::appendLookup(longWord.c_str(), "books/read/r-1.epub", ""));
  EXPECT_FALSE(learn::appendLookup("", "/books/meditations.epub", "Meditations"));

  const auto all = lines(paths::LOOKUPS);
  ASSERT_EQ(all.size(), 2u);
  auto first = parse(all[0]);
  EXPECT_STREQ(first["word"], "ataraxia");
  EXPECT_STREQ(first["book"], "books/meditations.epub");
  EXPECT_STREQ(first["title"], "Meditations");
  EXPECT_EQ(first["day"], 1);
  EXPECT_TRUE(first["at"].isNull());
  auto second = parse(all[1]);
  EXPECT_EQ(std::string(second["word"].as<const char*>()), std::string(63, 'a'));
  EXPECT_STREQ(second["book"], "books/read/r-1.epub");
  EXPECT_FALSE(second["title"].is<const char*>());
}

TEST_F(Fresh, AppendHighlightWritesTheContractRecord) {
  learn::ensureReady();
  HighlightInput input;
  input.book = "/books/meditations.epub";
  input.title = "Meditations";
  input.author = "Marcus Aurelius";
  input.chapter = "Book II";
  input.text = "You have power over your mind, not outside events.";
  input.spine = 3;
  input.start = 1204;
  input.end = 1257;
  char id[16];
  ASSERT_TRUE(learn::appendHighlight(input, id));
  EXPECT_EQ(std::strncmp(id, "h-", 2), 0);
  EXPECT_EQ(std::strlen(id), 10u);

  input.kind = HighlightKind::Question;
  const std::string longText = std::string(BODY_MAX - 1, 'a') + "\xC3\xA9";  // é straddles the limit
  input.text = longText.c_str();
  char id2[16];
  ASSERT_TRUE(learn::appendHighlight(input, id2));
  EXPECT_STRNE(id, id2);

  const auto all = lines(paths::HIGHLIGHTS);
  ASSERT_EQ(all.size(), 2u);
  auto h = parse(all[0]);
  EXPECT_STREQ(h["id"], id);
  EXPECT_STREQ(h["kind"], "highlight");
  EXPECT_STREQ(h["book"], "books/meditations.epub");
  EXPECT_STREQ(h["author"], "Marcus Aurelius");
  EXPECT_STREQ(h["chapter"], "Book II");
  EXPECT_EQ(h["pos"]["spine"], 3);
  EXPECT_EQ(h["pos"]["start"], 1204);
  EXPECT_EQ(h["pos"]["end"], 1257);
  EXPECT_EQ(h["day"], 1);
  EXPECT_TRUE(h["at"].isNull());
  auto q = parse(all[1]);
  EXPECT_STREQ(q["kind"], "question");
  EXPECT_EQ(std::strlen(q["text"].as<const char*>()), BODY_MAX - 1);

  // The host pulled (renamed) the file: the next append starts a new one.
  fake::files.erase(paths::HIGHLIGHTS);
  ASSERT_TRUE(learn::appendHighlight(input, id2));
  EXPECT_EQ(lines(paths::HIGHLIGHTS).size(), 1u);
}

TEST_F(Fresh, ForgetHighlightBlanksItsLinesAndLogsTheDeletion) {
  learn::ensureReady();
  HighlightInput input;
  input.book = "/books/meditations.epub";
  input.text = "You have power over your mind.";
  input.spine = 3;
  input.start = 10;
  input.end = 40;
  char id[16];
  char computed[16];
  highlightId(input, computed);
  ASSERT_TRUE(learn::appendHighlight(input, id));
  EXPECT_STREQ(id, computed);  // the device can name a highlight it wrote from its clipping alone
  input.text = "Another passage.";
  char other[16];
  ASSERT_TRUE(learn::appendHighlight(input, other));
  input.text = "You have power over your mind.";
  ASSERT_TRUE(learn::appendHighlight(input, id));  // highlighted twice
  const size_t before = contents(paths::HIGHLIGHTS).size();

  ASSERT_TRUE(learn::forgetHighlight(id));
  const auto all = lines(paths::HIGHLIGHTS);
  ASSERT_EQ(all.size(), 4u);
  // Blanked in place: later lines keep their offsets for the queue.
  EXPECT_EQ(all[0].find_first_not_of(' '), std::string::npos);
  EXPECT_EQ(all[2].find_first_not_of(' '), std::string::npos);
  EXPECT_STREQ(parse(all[1])["id"], other);
  EXPECT_EQ(contents(paths::HIGHLIGHTS).size(), before + all[3].size() + 1);
  auto deletion = parse(all[3]);
  EXPECT_STREQ(deletion["id"], id);
  EXPECT_EQ(deletion["deleted"], true);
  EXPECT_EQ(deletion["day"], 1);
  EXPECT_TRUE(deletion["at"].isNull());
  EXPECT_TRUE(isHighlightDeletion(all[3].c_str(), all[3].size()));
  EXPECT_FALSE(isHighlightDeletion(all[1].c_str(), all[1].size()));

  // The host pulled the file: the deletion record alone tells it.
  fake::files.erase(paths::HIGHLIGHTS);
  ASSERT_TRUE(learn::forgetHighlight(other));
  ASSERT_EQ(lines(paths::HIGHLIGHTS).size(), 1u);
  EXPECT_STREQ(parse(lines(paths::HIGHLIGHTS)[0])["id"], other);
}

TEST_F(Fresh, SleepSkipsDeletedHighlights) {
  fake::add("/x4/highlights.jsonl", R"({"id":"h1","text":"Gone.","title":"Book"})"
                                    "\n"
                                    R"({"id":"h2","text":"Kept \"deleted\":true.","title":"Book"})"
                                    "\n");
  ASSERT_TRUE(learn::forgetHighlight("h1"));
  learn::ensureReady();
  for (int day = 0; day < 3; ++day) {
    Item item;
    ASSERT_TRUE(learn::pickSleepItem(*buffer, item));
    EXPECT_STREQ(text(buffer->text, item.body), "Kept \"deleted\":true.");
    nextDay();
  }
}

TEST_F(Fresh, AppendStartsNewLineAfterATornWrite) {
  fake::add("/x4/reviews.jsonl", R"({"card":"d/a","gra)");
  ASSERT_TRUE(appendJsonLine(paths::REVIEWS, "{}", 2));
  const auto all = lines(paths::REVIEWS);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[1], "{}");
}

TEST_F(Fresh, LearnStateSurvivesACorruptSlot) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a")}));
  learn::ensureReady();
  ASSERT_EQ(answerCards(*buffer), 1u);
  learn::forgetPowerOn();
  EXPECT_EQ(learn::state().newToday, 1);

  // Each save goes to the other slot; break the newest one.
  auto& bytes = fake::files[paths::LEARN_STATE]->bytes;
  const size_t slot = bytes.size() / 2;
  int newest = -1;
  uint32_t best = 0;
  for (int i = 0; i < 2; ++i) {
    uint32_t seq = 0;
    std::memcpy(&seq, bytes.data() + i * slot + 8, sizeof(seq));
    if (newest < 0 || seq > best) {
      newest = i;
      best = seq;
    }
  }
  bytes[newest * slot + 20] ^= 0xFF;
  learn::forgetPowerOn();
  EXPECT_EQ(learn::state().day, 1u);
  EXPECT_EQ(learn::state().newToday, 0);
}

// ---- reading stats ----------------------------------------------------------

TEST_F(Fresh, ReadingAddsToBookTotals) {
  fake::add("/x4/decks/d.jsonl", deckFile({flip("a")}));
  learn::ensureReady();
  learn::addReading("/books/a.epub", 60000, 3);
  learn::addReading("/books/a.epub", 30000, 2);
  learn::addReading("/books/b.epub", 1000, 1);
  learn::addReading("/books/c.epub", 0, 0);  // nothing read: no entry

  learn::flushStats();
  auto stats = parse(contents(paths::STATS));
  EXPECT_EQ(stats["books"]["books/a.epub"]["read_ms"], 90000);
  EXPECT_EQ(stats["books"]["books/a.epub"]["pages"], 5);
  EXPECT_EQ(stats["books"]["books/b.epub"]["read_ms"], 1000);
  EXPECT_TRUE(stats["books"]["books/c.epub"].isNull());

  // Totals survive a power-off.
  learn::forgetPowerOn();
  learn::ensureReady();
  learn::addReading("books/a.epub", 10000, 1);
  learn::flushStats();
  stats = parse(contents(paths::STATS));
  EXPECT_EQ(stats["books"]["books/a.epub"]["read_ms"], 100000);
  EXPECT_EQ(stats["books"]["books/a.epub"]["pages"], 6);
}

TEST_F(Fresh, ReadingStatsKeepTheMostRecentBooks) {
  char path[32];
  for (int i = 0; i <= books::MAX_BOOKS; ++i) {
    snprintf(path, sizeof(path), "/books/%02d.epub", i);
    learn::addReading(path, 1000, 1);
  }
  // Book 00 was the least recently read, so book 32 took its record.
  learn::flushStats();
  auto stats = parse(contents(paths::STATS));
  EXPECT_TRUE(stats["books"]["books/00.epub"].isNull());
  EXPECT_EQ(stats["books"]["books/01.epub"]["read_ms"], 1000);
  EXPECT_EQ(stats["books"]["books/32.epub"]["read_ms"], 1000);
  EXPECT_EQ(stats["books"].size(), books::MAX_BOOKS);

  // Reading book 01 again keeps it; book 02 is now the oldest.
  learn::addReading("/books/01.epub", 500, 0);
  learn::addReading("/books/33.epub", 500, 1);
  learn::flushStats();
  stats = parse(contents(paths::STATS));
  EXPECT_EQ(stats["books"]["books/01.epub"]["read_ms"], 1500);
  EXPECT_TRUE(stats["books"]["books/02.epub"].isNull());
  EXPECT_EQ(stats["books"]["books/33.epub"]["read_ms"], 500);
}

TEST_F(Fresh, ReadingLeavesStatsForTheFlush) {
  learn::ensureReady();
  const std::string before = contents(paths::STATS);
  learn::addReading("/books/a.epub", 1000, 1);
  EXPECT_EQ(contents(paths::STATS), before);
  learn::flushStats();
  const std::string after = contents(paths::STATS);
  EXPECT_EQ(parse(after)["books"]["books/a.epub"]["read_ms"], 1000);
  // Nothing changed since: the flush writes nothing.
  fake::add(paths::STATS, "untouched");
  learn::flushStats();
  EXPECT_EQ(contents(paths::STATS), "untouched");
}

TEST_F(Fresh, ReadingStatsSkipOverlongKeys) {
  const std::string path = "/" + std::string(books::KEY_MAX + 1, 'x');
  learn::addReading(path.c_str(), 1000, 1);
  learn::flushStats();
  EXPECT_EQ(parse(contents(paths::STATS))["books"].size(), 0u);
}

// ---- Previously ---------------------------------------------------------------

#include "NextBooks.h"
#include "Previously.h"

namespace {

struct Recaps : Fresh {
  void SetUp() override {
    Fresh::SetUp();
    std::vector<std::string> cards;
    for (int i = 0; i < 60; ++i) cards.push_back(flip("c" + std::to_string(i)));
    fake::add("/x4/decks/d.jsonl", deckFile(cards));
    fake::add(paths::PREVIOUSLY, R"({"book":"books/b.epub","progress":0.2,"text":"Not this one."})"
                                 "\n"
                                 R"({"book":"books/a.epub","progress":0.41,"text":"Where you were."})"
                                 "\n");
    learn::ensureReady();
  }
  std::string recap(const char* path, int percent) {
    char out[previously::TEXT_MAX + 1];
    return previously::load(path, percent, out) ? out : "";
  }
};

}  // namespace

TEST_F(Recaps, ShowAfterThreeDaysAway) {
  learn::addReading("/books/a.epub", 60000, 10);
  EXPECT_EQ(recap("/books/a.epub", 41), "");
  nextDay();
  nextDay();
  EXPECT_EQ(recap("/books/a.epub", 41), "");
  nextDay();
  EXPECT_EQ(recap("/books/a.epub", 41), "Where you were.");
  EXPECT_EQ(recap("/books/a.epub", 42), "Where you were.");
  // Read past the recap, or before it: it no longer fits.
  EXPECT_EQ(recap("/books/a.epub", 43), "");
  EXPECT_EQ(recap("/books/a.epub", 39), "");
  // A book without a last read, or without a recap.
  EXPECT_EQ(recap("/books/b.epub", 20), "");
  learn::addReading("/books/c.epub", 1000, 1);
  nextDay();
  nextDay();
  nextDay();
  EXPECT_EQ(recap("/books/c.epub", 0), "");
  // Reading again resets the count.
  learn::addReading("/books/a.epub", 1000, 1);
  EXPECT_EQ(recap("/books/a.epub", 41), "");
}

TEST_F(Recaps, ShowAfterTwoDaysByAKnownClock) {
  testHostClock = true;
  testNow = 1'700'000'000;
  learn::addReading("/books/a.epub", 60000, 10);
  testNow += 2 * 24 * 3600 - 60;
  EXPECT_EQ(recap("/books/a.epub", 41), "");
  testNow += 60;
  EXPECT_EQ(recap("/books/a.epub", 41), "Where you were.");
  // Without the clock now, only days count.
  testHostClock = false;
  learn::forgetPowerOn();
  EXPECT_EQ(recap("/books/a.epub", 41), "");
}

TEST_F(Recaps, LastReadIsTheDay) {
  learn::addReading("/books/a.epub", 1000, 1);
  books::LastRead last{};
  ASSERT_TRUE(books::lastRead("books/a.epub", last));
  EXPECT_EQ(last.day, learn::state().day);
  nextDay();
  learn::addReading("/books/a.epub", 1000, 1);
  ASSERT_TRUE(books::lastRead("books/a.epub", last));
  EXPECT_EQ(last.day, 2u);
  EXPECT_FALSE(books::lastRead("books/b.epub", last));
}

TEST_F(Fresh, NextBooksAreThePicksOnTheCard) {
  fake::add("/books/b.epub", "x");
  fake::add("/Theory -- Someone.epub", "x");
  fake::add(paths::NEXT,
            R"({"after":"books/z.epub","books":[{"book":"books/b.epub","title":"Not for this book"}]})"
            "\n"
            R"({"after":"books/a.epub","books":[{"book":"books/gone.epub","title":"Gone"},)"
            R"({"book":"Theory -- Someone.epub","title":"Theory and Reality"},{"book":"books/a.epub","title":"Itself"},)"
            R"({"book":"books/b.epub","title":"B"}]})"
            "\n");
  std::vector<nextbooks::Pick> picks;
  ASSERT_TRUE(nextbooks::load("/books/a.epub", picks));
  ASSERT_EQ(picks.size(), 2u);
  EXPECT_EQ(picks[0].path, "/Theory -- Someone.epub");
  EXPECT_EQ(picks[0].title, "Theory and Reality");
  EXPECT_EQ(picks[1].path, "/books/b.epub");
  EXPECT_FALSE(nextbooks::load("/books/c.epub", picks));
  EXPECT_TRUE(picks.empty());
}

TEST_F(Fresh, ReadingStatsKeepLongKeys) {
  const std::string key = "Theory and Reality -- " + std::string(books::KEY_MAX - 30, 'x') + ".epub";
  ASSERT_LE(key.size(), books::KEY_MAX);
  learn::addReading(("/" + key).c_str(), 2000, 3);
  learn::flushStats();
  EXPECT_EQ(parse(contents(paths::STATS))["books"][key]["read_ms"], 2000);
  uint32_t ms = 0, pages = 0;
  ASSERT_TRUE(books::find(key.c_str(), ms, pages));
  EXPECT_EQ(pages, 3u);
}

std::string readingStatsV1() {
  std::string v1(16, '\0');
  const uint32_t header[4] = {0x42423458, 1, 160, 0};
  std::memcpy(v1.data(), header, sizeof(header));
  std::string record(160, '\0');
  std::memcpy(record.data(), "books/a.epub", 12);
  const uint32_t totals[3] = {5000, 4, 1};
  std::memcpy(record.data() + 148, totals, sizeof(totals));
  return v1 + record;
}

// A version 1 file is never taken for an unknown one: a failed move keeps it, and lastread.bin,
// for the next try.
TEST_F(Fresh, ReadingStatsKeepVersionOneWhenTheMoveFails) {
  const std::string v1 = readingStatsV1();
  fake::add(paths::BOOKS, v1);
  fake::add(paths::LAST_READ_V1, std::string(40, 'x'));
  fake::failWritePath = paths::BOOKS_TMP;
  uint32_t ms = 0, pages = 0;
  EXPECT_FALSE(books::find("books/a.epub", ms, pages));
  fake::failWritePath = paths::BOOKS_TMP;
  EXPECT_FALSE(books::add("books/a.epub", 1000, 1, 0, 0));
  EXPECT_EQ(contents(paths::BOOKS), v1);
  EXPECT_TRUE(fake::files.contains(paths::LAST_READ_V1));
  EXPECT_FALSE(fake::files.contains(paths::BOOKS_TMP));

  ASSERT_TRUE(books::find("books/a.epub", ms, pages));
  EXPECT_EQ(ms, 5000u);
  EXPECT_FALSE(fake::files.contains(paths::LAST_READ_V1));
}

// Power lost between replaceFile's remove and rename: only the complete tmp is left.
TEST_F(Fresh, ReadingStatsRecoverACutReplace) {
  fake::add(paths::BOOKS, readingStatsV1());
  uint32_t ms = 0, pages = 0;
  ASSERT_TRUE(books::find("books/a.epub", ms, pages));  // moved to version 2
  ASSERT_TRUE(Storage.rename(paths::BOOKS, paths::BOOKS_TMP));
  ASSERT_TRUE(books::find("books/a.epub", ms, pages));
  EXPECT_EQ(ms, 5000u);
  EXPECT_TRUE(fake::files.contains(paths::BOOKS));
  EXPECT_FALSE(fake::files.contains(paths::BOOKS_TMP));
}

TEST_F(Fresh, StateFileRecoversOnlyACompleteReplace) {
  fake::add(paths::SEEN_TMP, "new");
  statefile::recover(paths::SEEN_TMP, paths::SEEN);
  EXPECT_EQ(contents(paths::SEEN), "new");
  EXPECT_FALSE(fake::files.contains(paths::SEEN_TMP));

  fake::add(paths::SEEN_TMP, "cut short");  // beside the file: a write that never finished
  statefile::recover(paths::SEEN_TMP, paths::SEEN);
  EXPECT_EQ(contents(paths::SEEN), "new");
  EXPECT_FALSE(fake::files.contains(paths::SEEN_TMP));
}

TEST_F(Fresh, StateFileFailedReplaceKeepsTheOld) {
  fake::add(paths::READ_DONE, "old");
  fake::failWritePath = paths::READ_DONE_TMP;
  const char data[] = "new";
  EXPECT_FALSE(statefile::replace(paths::READ_DONE_TMP, paths::READ_DONE, {{data, 3}}));
  EXPECT_EQ(contents(paths::READ_DONE), "old");
  EXPECT_FALSE(fake::files.contains(paths::READ_DONE_TMP));
}

TEST_F(Fresh, PowerOnRecoversTheSeenSet) {
  fake::add(paths::SEEN_TMP, "seen");
  learn::state();
  EXPECT_EQ(contents(paths::SEEN), "seen");
  EXPECT_FALSE(fake::files.contains(paths::SEEN_TMP));
}

TEST_F(Fresh, ReadingStatsMoveFromVersionOne) {
  // Version 1: keys up to 147 bytes, no last read, beside a one-day lastread.bin.
  std::string v1(16, '\0');
  const uint32_t header[4] = {0x42423458, 1, 160, 0};
  std::memcpy(v1.data(), header, sizeof(header));
  for (const auto& [key, ms, seq] : {std::tuple{"books/a.epub", 5000u, 1u}, std::tuple{"books/b.epub", 7000u, 2u}}) {
    std::string record(160, '\0');
    std::memcpy(record.data(), key, std::strlen(key));
    const uint32_t totals[3] = {ms, 4, seq};
    std::memcpy(record.data() + 148, totals, sizeof(totals));
    v1 += record;
  }
  fake::add(paths::BOOKS, v1);
  fake::add(paths::LAST_READ_V1, std::string(40, 'x'));

  uint32_t ms = 0, pages = 0;
  ASSERT_TRUE(books::find("books/b.epub", ms, pages));
  EXPECT_EQ(ms, 7000u);
  EXPECT_EQ(pages, 4u);
  EXPECT_FALSE(fake::files.contains(paths::LAST_READ_V1));
  EXPECT_FALSE(fake::files.contains(paths::BOOKS_TMP));
  EXPECT_EQ(contents(paths::BOOKS).size(), 16u + 2 * 512);
  books::LastRead last{};
  EXPECT_FALSE(books::lastRead("books/a.epub", last));  // unknown, so never away

  learn::addReading("/books/a.epub", 1000, 1);
  learn::flushStats();
  const auto stats = parse(contents(paths::STATS));
  EXPECT_EQ(stats["books"]["books/a.epub"]["read_ms"], 6000);
  EXPECT_EQ(stats["books"]["books/b.epub"]["read_ms"], 7000);
  EXPECT_TRUE(books::lastRead("books/a.epub", last));
}
