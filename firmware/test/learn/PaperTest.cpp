// The newspaper's issue as the front page and a section read it, and where the keys lead.
#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "Learn.h"
#include "LearnPaths.h"
#include "Paper.h"
#include "ReadList.h"

using namespace x4;

namespace {

const char ISSUE[] =
    R"({"date":"2026-11-12","sections":[{"id":"hungary","name":"Hungary"},{"id":"world","name":"World"},{"id":"football","name":"Football"},{"id":"film","name":"Film"}]})"
    "\n"
    R"({"id":"r-1","section":"hungary","kind":"news","title":"A long Hungarian headline","head":"Short one","lead":"What it says.","source":"Telex","book":"books/read/r-1.epub"})"
    "\n"
    R"({"id":"r-2","section":"hungary","kind":"news","title":"Second headline","lead":"Second lead.","source":"444","book":"books/read/r-2.epub"})"
    "\n"
    R"({"id":"r-3","section":"world","kind":"news","title":"World headline","head":"World short","source":"DW","book":"books/read/r-3.epub"})"
    "\n"
    R"({"id":"r-4","section":"world","kind":"news","title":"Not on the card","book":"books/read/r-4.epub"})"
    "\n"
    R"({"id":"r-5","kind":"link","title":"A saved link","kept":true,"book":"books/read/r-5.epub"})"
    "\n";

struct Paper : ::testing::Test {
  void SetUp() override {
    fake::reset();
    learn::forgetPowerOn();
    buffer = std::make_unique<LineBuffer>();
    fake::add(paths::READ_LIST, ISSUE);
    for (const char* id : {"r-1", "r-2", "r-3", "r-5"}) fake::add(("/books/read/" + std::string(id) + ".epub").c_str(), "x");
  }
  paper::Front front() {
    paper::Front out;
    EXPECT_TRUE(paper::loadFront(*buffer, out));
    return out;
  }
  std::unique_ptr<LineBuffer> buffer;
};

}  // namespace

TEST_F(Paper, TheFrontPageHasEverySectionWithItsFirstUnreadHeadline) {
  const paper::Front f = front();
  EXPECT_STREQ(f.date, "2026-11-12");
  ASSERT_EQ(f.count, 4);
  EXPECT_STREQ(f.sections[0].name, "Hungary");
  EXPECT_STREQ(f.sections[0].head, "Short one");
  EXPECT_EQ(f.sections[0].pieces, 2);
  // A piece whose article is not on the card is not there; one without a section is in none.
  EXPECT_EQ(f.sections[1].pieces, 1);
  EXPECT_STREQ(f.sections[1].head, "World short");
  // A section with nothing keeps its row and has no line.
  EXPECT_EQ(f.sections[3].pieces, 0);
  EXPECT_STREQ(f.sections[3].head, "");
}

TEST_F(Paper, AReadPieceGivesTheLineToTheNextAndAReadSectionHasNone) {
  readlist::onBookClosed("/books/read/r-1.epub", 1.0f, true, 60000);
  // Without a short headline the front page takes the article's own.
  EXPECT_STREQ(front().sections[0].head, "Second headline");
  readlist::onBookClosed("/books/read/r-2.epub", 1.0f, true, 60000);
  const paper::Front f = front();
  EXPECT_STREQ(f.sections[0].head, "");
  EXPECT_EQ(f.sections[0].pieces, 2);
}

TEST_F(Paper, ASectionListsItsPiecesAndMarksTheReadOnes) {
  readlist::onBookClosed("/books/read/r-2.epub", 0.97f, false, 60000);
  paper::Piece pieces[paper::PIECES_MAX];
  ASSERT_EQ(paper::loadSection(*buffer, "hungary", pieces, paper::PIECES_MAX), 2);
  EXPECT_STREQ(pieces[0].id, "r-1");
  EXPECT_STREQ(pieces[0].source, "Telex");
  EXPECT_STREQ(pieces[0].title, "A long Hungarian headline");
  EXPECT_STREQ(pieces[0].lead, "What it says.");
  EXPECT_STREQ(pieces[0].book, "books/read/r-1.epub");
  EXPECT_FALSE(pieces[0].read);
  EXPECT_TRUE(pieces[1].read);
  EXPECT_EQ(paper::loadSection(*buffer, "film", pieces, paper::PIECES_MAX), 0);
}

TEST_F(Paper, AReadListWithoutAHeaderIsNotAnIssue) {
  fake::add(paths::READ_LIST, R"({"id":"r-1","kind":"news","title":"Old list","book":"books/read/r-1.epub"})"
                              "\n");
  paper::Front out;
  EXPECT_FALSE(paper::loadFront(*buffer, out));
  EXPECT_EQ(out.count, 0);
}

TEST_F(Paper, TheKeysLeadThroughThePaper) {
  using paper::Go;
  using paper::Key;
  const paper::Front f = front();
  paper::Place at;
  ASSERT_EQ(paper::rows(f), 7);  // four sections, Play, Book, Library

  // Up from the first row goes to the last, and Down from there comes back.
  EXPECT_EQ(paper::press(at, f, 0, Key::Up), Go::Move);
  EXPECT_EQ(at.row, 6);
  EXPECT_EQ(paper::press(at, f, 0, Key::Confirm), Go::Library);
  EXPECT_EQ(paper::press(at, f, 0, Key::Up), Go::Move);
  EXPECT_EQ(paper::press(at, f, 0, Key::Confirm), Go::Book);
  EXPECT_EQ(paper::press(at, f, 0, Key::Up), Go::Move);
  EXPECT_EQ(paper::press(at, f, 0, Key::Confirm), Go::Play);
  EXPECT_EQ(paper::press(at, f, 0, Key::Down), Go::Move);
  EXPECT_EQ(paper::press(at, f, 0, Key::Down), Go::Move);
  EXPECT_EQ(paper::press(at, f, 0, Key::Down), Go::Move);
  EXPECT_EQ(at.row, 0);
  // Left and Right do nothing on the front page.
  EXPECT_EQ(paper::press(at, f, 0, Key::Right), Go::Stay);

  // Confirm opens the section; Down moves among its pieces; Confirm opens one.
  EXPECT_EQ(paper::press(at, f, 0, Key::Confirm), Go::Section);
  EXPECT_EQ(at.screen, paper::Screen::Section);
  EXPECT_EQ(at.section, 0);
  EXPECT_EQ(paper::press(at, f, 2, Key::Down), Go::Move);
  EXPECT_EQ(at.piece, 1);
  EXPECT_EQ(paper::press(at, f, 2, Key::Confirm), Go::Piece);

  // Right leafs to the next section, on its first piece; Left at the first does nothing.
  EXPECT_EQ(paper::press(at, f, 2, Key::Right), Go::Section);
  EXPECT_EQ(at.section, 1);
  EXPECT_EQ(at.piece, 0);
  // Football is a section like the others, and its matches and tables are pages after it.
  EXPECT_EQ(paper::press(at, f, 1, Key::Right), Go::Section);
  EXPECT_EQ(at.section, 2);
  EXPECT_EQ(paper::press(at, f, 0, Key::Right), Go::Football);
  EXPECT_FALSE(at.fromRight);
  // Right out of the tables goes on to Film; Left from Film comes back onto the last table.
  EXPECT_EQ(paper::leaveFootball(at, f, true), Go::Section);
  EXPECT_EQ(at.section, 3);
  EXPECT_EQ(paper::press(at, f, 0, Key::Left), Go::Football);
  EXPECT_TRUE(at.fromRight);
  EXPECT_EQ(at.section, 2);
  // Left out of the tables returns to Football's pieces.
  EXPECT_EQ(paper::leaveFootball(at, f, false), Go::Section);
  EXPECT_EQ(at.section, 2);

  // Back from a section returns to the front page on that section's row.
  at.screen = paper::Screen::Section;
  at.section = 1;
  EXPECT_EQ(paper::press(at, f, 1, Key::Back), Go::Front);
  EXPECT_EQ(at.screen, paper::Screen::Front);
  EXPECT_EQ(at.row, 1);
  at.screen = paper::Screen::Section;
  at.section = 0;
  EXPECT_EQ(paper::press(at, f, 2, Key::Left), Go::Stay);
  // An empty section has nothing to open.
  at.section = 3;
  EXPECT_EQ(paper::press(at, f, 0, Key::Confirm), Go::Stay);
}

namespace {

std::string contents(const char* path) {
  const auto it = fake::files.find(path);
  return it == fake::files.end() ? "" : std::string(it->second->bytes.begin(), it->second->bytes.end());
}

std::vector<std::string> keptTitles(LineBuffer& buffer, uint8_t* unfinished = nullptr) {
  readlist::Entry entries[paper::KEPT_MAX];
  uint8_t lead = 0;
  const uint8_t n = paper::loadKept(buffer, entries, paper::KEPT_MAX, lead);
  if (unfinished) *unfinished = lead;
  std::vector<std::string> out;
  for (uint8_t i = 0; i < n; ++i) out.emplace_back(entries[i].title);
  return out;
}

// The next day's issue: one new piece in Hungary, nothing else listed.
const char NEXT_ISSUE[] =
    R"({"date":"2026-11-13","sections":[{"id":"hungary","name":"Hungary"}]})"
    "\n"
    R"({"id":"r-9","section":"hungary","kind":"news","title":"Tomorrow's headline","book":"books/read/r-9.epub"})"
    "\n";

}  // namespace

TEST_F(Paper, AHoldKeepsAPieceAndASecondLetsItGo) {
  paper::Piece pieces[paper::PIECES_MAX];
  ASSERT_EQ(paper::loadSection(*buffer, "hungary", pieces, paper::PIECES_MAX), 2);
  EXPECT_FALSE(pieces[0].kept);

  bool kept = false;
  ASSERT_TRUE(paper::toggleKept(*buffer, pieces[0], kept));
  EXPECT_TRUE(kept);
  ASSERT_EQ(paper::loadSection(*buffer, "hungary", pieces, paper::PIECES_MAX), 2);
  EXPECT_TRUE(pieces[0].kept);
  EXPECT_FALSE(pieces[1].kept);
  EXPECT_EQ(keptTitles(*buffer), std::vector<std::string>{"A long Hungarian headline"});

  ASSERT_TRUE(paper::toggleKept(*buffer, pieces[0], kept));
  EXPECT_FALSE(kept);
  EXPECT_TRUE(keptTitles(*buffer).empty());
  // Each hold is a line in the log the Mac pulls.
  const std::string log = contents(paths::KEPT_LOG);
  EXPECT_NE(log.find(R"({"id":"r-1","kept":true,)"), std::string::npos);
  EXPECT_NE(log.find(R"({"id":"r-1","kept":false,)"), std::string::npos);
}

TEST_F(Paper, ANewIssueTakesWhatWasNotKept) {
  paper::settle(*buffer, front());
  paper::Piece pieces[paper::PIECES_MAX];
  ASSERT_EQ(paper::loadSection(*buffer, "hungary", pieces, paper::PIECES_MAX), 2);
  bool kept = false;
  ASSERT_TRUE(paper::toggleKept(*buffer, pieces[0], kept));
  // The same issue seen again changes nothing.
  paper::settle(*buffer, front());
  EXPECT_TRUE(fake::files.contains("/books/read/r-2.epub"));

  fake::add(paths::READ_LIST, NEXT_ISSUE);
  fake::add("/books/read/r-9.epub", "x");
  paper::settle(*buffer, front());
  EXPECT_TRUE(fake::files.contains("/books/read/r-1.epub"));   // kept
  EXPECT_TRUE(fake::files.contains("/books/read/r-9.epub"));   // listed
  EXPECT_FALSE(fake::files.contains("/books/read/r-2.epub"));  // unread and not kept: gone
  EXPECT_FALSE(fake::files.contains("/books/read/r-3.epub"));
  // The link that arrived kept is still there, under the piece kept after it.
  EXPECT_EQ(keptTitles(*buffer), (std::vector<std::string>{"A long Hungarian headline", "A saved link"}));

  // Finished, a kept piece stays until the next issue, at the bottom; then it leaves.
  readlist::onBookClosed("/books/read/r-1.epub", 1.0f, true, 60000);
  uint8_t unfinished = 9;
  EXPECT_EQ(keptTitles(*buffer, &unfinished), (std::vector<std::string>{"A saved link", "A long Hungarian headline"}));
  EXPECT_EQ(unfinished, 1);
  std::string later = NEXT_ISSUE;
  later.replace(later.find("2026-11-13"), 10, "2026-11-14");
  fake::add(paths::READ_LIST, later);
  paper::settle(*buffer, front());
  EXPECT_FALSE(fake::files.contains("/books/read/r-1.epub"));
  EXPECT_EQ(keptTitles(*buffer), std::vector<std::string>{"A saved link"});
}

TEST_F(Paper, ASavedLinkArrivesKeptAndLetGoStaysGone) {
  paper::settle(*buffer, front());
  EXPECT_EQ(keptTitles(*buffer), std::vector<std::string>{"A saved link"});

  bool kept = true;
  ASSERT_TRUE(paper::toggleKeptBook(*buffer, "/books/read/r-5.epub", kept));
  EXPECT_FALSE(kept);
  // The issue still lists it: it does not arrive a second time.
  paper::settle(*buffer, front());
  EXPECT_TRUE(keptTitles(*buffer).empty());
  // Held again while it is still on the card, it is kept again.
  ASSERT_TRUE(paper::toggleKeptBook(*buffer, "books/read/r-5.epub", kept));
  EXPECT_TRUE(kept);
  ASSERT_TRUE(paper::toggleKeptBook(*buffer, "books/read/r-5.epub", kept));

  fake::add(paths::READ_LIST, NEXT_ISSUE);
  fake::add("/books/read/r-9.epub", "x");
  paper::settle(*buffer, front());
  EXPECT_FALSE(fake::files.contains("/books/read/r-5.epub"));
}

TEST_F(Paper, TheNewestKeptIsOnTopAndThirtyTwoIsTheLimit) {
  paper::Piece piece;
  bool kept = false;
  for (int i = 0; i < paper::KEPT_MAX; ++i) {
    piece = paper::Piece{};
    snprintf(piece.id, sizeof(piece.id), "r-k%d", i);
    snprintf(piece.title, sizeof(piece.title), "Kept %d", i);
    snprintf(piece.book, sizeof(piece.book), "books/read/r-k%d.epub", i);
    fake::add(std::string("/") + piece.book, "x");
    ASSERT_TRUE(paper::toggleKept(*buffer, piece, kept)) << i;
  }
  const std::vector<std::string> titles = keptTitles(*buffer);
  ASSERT_EQ(titles.size(), paper::KEPT_MAX);
  EXPECT_EQ(titles.front(), "Kept 31");
  EXPECT_EQ(titles.back(), "Kept 0");

  paper::Piece pieces[paper::PIECES_MAX];
  ASSERT_EQ(paper::loadSection(*buffer, "hungary", pieces, paper::PIECES_MAX), 2);
  EXPECT_FALSE(paper::toggleKept(*buffer, pieces[0], kept));
  EXPECT_FALSE(kept);
}

TEST_F(Paper, FootballsTabsNameTheCompetitionsOfItsTables) {
  char comps[paper::COMPETITIONS_MAX][paper::COMPETITION_BYTES + 1];
  EXPECT_EQ(paper::loadCompetitions(comps, paper::COMPETITIONS_MAX), 0);  // no football.json yet
  fake::add(x4::paths::FOOTBALL_JSON,
            R"({"fetched":1,"matches":[{"comp":"FA"}],"tables":[{"comp":"PL","rows":[{"pos":1,"team":"Arsenal","points":30}]},)"
            R"({"comp":"","rows":[]},{"comp":"PD","rows":[]},{"comp":"SA"},{"comp":"CL"},{"comp":"EL"}]})");
  ASSERT_EQ(paper::loadCompetitions(comps, paper::COMPETITIONS_MAX), 4);
  EXPECT_STREQ(comps[0], "PL");
  EXPECT_STREQ(comps[1], "PD");  // a table without a name has no tab
  EXPECT_STREQ(comps[3], "CL");  // four at most
}
