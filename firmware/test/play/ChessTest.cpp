#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "play/games/Chess.h"

using namespace x4::play::chess;

namespace {

Position fen(const char* text) {
  Position pos;
  EXPECT_TRUE(parseFen(text, std::strlen(text), pos)) << text;
  return pos;
}

std::string fenOf(const Position& pos) {
  char out[FEN_MAX];
  toFen(pos, out, sizeof(out));
  return out;
}

uint8_t sq(const char* name) { return static_cast<uint8_t>((name[1] - '1') * 8 + (name[0] - 'a')); }

// The legal targets of the piece on `from`, as sorted square names.
std::set<std::string> moves(const Position& pos, const char* from) {
  std::set<std::string> out;
  const uint64_t bits = targets(pos, sq(from));
  for (uint8_t s = 0; s < SQUARES; ++s) {
    if ((bits >> s) & 1)
      out.insert(std::string{static_cast<char>('a' + fileOf(s)), static_cast<char>('1' + rankOf(s))});
  }
  return out;
}

Move uci(const char* text) {
  Move m;
  EXPECT_TRUE(parseUci(text, std::strlen(text), m)) << text;
  return m;
}

int countMoves(const Position& pos) {
  int n = 0;
  for (uint8_t s = 0; s < SQUARES; ++s) n += __builtin_popcountll(targets(pos, s));
  return n;
}

Puzzle puzzle(const char* line) {
  Puzzle p;
  EXPECT_TRUE(parsePuzzle(line, std::strlen(line), p)) << line;
  return p;
}

constexpr char START[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
// The example on database.lichess.org: Black's king steps up, White mates in two.
constexpr char LICHESS[] = "00sHx,1760,q3k1nr/1pp1nQpp/3p4/1P2p3/4P3/B1PP1b2/B5PP/5K2 b k - 0 17,e8d7 a2e6 d7d8 f7f8";

}  // namespace

// ---- FEN ----------------------------------------------------------------------------

TEST(ChessTest, FenRoundTrip) {
  for (const char* text : {START, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                           "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 41",
                           "q3k1nr/1pp1nQpp/3p4/1P2p3/4P3/B1PP1b2/B5PP/5K2 b k - 0 17"}) {
    EXPECT_EQ(fenOf(fen(text)), text);
  }
  // Move counts may be left out.
  EXPECT_EQ(fenOf(fen("4k3/8/8/8/8/8/8/4K3 b - -")), "4k3/8/8/8/8/8/8/4K3 b - - 0 1");
}

TEST(ChessTest, FenRejectsBadPositions) {
  Position pos;
  for (const char* text : {"", "8/8/8/8/8/8/8/8 w - - 0 1",      // no kings
                           "4k3/8/8/8/8/8/8/4K2 w - - 0 1",      // short rank
                           "4k3/8/8/8/8/8/8/4K3 x - - 0 1",      // side
                           "4k3/8/8/8/8/8/8/4KK2 w - - 0 1",     // two kings
                           "4k3/8/8/8/8/8/8/P3K3 w - - 0 1",     // pawn on the first rank
                           "4k3/8/8/3pP3/8/8/8/4K3 w - d3 0 1",  // en passant on the wrong side
                           "4k3/8/8/8/8/8/8/4K3 w - - 0 1 extra", "4k3/8/8/8/8/8/8/4K3 w Z - 0 1"}) {
    EXPECT_FALSE(parseFen(text, std::strlen(text), pos)) << text;
  }
  // Rights without the rook in its corner are dropped.
  EXPECT_EQ(fen("4k3/8/8/8/8/8/8/4K3 w KQkq - 0 1").castling, 0);
}

TEST(ChessTest, UciRoundTrip) {
  char out[6];
  for (const char* text : {"e2e4", "a7a8q", "h2h1n", "e1g1"}) {
    toUci(uci(text), out);
    EXPECT_STREQ(out, text);
  }
  Move m;
  for (const char* text : {"e2e", "e2e4x", "e2e4Q", "i2e4", "e2e2", "e7e8k", "e7e8p"}) {
    EXPECT_FALSE(parseUci(text, std::strlen(text), m)) << text;
  }
}

// ---- moves -------------------------------------------------------------------------

TEST(ChessTest, StartingPositionHasTwentyMoves) {
  const Position pos = fen(START);
  EXPECT_EQ(countMoves(pos), 20);
  EXPECT_EQ(moves(pos, "g1"), (std::set<std::string>{"f3", "h3"}));
  EXPECT_EQ(moves(pos, "e2"), (std::set<std::string>{"e3", "e4"}));
  EXPECT_TRUE(moves(pos, "e7").empty());  // not the side to move
  EXPECT_TRUE(moves(pos, "e4").empty());  // empty square
}

// Perft 1 of the standard test positions (chessprogramming.org/Perft_Results).
TEST(ChessTest, MoveCountsOfKnownPositions) {
  EXPECT_EQ(countMoves(fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1")), 48);
  EXPECT_EQ(countMoves(fen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1")), 14);
  EXPECT_EQ(countMoves(fen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1")), 6);
  // Perft counts dxc8 four times, once per promotion piece: 44 there.
  EXPECT_EQ(countMoves(fen("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8")), 41);
}

TEST(ChessTest, CastlingNeedsSafeEmptySquares) {
  // Both ways open.
  Position pos = fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
  EXPECT_EQ(moves(pos, "e1"), (std::set<std::string>{"c1", "d1", "d2", "e2", "f1", "f2", "g1"}));
  // A rook on f8 covers f1: no castling through it; queenside still fine.
  pos = fen("r4rk1/8/8/8/8/8/8/R3K2R w KQ - 0 1");
  EXPECT_EQ(moves(pos, "e1").count("g1"), 0u);
  EXPECT_EQ(moves(pos, "e1").count("c1"), 1u);
  // b1 may be attacked (the king does not cross it), d1 may not.
  pos = fen("1r2k3/8/8/8/8/8/8/R3K3 w Q - 0 1");
  EXPECT_EQ(moves(pos, "e1").count("c1"), 1u);
  pos = fen("3rk3/8/8/8/8/8/8/R3K3 w Q - 0 1");
  EXPECT_EQ(moves(pos, "e1").count("c1"), 0u);
  // Not out of check.
  pos = fen("4r1k1/8/8/8/8/8/8/R3K2R w KQ - 0 1");
  EXPECT_EQ(moves(pos, "e1").count("g1"), 0u);
  EXPECT_EQ(moves(pos, "e1").count("c1"), 0u);
  // Not through a piece, nor without the right.
  pos = fen("4k3/8/8/8/8/8/8/RN2K1NR w KQ - 0 1");
  EXPECT_EQ(moves(pos, "e1").count("g1"), 0u);
  EXPECT_EQ(moves(pos, "e1").count("c1"), 0u);
  pos = fen("4k3/8/8/8/8/8/8/R3K2R w Q - 0 1");
  EXPECT_EQ(moves(pos, "e1").count("g1"), 0u);

  // Castling moves the rook; moving a rook or the king loses rights.
  pos = fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
  apply(pos, uci("e1g1"));
  EXPECT_EQ(fenOf(pos), "r3k2r/8/8/8/8/8/8/R4RK1 b kq - 1 1");
  apply(pos, uci("a8b8"));
  EXPECT_EQ(fenOf(pos), "1r2k2r/8/8/8/8/8/8/R4RK1 w k - 2 2");
  apply(pos, uci("f1f8"));  // check
  apply(pos, uci("e8e7"));
  EXPECT_EQ(pos.castling, 0);
  // Taking a rook in its corner removes that right.
  pos = fen("r3k2r/8/8/8/8/8/6B1/4K3 w kq - 0 1");
  apply(pos, uci("g2a8"));
  EXPECT_EQ(pos.castling, BLACK_KINGSIDE);
}

TEST(ChessTest, EnPassant) {
  Position pos = fen("4k3/3p4/8/4P3/8/8/8/4K3 b - - 0 1");
  apply(pos, uci("d7d5"));
  EXPECT_EQ(fenOf(pos), "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2");
  EXPECT_EQ(moves(pos, "e5"), (std::set<std::string>{"d6", "e6"}));
  apply(pos, uci("e5d6"));
  EXPECT_EQ(fenOf(pos), "4k3/8/3P4/8/8/8/8/4K3 b - - 0 2");
  // Only right after the double step.
  pos = fen("4k3/8/8/3pP3/8/8/8/4K3 w - - 0 1");
  EXPECT_EQ(moves(pos, "e5"), (std::set<std::string>{"e6"}));
  // Taking en passant would open the rank to the rook: illegal.
  pos = fen("8/8/8/KPp4r/8/8/8/7k w - c6 0 1");
  EXPECT_EQ(moves(pos, "b5"), (std::set<std::string>{"b6"}));
}

TEST(ChessTest, PinsAndChecks) {
  // The knight on e2 is pinned to the king by the rook: it cannot move.
  Position pos = fen("4r1k1/8/8/8/8/8/4N3/4K3 w - - 0 1");
  EXPECT_TRUE(moves(pos, "e2").empty());
  // A pinned rook slides along the pin line.
  pos = fen("4r1k1/8/8/8/8/8/4R3/4K3 w - - 0 1");
  EXPECT_EQ(moves(pos, "e2"), (std::set<std::string>{"e3", "e4", "e5", "e6", "e7", "e8"}));
  // In check, only moves that answer it.
  pos = fen("4r1k1/8/8/8/8/8/3B4/R3K3 w - - 0 1");
  EXPECT_TRUE(inCheck(pos));
  EXPECT_EQ(moves(pos, "d2"), (std::set<std::string>{"e3"}));
  EXPECT_TRUE(moves(pos, "a1").empty());
  EXPECT_EQ(moves(pos, "e1"), (std::set<std::string>{"d1", "f1", "f2"}));
  // The king may not step into an attack or take a guarded piece.
  pos = fen("4k3/8/8/8/8/2b5/3q4/4K3 w - - 0 1");
  EXPECT_EQ(moves(pos, "e1"), (std::set<std::string>{"f1"}));
  // Mate and stalemate.
  EXPECT_TRUE(checkmate(fen("R5k1/5ppp/8/8/8/8/8/6K1 b - - 0 1")));
  const Position stale = fen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
  EXPECT_FALSE(inCheck(stale));
  EXPECT_FALSE(hasLegalMove(stale));
  EXPECT_FALSE(checkmate(stale));
}

TEST(ChessTest, Promotion) {
  Position pos = fen("8/4P1k1/8/8/8/8/8/4K3 w - - 0 1");
  EXPECT_TRUE(promotes(pos, sq("e7"), sq("e8")));
  EXPECT_FALSE(isLegal(pos, Move{sq("e7"), sq("e8"), EMPTY}));  // needs a piece
  EXPECT_TRUE(isLegal(pos, uci("e7e8n")));
  EXPECT_FALSE(isLegal(pos, Move{sq("e1"), sq("e2"), QUEEN}));  // only pawns promote
  Position knight = pos;
  apply(knight, uci("e7e8n"));
  EXPECT_EQ(fenOf(knight), "4N3/6k1/8/8/8/8/8/4K3 b - - 0 1");
  apply(pos, Move{sq("e7"), sq("e8"), EMPTY});  // a queen by default
  EXPECT_EQ(fenOf(pos), "4Q3/6k1/8/8/8/8/8/4K3 b - - 0 1");
  // Black promotes on the first rank, taking.
  pos = fen("4k3/8/8/8/8/8/1p6/R3K3 b Q - 0 1");
  EXPECT_EQ(moves(pos, "b2"), (std::set<std::string>{"a1", "b1"}));
  apply(pos, uci("b2a1r"));
  EXPECT_EQ(fenOf(pos), "4k3/8/8/8/8/8/8/r3K3 w - - 0 2");
}

// ---- puzzles --------------------------------------------------------------------------

TEST(ChessTest, PlaysALichessLine) {
  const Puzzle p = puzzle(LICHESS);
  EXPECT_STREQ(p.id, "00sHx");
  EXPECT_EQ(p.rating, 1760);
  ASSERT_EQ(p.count, 4);
  Position pos = p.start;
  apply(pos, p.moves[0]);   // the device's first move
  EXPECT_FALSE(pos.black);  // White finds the answer
  Move m{sq("a2"), sq("e6"), EMPTY};
  EXPECT_EQ(judge(p, 1, pos, m), Verdict::Right);
  apply(pos, m);
  apply(pos, p.moves[2]);
  Move wrong{sq("f7"), sq("g8"), EMPTY};
  EXPECT_EQ(judge(p, 3, pos, wrong), Verdict::Wrong);
  m = {sq("f7"), sq("f8"), EMPTY};
  EXPECT_EQ(judge(p, 3, pos, m), Verdict::Right);
  apply(pos, m);
  EXPECT_TRUE(checkmate(pos));
}

TEST(ChessTest, AnyMateCounts) {
  // Two rooks, two back-rank mates; the line lists Ra8.
  const Puzzle p = puzzle("mate2,900,6k1/5ppp/8/8/8/8/8/RR4K1 b - - 0 1,g8h8 a1a8");
  Position pos = p.start;
  apply(pos, p.moves[0]);
  Move other{sq("b1"), sq("b8"), EMPTY};
  EXPECT_EQ(judge(p, 1, pos, other), Verdict::Mate);
  Move listed{sq("a1"), sq("a8"), EMPTY};
  EXPECT_EQ(judge(p, 1, pos, listed), Verdict::Right);
  Move check{sq("a1"), sq("a7"), EMPTY};
  EXPECT_EQ(judge(p, 1, pos, check), Verdict::Wrong);
  Move illegal{sq("a1"), sq("b2"), EMPTY};
  EXPECT_EQ(judge(p, 1, pos, illegal), Verdict::Wrong);
}

TEST(ChessTest, PromotionTakesTheListedPiece) {
  // Underpromotion: the line takes on d8 with a knight; going there makes that knight.
  const Puzzle p = puzzle("under,1500,3q1k2/4P3/8/8/8/8/6K1/8 b - - 0 1,f8g7 e7d8n");
  Position pos = p.start;
  apply(pos, p.moves[0]);
  Move m{sq("e7"), sq("d8"), EMPTY};
  EXPECT_EQ(judge(p, 1, pos, m), Verdict::Right);
  EXPECT_EQ(m.promo, KNIGHT);
  // Elsewhere a pawn becomes a queen.
  Move push{sq("e7"), sq("e8"), EMPTY};
  EXPECT_EQ(judge(p, 1, pos, push), Verdict::Wrong);
  EXPECT_EQ(push.promo, QUEEN);
}

TEST(ChessTest, RejectsBadLines) {
  Puzzle p;
  for (const char* line : {"", "id,1500,8/8/8/8/8/8/8/8 w - - 0 1,e2e4 e7e5",    // no kings
                           "id,1500,4k3/8/8/8/8/8/4P3/4K3 w - - 0 1,e2e5 e8e7",  // illegal move
                           "id,1500,4k3/8/8/8/8/8/4P3/4K3 w - - 0 1,e2e4",       // odd count
                           "id,15x0,4k3/8/8/8/8/8/4P3/4K3 w - - 0 1,e2e4 e8e7",  // rating
                           "waytoolongpuzzleid,1500,4k3/8/8/8/8/8/4P3/4K3 w - - 0 1,e2e4 e8e7",
                           "id,1500,4k3/8/8/8/8/8/4P3/4K3 w - - 0 1"}) {
    EXPECT_FALSE(parsePuzzle(line, std::strlen(line), p)) << line;
  }
  EXPECT_TRUE(parsePuzzle(LICHESS, std::strlen(LICHESS), p));
}
