#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "play/games/Words.h"

using namespace x4::play;
using namespace x4::play::words;

namespace {

struct WordsTest : ::testing::Test {
  void SetUp() override { fake::reset(); }
};

std::vector<Letter> letters(Lang lang, const char* text) {
  Letter out[LETTERS_MAX];
  const int n = decode(lang, text, std::strlen(text), out, LETTERS_MAX);
  return n < 0 ? std::vector<Letter>{} : std::vector<Letter>(out, out + n);
}

std::string marks(const char* answer, const char* guess, Lang lang = Lang::En) {
  const auto a = letters(lang, answer);
  const auto g = letters(lang, guess);
  Mark m[WORD_LEN];
  score(a.data(), g.data(), m);
  std::string out;
  for (const Mark k : m) out += k == Mark::Correct ? 'C' : k == Mark::Present ? 'P' : '.';
  return out;
}

std::string file(const std::vector<std::string>& lines) {
  std::string out;
  for (const auto& line : lines) out += line + "\n";
  return out;
}

constexpr char CROSSWORD[] =
    R"({"id":"en-0004","size":5,"rows":["there","a.a.n","burnt","o.t.e","other"],)"
    R"("across":[{"n":1,"clue":"In that place"},{"n":4,"clue":"Damaged by fire"},{"n":5,"clue":"Second"}],)"
    R"("down":[{"n":1,"clue":"Forbidden"},{"n":2,"clue":"Soil"},{"n":3,"clue":"To go into"}]})";

}  // namespace

// ---- letters ------------------------------------------------------------------

TEST_F(WordsTest, HungarianAccentsAreSingleLetters) {
  const auto w = letters(Lang::Hu, "felhő");
  ASSERT_EQ(w.size(), 5u);
  EXPECT_EQ(w[4], 0x151);
  EXPECT_TRUE(letters(Lang::En, "felhő").empty());   // ő is not an English letter
  EXPECT_EQ(letters(Lang::Hu, "szőlő").size(), 5u);  // sz is two letters
  EXPECT_TRUE(letters(Lang::Hu, "Alma").empty());    // lowercase only

  char text[32];
  encode(w.data(), static_cast<int>(w.size()), text, sizeof(text));
  EXPECT_STREQ(text, "felhő");

  const Letter* abc = nullptr;
  EXPECT_EQ(alphabet(Lang::Hu, &abc), 35);
  EXPECT_EQ(alphabet(Lang::En, &abc), 26);
}

TEST_F(WordsTest, UpperCase) {
  EXPECT_EQ(upper('a'), 'A');
  EXPECT_EQ(upper(0xE1), 0xC1);    // á
  EXPECT_EQ(upper(0xF6), 0xD6);    // ö
  EXPECT_EQ(upper(0x151), 0x150);  // ő
  EXPECT_EQ(upper(0x171), 0x170);  // ű
  EXPECT_EQ(upper('A'), 'A');
}

// ---- scoring ----------------------------------------------------------------------

TEST_F(WordsTest, ScoresGreensThenYellows) {
  EXPECT_EQ(marks("crane", "crane"), "CCCCC");
  EXPECT_EQ(marks("crane", "nacre"), "PPPPC");
  EXPECT_EQ(marks("crane", "pious"), ".....");
}

TEST_F(WordsTest, RepeatedLettersCountOnlyAsOftenAsTheAnswerHasThem) {
  EXPECT_EQ(marks("abbey", "bobby"), "P.C.C");  // the third b finds no b left
  EXPECT_EQ(marks("speed", "erase"), "P..PP");  // two e in both
  EXPECT_EQ(marks("those", "geese"), "...CC");  // a green e takes the only e
  EXPECT_EQ(marks("eerie", "level"), ".C.P.");
}

TEST_F(WordsTest, ScoresHungarianLetters) {
  EXPECT_EQ(marks("felhő", "erdő\xc3\xa9", Lang::Hu), "P..P.");  // "erdőé"
  EXPECT_EQ(marks("kőfal", "falók", Lang::Hu), "PPP.P");
  EXPECT_EQ(marks("kőfal", "kofal", Lang::Hu), "C.CCC");  // o is not ő
}

// ---- word files ----------------------------------------------------------------------

TEST_F(WordsTest, BinarySearchFindsEveryWordAndNothingElse) {
  std::vector<std::string> words = {"alma", "almák", "almás", "erdő", "erdők", "felhő", "kőfal", "szőlő", "ál", "ős"};
  std::sort(words.begin(), words.end());  // by bytes
  fake::add("/x4/words/hu/valid5.txt", file(words));
  for (const auto& w : words) {
    EXPECT_TRUE(contains("/x4/words/hu/valid5.txt", w.c_str(), w.size())) << w;
  }
  for (const char* w : {"a", "alm", "almáé", "zzz", "erdőd", "felh", "ősz", ""}) {
    EXPECT_FALSE(contains("/x4/words/hu/valid5.txt", w, std::strlen(w))) << w;
  }
  EXPECT_FALSE(contains("/x4/words/hu/missing.txt", "alma", 4));
  const auto w = letters(Lang::Hu, "kőfal");
  EXPECT_TRUE(contains("/x4/words/hu/valid5.txt", w.data(), 5));
}

TEST_F(WordsTest, BinarySearchReadsLittle) {
  std::vector<std::string> words;
  for (int i = 0; i < 20000; ++i) {
    char w[6];
    snprintf(w, sizeof(w), "%c%c%c%c%c", 'a' + i / 2000 % 26, 'a' + i / 300 % 26, 'a' + i / 26 % 26, 'a' + i % 26, 'a');
    words.emplace_back(w);
  }
  std::sort(words.begin(), words.end());
  words.erase(std::unique(words.begin(), words.end()), words.end());
  fake::add("/w.txt", file(words));
  for (const auto& w : {words.front(), words[words.size() / 3], words.back()}) {
    fake::resetIoCounters();
    EXPECT_TRUE(contains("/w.txt", w.c_str(), w.size()));
    EXPECT_LT(fake::reads, 64u);  // about two reads per halving
  }
  EXPECT_FALSE(contains("/w.txt", "zzzzz", 5));
}

TEST_F(WordsTest, PickLineFollowsTheLevel) {
  std::vector<std::string> words;
  for (int i = 0; i < 1000; ++i) words.push_back("w" + std::to_string(1000 + i));  // "w1000" most common
  fake::add("/a.txt", file(words));
  char out[LINE_SIZE];
  std::set<std::string> easy;
  for (uint32_t seed = 0; seed < 300; ++seed) {
    ASSERT_TRUE(pickLine("/a.txt", seed * 2654435761u, 1, out, sizeof(out)));
    easy.insert(out);
    EXPECT_LT(std::stoi(out + 1), 1000 + 320) << out;
    ASSERT_TRUE(pickLine("/a.txt", seed * 2654435761u, 10, out, sizeof(out)));
    EXPECT_GE(std::stoi(out + 1), 1000 + 440) << out;
  }
  EXPECT_GT(easy.size(), 100u);
  // Same seed, same word.
  char again[LINE_SIZE];
  pickLine("/a.txt", 7, 5, out, sizeof(out));
  pickLine("/a.txt", 7, 5, again, sizeof(again));
  EXPECT_STREQ(out, again);
  // A filter skips lines and wraps around the file.
  const auto onlyFirst = [](const char* line, void*) { return std::strcmp(line, "w1000") == 0; };
  ASSERT_TRUE(pickLine("/a.txt", 12345, 10, out, sizeof(out), onlyFirst));
  EXPECT_STREQ(out, "w1000");
  EXPECT_FALSE(pickLine("/none.txt", 1, 1, out, sizeof(out)));
}

TEST_F(WordsTest, PickBandStaysInItsSlice) {
  std::vector<std::string> lines;
  for (int i = 0; i < 1000; ++i) lines.push_back("p" + std::to_string(1000 + i));  // equal lengths
  fake::add("/b.txt", file(lines));
  char out[LINE_SIZE];
  for (uint8_t band = 0; band < 10; ++band) {
    std::set<std::string> seen;
    for (uint32_t seed = 0; seed < 200; ++seed) {
      ASSERT_TRUE(pickBand("/b.txt", seed * 2654435761u, band, 10, out, sizeof(out)));
      const int n = std::stoi(out + 1) - 1000;
      EXPECT_GE(n, band * 100) << out;
      EXPECT_LE(n, band * 100 + 100) << out;  // the slice's last line may reach the next one
      seen.insert(out);
    }
    EXPECT_GT(seen.size(), 50u);
  }
  EXPECT_FALSE(pickBand("/b.txt", 1, 10, 10, out, sizeof(out)));
  EXPECT_FALSE(pickBand("/none.txt", 1, 0, 10, out, sizeof(out)));
}

// ---- crossword --------------------------------------------------------------------------

TEST_F(WordsTest, CrosswordNumbersAndEntries) {
  Crossword c;
  ASSERT_TRUE(parseCrossword(Lang::En, CROSSWORD, std::strlen(CROSSWORD), c));
  EXPECT_STREQ(c.id, "en-0004");
  EXPECT_EQ(c.cells[6], 0);  // (1, 1) is black
  EXPECT_EQ(c.cells[0], 't');
  EXPECT_EQ(c.numbers[0], 1);
  EXPECT_EQ(c.numbers[2], 2);
  EXPECT_EQ(c.numbers[4], 3);
  EXPECT_EQ(c.numbers[10], 4);
  EXPECT_EQ(c.numbers[20], 5);
  ASSERT_EQ(c.entryCount, 6);

  const int burnt = c.entryAt(12, true);
  ASSERT_GE(burnt, 0);
  EXPECT_STREQ(c.entries[burnt].clue, "Damaged by fire");
  EXPECT_EQ(c.entries[burnt].length, 5);
  EXPECT_EQ(c.entryAt(5, true), -1);  // a run of one is no entry
  const int taboo = c.entryAt(5, false);
  ASSERT_GE(taboo, 0);
  EXPECT_EQ(c.cellOf(c.entries[taboo], 4), 20);
}

// The grid of CROSSWORD; # is black.
//   t h e r e
//   a # a # n
//   b u r n t
//   o # t # e
//   o t h e r
TEST_F(WordsTest, CrosswordArrowsTurnThenMove) {
  Crossword c;
  ASSERT_TRUE(parseCrossword(Lang::En, CROSSWORD, std::strlen(CROSSWORD), c));
  const Spot first = firstSpot(c);
  EXPECT_EQ(first.cell, 0);
  EXPECT_TRUE(first.across);

  // Along the word: one white cell on.
  Spot s = moveSpot(c, first, true, 1);
  EXPECT_EQ(s.cell, 1);
  EXPECT_TRUE(s.across);
  // Across it, where a word runs that way: turn in place, then move.
  s = moveSpot(c, first, false, 1);
  EXPECT_EQ(s.cell, 0);
  EXPECT_FALSE(s.across);
  s = moveSpot(c, s, false, 1);
  EXPECT_EQ(s.cell, 5);
  EXPECT_FALSE(s.across);
  // No word down at (0, 1): Down moves over the black square and stays across.
  s = moveSpot(c, {1, true}, false, 1);
  EXPECT_EQ(s.cell, 11);
  EXPECT_TRUE(s.across);
  // Left and Right wrap in reading order, Up and Down in column order.
  EXPECT_EQ(moveSpot(c, {24, true}, true, 1).cell, 0);
  EXPECT_EQ(moveSpot(c, {0, true}, true, -1).cell, 24);
  EXPECT_EQ(moveSpot(c, {4, true}, true, 1).cell, 5);
  s = moveSpot(c, {0, false}, false, -1);
  EXPECT_EQ(s.cell, 24);
  EXPECT_FALSE(s.across);
  EXPECT_EQ(moveSpot(c, {20, false}, false, 1).cell, 1);
}

TEST_F(WordsTest, CrosswordTypingSteps) {
  Crossword c;
  ASSERT_TRUE(parseCrossword(Lang::En, CROSSWORD, std::strlen(CROSSWORD), c));
  Letter typed[CELLS] = {};

  typed[0] = 't';
  Spot s = afterLetter(c, typed, {0, true});
  EXPECT_EQ(s.cell, 1);
  EXPECT_TRUE(s.across);
  // Filled cells are skipped; the word's first gap comes after its end.
  typed[1] = 'h';
  typed[3] = 'r';
  typed[4] = 'e';
  EXPECT_EQ(afterLetter(c, typed, {0, true}).cell, 2);
  EXPECT_EQ(afterLetter(c, typed, {3, true}).cell, 2);
  // A full word is typed over cell by cell; past its end comes the next word.
  typed[2] = 'e';
  EXPECT_EQ(afterLetter(c, typed, {1, true}).cell, 2);
  s = afterLetter(c, typed, {4, true});
  EXPECT_EQ(s.cell, 10);
  EXPECT_TRUE(s.across);

  // Delete clears this cell, or steps back when it is empty.
  EXPECT_EQ(eraseSpot(c, typed, {2, true}).cell, 2);
  typed[2] = 0;
  EXPECT_EQ(eraseSpot(c, typed, {2, true}).cell, 1);
  typed[0] = 0;
  EXPECT_EQ(eraseSpot(c, typed, {0, true}).cell, 0);
}

TEST_F(WordsTest, CrosswordNextWordSkipsFullOnes) {
  Crossword c;
  ASSERT_TRUE(parseCrossword(Lang::En, CROSSWORD, std::strlen(CROSSWORD), c));
  Letter typed[CELLS] = {};
  for (uint8_t i = 10; i < 15; ++i) typed[i] = c.cells[i];  // burnt
  typed[20] = 'o';
  Spot s = nextWord(c, typed, {0, true});
  EXPECT_EQ(s.cell, 21);  // other, at its first gap
  EXPECT_TRUE(s.across);
  // From the last word back to the first.
  s = nextWord(c, typed, {4, false});
  EXPECT_EQ(s.cell, 0);
  EXPECT_TRUE(s.across);
  // From an across word to the down words after the last across.
  s = nextWord(c, typed, {20, true});
  EXPECT_EQ(s.cell, 0);
  EXPECT_FALSE(s.across);
  // A full grid: the next word's first cell.
  std::copy(c.cells, c.cells + CELLS, typed);
  s = nextWord(c, typed, {0, true});
  EXPECT_EQ(s.cell, 10);
  EXPECT_TRUE(s.across);
}

TEST_F(WordsTest, CrosswordRejectsBadLines) {
  Crossword c;
  std::string bad = CROSSWORD;
  bad.replace(bad.find("\"n\":4"), 5, "\"n\":9");  // no cell has number 9
  EXPECT_FALSE(parseCrossword(Lang::En, bad.c_str(), bad.size(), c));
  bad = CROSSWORD;
  bad.replace(bad.find("burnt"), 5, "burn");
  EXPECT_FALSE(parseCrossword(Lang::En, bad.c_str(), bad.size(), c));
  EXPECT_FALSE(parseCrossword(Lang::En, "{", 1, c));
  const char hu[] =
      R"({"id":"hu-1","size":5,"rows":["kőfal","a.l.m","almás","é.m.a","tűzés"],"across":[{"n":1,"clue":"Wall"}],"down":[]})";
  ASSERT_TRUE(parseCrossword(Lang::Hu, hu, std::strlen(hu), c));
  EXPECT_EQ(c.cells[1], 0x151);
  EXPECT_EQ(c.cells[21], 0x171);
}

// A 7x7 grid; # is black.
//   a b c # d e f      1 2 3 # 4 5 6
//   g h i # j k l      7 . . # 8 . .
//   m n o p q r s      9 . . 10. . .
//   # # t u v # #      # # 11. . # #
//   w x y z a b c      12 13 . . . 14 15
//   d e f # g h i      16. . # 17. .
//   j k l # m n o      18. . # 19. .
constexpr char ROWS7[] = R"(["abc.def","ghi.jkl","mnopqrs","..tuv..","wxyzabc","def.ghi","jkl.mno"])";

std::string grid7(const std::string& across1Src, const std::string& down3Src) {
  std::string out = R"({"id":"en-0101","size":7,"rows":)" + std::string(ROWS7) + R"(,"across":[)";
  const int across[] = {1, 4, 7, 8, 9, 11, 12, 16, 17, 18, 19};
  const int down[] = {1, 2, 3, 4, 5, 6, 10, 12, 13, 14, 15};
  for (const int n : across) {
    out += "{\"n\":" + std::to_string(n) + ",\"clue\":\"A" + std::to_string(n) + "\"";
    if (n == 1 && !across1Src.empty()) out += ",\"src\":\"" + across1Src + "\"";
    if (n == 9) out += ",\"src\":\"Meditations\"";
    out += "},";
  }
  out.back() = ']';
  out += ",\"down\":[";
  for (const int n : down) {
    out += "{\"n\":" + std::to_string(n) + ",\"clue\":\"D" + std::to_string(n) + "\"";
    if (n == 3 && !down3Src.empty()) out += ",\"src\":\"" + down3Src + "\"";
    out += "},";
  }
  out.back() = ']';
  return out + "}";
}

TEST_F(WordsTest, SevenBySevenNumbersEntriesAndSources) {
  Crossword c;
  const std::string longTitle = "A very long article title that runs well past forty-eight bytes";
  const std::string line = grid7("Meditations", longTitle);
  ASSERT_TRUE(parseCrossword(Lang::En, line.c_str(), line.size(), c));
  EXPECT_EQ(c.size, 7);
  EXPECT_EQ(c.cellCount(), 49);
  EXPECT_EQ(c.entryCount, 22);
  EXPECT_EQ(c.numbers[0], 1);
  EXPECT_EQ(c.numbers[4], 4);
  EXPECT_EQ(c.numbers[7], 7);
  EXPECT_EQ(c.numbers[14], 9);
  EXPECT_EQ(c.numbers[17], 10);
  EXPECT_EQ(c.numbers[23], 11);
  EXPECT_EQ(c.numbers[28], 12);
  EXPECT_EQ(c.numbers[48 - 6], 18);
  EXPECT_EQ(c.cells[21], 0);
  EXPECT_EQ(c.cells[23], 't');

  const int nine = c.entryAt(14, true);
  ASSERT_GE(nine, 0);
  EXPECT_EQ(c.entries[nine].length, 7);
  const int three = c.entryAt(2, false);
  ASSERT_GE(three, 0);
  EXPECT_EQ(c.entries[three].length, 7);
  EXPECT_EQ(c.cellOf(c.entries[three], 6), 44);
  const int ten = c.entryAt(31, false);
  ASSERT_GE(ten, 0);
  EXPECT_EQ(c.entries[ten].number, 10);
  EXPECT_EQ(c.entries[ten].length, 3);

  // One source per title, in entry order; a long title is cut on a character.
  ASSERT_EQ(c.sourceCount, 2);
  EXPECT_STREQ(c.sources[0], "Meditations");
  EXPECT_EQ(std::strlen(c.sources[1]), SRC_MAX);
  EXPECT_EQ(c.entries[c.entryAt(0, true)].source, 1);
  EXPECT_EQ(c.entries[nine].source, 1);
  EXPECT_EQ(c.entries[three].source, 2);
  EXPECT_EQ(c.entries[ten].source, 0);
}

TEST_F(WordsTest, SevenBySevenArrowsWrapOverTheWholeGrid) {
  Crossword c;
  const std::string line = grid7("", "");
  ASSERT_TRUE(parseCrossword(Lang::En, line.c_str(), line.size(), c));
  EXPECT_EQ(c.sourceCount, 1);  // 9 across only
  EXPECT_EQ(firstSpot(c).cell, 0);
  EXPECT_EQ(moveSpot(c, {48, true}, true, 1).cell, 0);
  EXPECT_EQ(moveSpot(c, {2, true}, true, 1).cell, 4);  // over the black square
  Spot s = moveSpot(c, {14, false}, false, 1);
  EXPECT_EQ(s.cell, 28);  // (3, 0) is black
  EXPECT_FALSE(s.across);
  s = moveSpot(c, {42, false}, false, 1);
  EXPECT_EQ(s.cell, 1);  // the next column
}

TEST_F(WordsTest, SourcesPastTheTableHaveNone) {
  std::string line = R"({"id":"en-1","size":7,"rows":)" + std::string(ROWS7) + R"(,"across":[)";
  const int across[] = {1, 4, 7, 8, 9, 11, 12, 16, 17, 18, 19};
  for (const int n : across) {
    line += "{\"n\":" + std::to_string(n) + ",\"clue\":\"x\",\"src\":\"S" + std::to_string(n) + "\"},";
  }
  line.back() = ']';
  line += R"(,"down":[]})";
  Crossword c;
  ASSERT_TRUE(parseCrossword(Lang::En, line.c_str(), line.size(), c));
  EXPECT_EQ(c.sourceCount, SOURCES_MAX);
  EXPECT_EQ(c.entries[SOURCES_MAX - 1].source, SOURCES_MAX);
  EXPECT_EQ(c.entries[SOURCES_MAX].source, 0);
}

TEST_F(WordsTest, OnlyFiveAndSevenPlay) {
  Crossword c;
  ASSERT_TRUE(parseCrossword(Lang::En, CROSSWORD, std::strlen(CROSSWORD), c));
  EXPECT_EQ(c.size, 5);
  std::string six = grid7("", "");
  six.replace(six.find("\"size\":7"), 8, "\"size\":6");
  EXPECT_FALSE(parseCrossword(Lang::En, six.c_str(), six.size(), c));
  std::string shortRows = grid7("", "");
  shortRows.replace(shortRows.find(",\"jkl.mno\""), 10, "");
  EXPECT_FALSE(parseCrossword(Lang::En, shortRows.c_str(), shortRows.size(), c));
}

TEST_F(WordsTest, ReadLineWrapsAndProgressPersists) {
  fake::add("/x4/crosswords/en.jsonl", "first\nsecond\nthird\n");
  char out[CROSSWORD_LINE_MAX];
  uint32_t count = 0;
  ASSERT_TRUE(readLine("/x4/crosswords/en.jsonl", 1, out, sizeof(out), &count));
  EXPECT_STREQ(out, "second");
  EXPECT_EQ(count, 3u);
  ASSERT_TRUE(readLine("/x4/crosswords/en.jsonl", 5, out, sizeof(out)));
  EXPECT_STREQ(out, "third");
  ASSERT_TRUE(readLine("/x4/crosswords/en.jsonl", 3, out, sizeof(out)));
  EXPECT_STREQ(out, "first");
  fake::add("/x4/crosswords/hu.jsonl", "only");
  ASSERT_TRUE(readLine("/x4/crosswords/hu.jsonl", 0, out, sizeof(out), &count));
  EXPECT_STREQ(out, "only");
  EXPECT_EQ(count, 1u);

  EXPECT_EQ(crosswordNext(Lang::En), 0u);
  setCrosswordNext(Lang::En, 42);
  setCrosswordNext(Lang::Hu, 7);
  EXPECT_EQ(crosswordNext(Lang::En), 42u);
  EXPECT_EQ(crosswordNext(Lang::Hu), 7u);
}

TEST_F(WordsTest, LanguageIsStoredPerGame) {
  EXPECT_EQ(standing("words").lang, DEFAULT_LANG);
  EXPECT_EQ(DEFAULT_LANG, Lang::Hu);
  setLang("words", Lang::En);
  EXPECT_EQ(standing("words").lang, Lang::En);
  EXPECT_EQ(standing("crossword").lang, Lang::Hu);
  // The language survives a finished game.
  Result r;
  r.game = "words";
  r.lang = "en";
  r.won = true;
  finish(r, 3, 0);
  EXPECT_EQ(standing("words").lang, Lang::En);
  EXPECT_EQ(standing("words").level, LEVEL_START + 1);
}
