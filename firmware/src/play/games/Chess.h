#pragma once

#include <cstddef>
#include <cstdint>

// Chess rules for the tactics game: FEN, UCI moves and legal moves. Pure C++,
// no allocation: a Position is 70 bytes and is copied to test a move.
// Squares are 0-63, a1 = 0, h1 = 7, a8 = 56.
namespace x4::play::chess {

inline constexpr uint8_t SQUARES = 64;
inline constexpr uint8_t NO_SQUARE = 64;

// A piece is its type, plus BLACK for Black's.
enum Type : uint8_t { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
inline constexpr uint8_t BLACK = 8;
inline uint8_t typeOf(const uint8_t piece) { return piece & 7; }
inline bool isBlack(const uint8_t piece) { return (piece & BLACK) != 0; }

inline uint8_t fileOf(const uint8_t square) { return square & 7; }
inline uint8_t rankOf(const uint8_t square) { return square >> 3; }

// Castling rights.
inline constexpr uint8_t WHITE_KINGSIDE = 1;
inline constexpr uint8_t WHITE_QUEENSIDE = 2;
inline constexpr uint8_t BLACK_KINGSIDE = 4;
inline constexpr uint8_t BLACK_QUEENSIDE = 8;

struct Move {
  uint8_t from = 0;
  uint8_t to = 0;
  uint8_t promo = EMPTY;  // KNIGHT to QUEEN, or EMPTY

  bool operator==(const Move& other) const { return from == other.from && to == other.to && promo == other.promo; }
};

struct Position {
  uint8_t board[SQUARES] = {};
  uint8_t black = 0;  // Black to move
  uint8_t castling = 0;
  uint8_t ep = NO_SQUARE;  // the square a pawn passed over, or NO_SQUARE
  uint8_t halfmove = 0;
  uint16_t fullmove = 1;
};

inline constexpr size_t FEN_MAX = 92;  // the longest FEN, with its NUL

// A FEN; the halfmove and fullmove counts may be left out. False when it is
// malformed or a side has no king or more than one.
bool parseFen(const char* text, size_t length, Position& out);
// Writes the FEN with its NUL; returns its length.
size_t toFen(const Position& pos, char* out, size_t size);

// "e2e4", "e7e8q". Castling is the king's two-square move.
bool parseUci(const char* text, size_t length, Move& out);
// Writes 4 or 5 characters and a NUL; `out` holds at least 6.
void toUci(const Move& move, char* out);

// Whether a piece of the given side attacks `square`.
bool attacked(const Position& pos, uint8_t square, bool byBlack);
// Whether the side to move is in check.
bool inCheck(const Position& pos);

// The squares the piece on `from` can legally move to, one bit per square.
// Empty when that square has no piece of the side to move.
uint64_t targets(const Position& pos, uint8_t from);
// A pawn's move to the last rank needs a promotion piece; every other move none.
bool isLegal(const Position& pos, const Move& move);
// Plays a legal move. A pawn reaching the last rank without a promotion
// piece becomes a queen.
void apply(Position& pos, Move move);

bool hasLegalMove(const Position& pos);
bool checkmate(const Position& pos);
// The move promotes: a pawn of the side to move goes to the last rank.
bool promotes(const Position& pos, uint8_t from, uint8_t to);

// ---- puzzles ----------------------------------------------------------------------

inline constexpr size_t ID_MAX = 15;
inline constexpr uint8_t MOVES_MAX = 16;        // plies, the opponent's first move included
inline constexpr size_t PUZZLE_LINE_MAX = 256;  // bytes of a puzzles.txt line

// One line of /x4/chess/puzzles.txt: "<id>,<rating>,<fen>,<moves>" (docs/DATA.md).
// The FEN is the position before the opponent's move moves[0]; the player
// answers with moves[1], the opponent plays moves[2], and so on. Every move
// is checked legal; the line has an even number of moves, at least two.
struct Puzzle {
  char id[ID_MAX + 1] = {};
  uint16_t rating = 0;
  Position start;
  Move moves[MOVES_MAX];
  uint8_t count = 0;
};
bool parsePuzzle(const char* line, size_t length, Puzzle& out);

// How the player's move at `ply` (odd) of `puzzle`, from `pos`, is taken:
// the listed move, or any move that mates (the Lichess rule). `move.promo`
// is filled in for a pawn reaching the last rank: the listed move's piece
// when it goes the same way, else a queen.
enum class Verdict : uint8_t { Wrong, Right, Mate };
Verdict judge(const Puzzle& puzzle, uint8_t ply, const Position& pos, Move& move);

}  // namespace x4::play::chess
