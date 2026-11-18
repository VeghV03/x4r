#pragma once

#include <cstddef>
#include <cstdint>

#include "JsonlReader.h"
#include "ReadList.h"

// The newspaper (SPEC: The newspaper): the issue in /x4/read.jsonl as the front page and a
// section's screen read it, and where a key takes you between them. No drawing.
// Main-task only.
namespace x4::paper {

inline constexpr uint8_t SECTIONS_MAX = 8;
// Pieces on a section's screen.
inline constexpr uint8_t PIECES_MAX = 3;
inline constexpr size_t SECTION_ID_BYTES = 16;
inline constexpr size_t SECTION_NAME_BYTES = 24;
// 32 characters for the front page and 60 under a headline, with room for accents.
inline constexpr size_t HEAD_BYTES = 96;
inline constexpr size_t LEAD_BYTES = 180;
inline constexpr size_t SOURCE_BYTES = 48;
inline constexpr char FOOTBALL[] = "football";

struct Section {
  char id[SECTION_ID_BYTES + 1] = {};
  char name[SECTION_NAME_BYTES + 1] = {};
  // The headline of its first piece not read; empty when there is none.
  char head[HEAD_BYTES + 1] = {};
  uint8_t pieces = 0;  // on the card
};

struct Front {
  char date[11] = {};  // YYYY-MM-DD
  Section sections[SECTIONS_MAX];
  uint8_t count = 0;

  int find(const char* sectionId) const;
};

struct Piece {
  char id[ID_MAX + 1] = {};
  char source[SOURCE_BYTES + 1] = {};
  char title[readlist::TITLE_BYTES + 1] = {};
  char lead[LEAD_BYTES + 1] = {};
  char book[readlist::BOOK_BYTES + 1] = {};  // book key
  bool read = false;
  bool kept = false;
  uint16_t minutes = 0;
};

// The issue's sections, each with the headline of its first unread piece. False when the
// card has no issue: no Read list, or one without the header line.
bool loadFront(LineBuffer& buffer, Front& out);
// A section's pieces whose article is on the card, in file order. Returns how many.
uint8_t loadSection(LineBuffer& buffer, const char* sectionId, Piece* out, uint8_t max);

// Kept pieces (SPEC: The issue). A piece kept stays on the card, in the Library's Kept, when its
// issue is replaced; one finished or let go leaves with the next issue.
inline constexpr uint8_t KEPT_MAX = 32;
// Links that arrive kept in one issue.
inline constexpr uint8_t LINKS_MAX = 8;
// Brings the kept pieces up to the issue on the card. Links that arrive kept are added. When
// the issue is a new one, kept pieces finished or let go are dropped, and every article that
// neither the issue lists nor a kept piece needs is removed. Call where an issue is first
// seen: the front page.
void settle(LineBuffer& buffer, const Front& front);
// A hold of Confirm on a piece: keeps it, or lets a kept one go, and logs it. `kept` gets what
// it is now. False when 32 are kept already or the card could not be written.
bool toggleKept(LineBuffer& buffer, const Piece& piece, bool& kept);
// The same from where only the article is known (the reader, the Kept tab): a kept one is let
// go; one in the issue is kept. False as above, or when the article is neither.
bool toggleKeptBook(LineBuffer& buffer, const char* bookKey, bool& kept);
// Leaves piece `id`'s whole line in `buffer.text` (`length` gets its size): from the issue,
// else from where a kept piece keeps it. False when neither has it.
bool loadPieceLine(LineBuffer& buffer, const char* id, size_t& length);
// The same by its article. `id` gets the piece's id.
bool loadPieceLineOf(LineBuffer& buffer, const char* bookKey, size_t& length, char (&id)[ID_MAX + 1]);
// Whether the article is a piece that carries a link or a forecast: then no card follows it.
bool carriesMore(LineBuffer& buffer, const char* bookKey);
// Whether the article is a kept piece's.
bool isKept(LineBuffer& buffer, const char* bookKey);
// The kept pieces whose article is on the card: the unfinished first, the newest kept on top,
// then the finished. `unfinished` gets how many lead. For the Library's Kept tab.
uint8_t loadKept(LineBuffer& buffer, readlist::Entry* out, uint8_t max, uint8_t& unfinished);

// Where the paper stands and what a key does there.
enum class Key : uint8_t { Back, Confirm, Left, Right, Up, Down };
enum class Screen : uint8_t { Front, Section };
// The front page's rows below the sections, in order.
enum class Fixed : uint8_t { Play, Book, Library, COUNT };

struct Place {
  Screen screen = Screen::Front;
  uint8_t row = 0;      // front page: a section, then Play, Book, Library
  uint8_t section = 0;  // the section open
  uint8_t piece = 0;    // selected on its screen
  bool fromRight = false;  // Football's tables were leafed into from the section after them
};

enum class Go : uint8_t {
  Stay,      // nothing changes
  Move,      // the selection moved on the same screen
  Front,     // the front page is drawn
  Section,   // a section's screen is drawn: `place.section`
  Piece,     // the reader opens the selected piece
  Football,  // Football's matches and tables, the pages after its pieces: `place.fromRight`
  Play,
  Book,
  Library,
};

// Rows on the front page.
inline uint8_t rows(const Front& front) { return front.count + static_cast<uint8_t>(Fixed::COUNT); }
// What `key` does where `place` stands. `pieces` is how many the open section shows.
Go press(Place& place, const Front& front, uint8_t pieces, Key key);
// Leafing out of Football's matches and tables: Left returns to its pieces, Right goes on to
// the next section (Stay when Football is the last).
Go leaveFootball(Place& place, const Front& front, bool right);

// Football's tabs (D44): Analysis, its pieces; Matches; then one per competition's table.
inline constexpr uint8_t COMPETITIONS_MAX = 4;
inline constexpr size_t COMPETITION_BYTES = 12;
inline constexpr uint8_t FOOTBALL_TABS_MAX = 2 + COMPETITIONS_MAX;
// The competitions' short names ("PL") in /x4/football.json's order; none without the file.
uint8_t loadCompetitions(char (*out)[COMPETITION_BYTES + 1], uint8_t max);

}  // namespace x4::paper
