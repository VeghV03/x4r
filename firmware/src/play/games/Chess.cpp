#include "Chess.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace x4::play::chess {

namespace {

constexpr int8_t KNIGHT_STEPS[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
constexpr int8_t KING_STEPS[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
constexpr int8_t ROOK_DIRS[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
constexpr int8_t BISHOP_DIRS[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};

constexpr char PIECE_LETTERS[] = " pnbrqk";

uint8_t colorOf(const bool black) { return black ? BLACK : 0; }

// The square at file f, rank r, or NO_SQUARE off the board.
uint8_t at(const int f, const int r) {
  return f < 0 || f > 7 || r < 0 || r > 7 ? NO_SQUARE : static_cast<uint8_t>(r * 8 + f);
}

bool own(const Position& pos, const uint8_t piece) { return piece != EMPTY && isBlack(piece) == (pos.black != 0); }

uint8_t typeFromLetter(const char c) {
  const char* p = std::strchr(PIECE_LETTERS + 1, c | 0x20);
  return c && p ? static_cast<uint8_t>(p - PIECE_LETTERS) : EMPTY;
}

uint8_t kingSquare(const Position& pos, const bool black) {
  const uint8_t king = KING | colorOf(black);
  for (uint8_t s = 0; s < SQUARES; ++s) {
    if (pos.board[s] == king) return s;
  }
  return NO_SQUARE;
}

// Whether a sliding piece of `pieceA` or `pieceB` sees `square` along `dirs`.
bool slides(const Position& pos, const uint8_t square, const int8_t (&dirs)[4][2], const uint8_t pieceA,
            const uint8_t pieceB) {
  for (const auto& d : dirs) {
    int f = fileOf(square) + d[0];
    int r = rankOf(square) + d[1];
    for (uint8_t s = at(f, r); s != NO_SQUARE; s = at(f += d[0], r += d[1])) {
      const uint8_t p = pos.board[s];
      if (p == EMPTY) continue;
      if (p == pieceA || p == pieceB) return true;
      break;
    }
  }
  return false;
}

// Destinations by the movement rules alone, before checking the own king.
uint64_t pseudoTargets(const Position& pos, const uint8_t from) {
  const uint8_t piece = pos.board[from];
  if (!own(pos, piece)) return 0;
  const bool black = isBlack(piece);
  const int f = fileOf(from);
  const int r = rankOf(from);
  uint64_t out = 0;
  const auto add = [&](const uint8_t s) {
    if (s != NO_SQUARE && !own(pos, pos.board[s])) out |= 1ULL << s;
  };
  switch (typeOf(piece)) {
    case PAWN: {
      const int dir = black ? -1 : 1;
      const uint8_t one = at(f, r + dir);
      if (one != NO_SQUARE && pos.board[one] == EMPTY) {
        out |= 1ULL << one;
        const uint8_t two = at(f, r + 2 * dir);
        if (r == (black ? 6 : 1) && pos.board[two] == EMPTY) out |= 1ULL << two;
      }
      for (const int df : {-1, 1}) {
        const uint8_t s = at(f + df, r + dir);
        if (s == NO_SQUARE) continue;
        const uint8_t p = pos.board[s];
        if ((p != EMPTY && isBlack(p) != black) || s == pos.ep) out |= 1ULL << s;
      }
      break;
    }
    case KNIGHT:
      for (const auto& d : KNIGHT_STEPS) add(at(f + d[0], r + d[1]));
      break;
    case KING: {
      for (const auto& d : KING_STEPS) add(at(f + d[0], r + d[1]));
      // Castling: the right, the rook in its corner, empty squares between,
      // and the king neither in check nor crossing an attacked square.
      const uint8_t home = black ? 60 : 4;
      if (from != home || attacked(pos, from, !black)) break;
      const uint8_t rook = ROOK | colorOf(black);
      const uint8_t kingside = black ? BLACK_KINGSIDE : WHITE_KINGSIDE;
      const uint8_t queenside = black ? BLACK_QUEENSIDE : WHITE_QUEENSIDE;
      if ((pos.castling & kingside) && pos.board[home + 3] == rook && pos.board[home + 1] == EMPTY &&
          pos.board[home + 2] == EMPTY && !attacked(pos, home + 1, !black) && !attacked(pos, home + 2, !black)) {
        out |= 1ULL << (home + 2);
      }
      if ((pos.castling & queenside) && pos.board[home - 4] == rook && pos.board[home - 1] == EMPTY &&
          pos.board[home - 2] == EMPTY && pos.board[home - 3] == EMPTY && !attacked(pos, home - 1, !black) &&
          !attacked(pos, home - 2, !black)) {
        out |= 1ULL << (home - 2);
      }
      break;
    }
    default: {
      const uint8_t type = typeOf(piece);
      const auto ray = [&](const int8_t (&dirs)[4][2]) {
        for (const auto& d : dirs) {
          int ff = f + d[0];
          int rr = r + d[1];
          for (uint8_t s = at(ff, rr); s != NO_SQUARE; s = at(ff += d[0], rr += d[1])) {
            add(s);
            if (pos.board[s] != EMPTY) break;
          }
        }
      };
      if (type == ROOK || type == QUEEN) ray(ROOK_DIRS);
      if (type == BISHOP || type == QUEEN) ray(BISHOP_DIRS);
      break;
    }
  }
  return out;
}

// Whether playing from-to leaves the mover's own king safe.
bool keepsKingSafe(const Position& pos, const uint8_t from, const uint8_t to) {
  Position next = pos;
  apply(next, Move{from, to, promotes(pos, from, to) ? QUEEN : EMPTY});
  const uint8_t king = kingSquare(next, pos.black != 0);
  return king != NO_SQUARE && !attacked(next, king, pos.black == 0);
}

bool readNumber(const char*& p, const char* end, uint32_t& out) {
  if (p >= end || *p < '0' || *p > '9') return false;
  out = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    out = out * 10 + static_cast<uint32_t>(*p++ - '0');
    if (out > 100000) return false;
  }
  return true;
}

}  // namespace

bool parseFen(const char* text, const size_t length, Position& out) {
  out = Position{};
  const char* p = text;
  const char* end = text + length;
  int rank = 7;
  int file = 0;
  for (; p < end && *p != ' '; ++p) {
    const char c = *p;
    if (c == '/') {
      if (file != 8 || rank == 0) return false;
      --rank;
      file = 0;
    } else if (c >= '1' && c <= '8') {
      file += c - '0';
      if (file > 8) return false;
    } else {
      const uint8_t type = typeFromLetter(c);
      if (type == EMPTY || file > 7) return false;
      out.board[rank * 8 + file++] = static_cast<uint8_t>(type | (c >= 'a' ? BLACK : 0));
    }
  }
  if (rank != 0 || file != 8 || p >= end) return false;
  ++p;
  if (p >= end || (*p != 'w' && *p != 'b')) return false;
  out.black = *p++ == 'b';
  if (p >= end || *p++ != ' ') return false;
  if (p < end && *p == '-') {
    ++p;
  } else {
    for (; p < end && *p != ' '; ++p) {
      switch (*p) {
        case 'K':
          out.castling |= WHITE_KINGSIDE;
          break;
        case 'Q':
          out.castling |= WHITE_QUEENSIDE;
          break;
        case 'k':
          out.castling |= BLACK_KINGSIDE;
          break;
        case 'q':
          out.castling |= BLACK_QUEENSIDE;
          break;
        default:
          return false;
      }
    }
  }
  if (p >= end || *p++ != ' ') return false;
  if (p < end && *p == '-') {
    ++p;
  } else {
    // The square behind the pawn that just moved two: rank 6 when White is to move.
    if (end - p < 2 || p[0] < 'a' || p[0] > 'h' || p[1] != (out.black ? '3' : '6')) return false;
    out.ep = at(p[0] - 'a', p[1] - '1');
    p += 2;
  }
  if (p < end) {
    uint32_t half = 0;
    uint32_t full = 0;
    if (*p++ != ' ' || !readNumber(p, end, half) || p >= end || *p++ != ' ' || !readNumber(p, end, full) || p != end ||
        half > 255 || full == 0 || full > 65535) {
      return false;
    }
    out.halfmove = static_cast<uint8_t>(half);
    out.fullmove = static_cast<uint16_t>(full);
  }
  // Exactly one king a side; no pawn on the first or last rank.
  int kings[2] = {};
  for (uint8_t s = 0; s < SQUARES; ++s) {
    const uint8_t piece = out.board[s];
    if (typeOf(piece) == KING) ++kings[isBlack(piece)];
    if (typeOf(piece) == PAWN && (rankOf(s) == 0 || rankOf(s) == 7)) return false;
  }
  if (kings[0] != 1 || kings[1] != 1) return false;
  // Rights only where king and rook still stand.
  const auto keep = [&](const uint8_t right, const uint8_t king, const uint8_t rook, const uint8_t color) {
    if (out.board[king] != (KING | color) || out.board[rook] != (ROOK | color)) out.castling &= ~right;
  };
  keep(WHITE_KINGSIDE, 4, 7, 0);
  keep(WHITE_QUEENSIDE, 4, 0, 0);
  keep(BLACK_KINGSIDE, 60, 63, BLACK);
  keep(BLACK_QUEENSIDE, 60, 56, BLACK);
  return true;
}

size_t toFen(const Position& pos, char* out, const size_t size) {
  char fen[FEN_MAX + 8];
  size_t n = 0;
  for (int rank = 7; rank >= 0; --rank) {
    int empty = 0;
    for (int file = 0; file < 8; ++file) {
      const uint8_t piece = pos.board[rank * 8 + file];
      if (piece == EMPTY) {
        ++empty;
        continue;
      }
      if (empty) fen[n++] = static_cast<char>('0' + empty);
      empty = 0;
      const char letter = PIECE_LETTERS[typeOf(piece)];
      fen[n++] = isBlack(piece) ? letter : static_cast<char>(letter - 0x20);
    }
    if (empty) fen[n++] = static_cast<char>('0' + empty);
    if (rank) fen[n++] = '/';
  }
  fen[n++] = ' ';
  fen[n++] = pos.black ? 'b' : 'w';
  fen[n++] = ' ';
  if (!pos.castling) fen[n++] = '-';
  if (pos.castling & WHITE_KINGSIDE) fen[n++] = 'K';
  if (pos.castling & WHITE_QUEENSIDE) fen[n++] = 'Q';
  if (pos.castling & BLACK_KINGSIDE) fen[n++] = 'k';
  if (pos.castling & BLACK_QUEENSIDE) fen[n++] = 'q';
  fen[n++] = ' ';
  if (pos.ep == NO_SQUARE) {
    fen[n++] = '-';
  } else {
    fen[n++] = static_cast<char>('a' + fileOf(pos.ep));
    fen[n++] = static_cast<char>('1' + rankOf(pos.ep));
  }
  fen[n] = '\0';
  const int written = snprintf(out, size, "%s %u %u", fen, pos.halfmove, pos.fullmove);
  return written < 0 ? 0 : static_cast<size_t>(written) < size ? static_cast<size_t>(written) : size - 1;
}

bool parseUci(const char* text, const size_t length, Move& out) {
  out = Move{};
  if (length != 4 && length != 5) return false;
  for (int k = 0; k < 4; k += 2) {
    if (text[k] < 'a' || text[k] > 'h' || text[k + 1] < '1' || text[k + 1] > '8') return false;
  }
  out.from = at(text[0] - 'a', text[1] - '1');
  out.to = at(text[2] - 'a', text[3] - '1');
  if (length == 5) {
    out.promo = typeFromLetter(text[4]);
    if (out.promo < KNIGHT || out.promo > QUEEN || text[4] < 'a') return false;
  }
  return out.from != out.to;
}

void toUci(const Move& move, char* out) {
  out[0] = static_cast<char>('a' + fileOf(move.from));
  out[1] = static_cast<char>('1' + rankOf(move.from));
  out[2] = static_cast<char>('a' + fileOf(move.to));
  out[3] = static_cast<char>('1' + rankOf(move.to));
  size_t n = 4;
  if (move.promo) out[n++] = PIECE_LETTERS[move.promo];
  out[n] = '\0';
}

bool attacked(const Position& pos, const uint8_t square, const bool byBlack) {
  const uint8_t color = colorOf(byBlack);
  const int f = fileOf(square);
  const int r = rankOf(square);
  // A pawn attacks forward diagonally, so it stands one rank behind the square.
  const int pawnRank = byBlack ? r + 1 : r - 1;
  for (const int df : {-1, 1}) {
    const uint8_t s = at(f + df, pawnRank);
    if (s != NO_SQUARE && pos.board[s] == (PAWN | color)) return true;
  }
  for (const auto& d : KNIGHT_STEPS) {
    const uint8_t s = at(f + d[0], r + d[1]);
    if (s != NO_SQUARE && pos.board[s] == (KNIGHT | color)) return true;
  }
  for (const auto& d : KING_STEPS) {
    const uint8_t s = at(f + d[0], r + d[1]);
    if (s != NO_SQUARE && pos.board[s] == (KING | color)) return true;
  }
  return slides(pos, square, ROOK_DIRS, ROOK | color, QUEEN | color) ||
         slides(pos, square, BISHOP_DIRS, BISHOP | color, QUEEN | color);
}

bool inCheck(const Position& pos) {
  const uint8_t king = kingSquare(pos, pos.black != 0);
  return king != NO_SQUARE && attacked(pos, king, pos.black == 0);
}

uint64_t targets(const Position& pos, const uint8_t from) {
  if (from >= SQUARES) return 0;
  uint64_t out = 0;
  for (uint64_t pseudo = pseudoTargets(pos, from); pseudo; pseudo &= pseudo - 1) {
    const auto to = static_cast<uint8_t>(__builtin_ctzll(pseudo));
    if (keepsKingSafe(pos, from, to)) out |= 1ULL << to;
  }
  return out;
}

bool promotes(const Position& pos, const uint8_t from, const uint8_t to) {
  if (from >= SQUARES || to >= SQUARES) return false;
  const uint8_t piece = pos.board[from];
  return typeOf(piece) == PAWN && own(pos, piece) && rankOf(to) == (isBlack(piece) ? 0 : 7);
}

bool isLegal(const Position& pos, const Move& move) {
  if (move.from >= SQUARES || move.to >= SQUARES) return false;
  if (promotes(pos, move.from, move.to) != (move.promo != EMPTY)) return false;
  return (targets(pos, move.from) >> move.to) & 1;
}

void apply(Position& pos, Move move) {
  const uint8_t piece = pos.board[move.from];
  const uint8_t type = typeOf(piece);
  const bool black = isBlack(piece);
  const bool capture = pos.board[move.to] != EMPTY;
  const int df = fileOf(move.to) - fileOf(move.from);
  const int dr = rankOf(move.to) - rankOf(move.from);

  if (type == PAWN && move.to == pos.ep && !capture && df != 0) {
    pos.board[at(fileOf(move.to), rankOf(move.from))] = EMPTY;  // en passant
  }
  if (type == KING && (df == 2 || df == -2)) {
    const uint8_t rookFrom = df > 0 ? move.from + 3 : move.from - 4;
    const uint8_t rookTo = df > 0 ? move.from + 1 : move.from - 1;
    pos.board[rookTo] = pos.board[rookFrom];
    pos.board[rookFrom] = EMPTY;
  }
  uint8_t placed = piece;
  if (type == PAWN && rankOf(move.to) == (black ? 0 : 7)) {
    placed = static_cast<uint8_t>((move.promo ? move.promo : QUEEN) | colorOf(black));
  }
  pos.board[move.to] = placed;
  pos.board[move.from] = EMPTY;

  // A king move gives up both rights; a rook leaving or taken in its corner, that side's.
  if (type == KING) pos.castling &= black ? ~(BLACK_KINGSIDE | BLACK_QUEENSIDE) : ~(WHITE_KINGSIDE | WHITE_QUEENSIDE);
  for (const uint8_t s : {move.from, move.to}) {
    if (s == 0) pos.castling &= ~WHITE_QUEENSIDE;
    if (s == 7) pos.castling &= ~WHITE_KINGSIDE;
    if (s == 56) pos.castling &= ~BLACK_QUEENSIDE;
    if (s == 63) pos.castling &= ~BLACK_KINGSIDE;
  }

  pos.ep = type == PAWN && (dr == 2 || dr == -2) ? static_cast<uint8_t>((move.from + move.to) / 2) : NO_SQUARE;
  pos.halfmove = type == PAWN || capture ? 0 : static_cast<uint8_t>(pos.halfmove < 255 ? pos.halfmove + 1 : 255);
  if (black) ++pos.fullmove;
  pos.black = !black;
}

bool hasLegalMove(const Position& pos) {
  for (uint8_t s = 0; s < SQUARES; ++s) {
    if (own(pos, pos.board[s]) && targets(pos, s)) return true;
  }
  return false;
}

bool checkmate(const Position& pos) { return inCheck(pos) && !hasLegalMove(pos); }

bool parsePuzzle(const char* line, const size_t length, Puzzle& out) {
  out = Puzzle{};
  const char* end = line + length;
  const char* fields[4];
  size_t sizes[4];
  const char* p = line;
  for (int k = 0; k < 4; ++k) {
    const char* comma = k < 3 ? static_cast<const char*>(std::memchr(p, ',', end - p)) : end;
    if (!comma) return false;
    fields[k] = p;
    sizes[k] = static_cast<size_t>(comma - p);
    p = comma + 1;
  }
  if (sizes[0] == 0 || sizes[0] > ID_MAX) return false;
  std::memcpy(out.id, fields[0], sizes[0]);
  const char* r = fields[1];
  uint32_t rating = 0;
  if (!readNumber(r, fields[1] + sizes[1], rating) || r != fields[1] + sizes[1] || rating > 4000) return false;
  out.rating = static_cast<uint16_t>(rating);
  if (!parseFen(fields[2], sizes[2], out.start)) return false;

  Position pos = out.start;
  const char* m = fields[3];
  const char* movesEnd = fields[3] + sizes[3];
  while (m < movesEnd) {
    const char* space = static_cast<const char*>(std::memchr(m, ' ', movesEnd - m));
    const char* stop = space ? space : movesEnd;
    Move move;
    if (out.count == MOVES_MAX || !parseUci(m, static_cast<size_t>(stop - m), move) || !isLegal(pos, move)) {
      return false;
    }
    apply(pos, move);
    out.moves[out.count++] = move;
    m = space ? space + 1 : movesEnd;
  }
  return out.count >= 2 && out.count % 2 == 0;
}

Verdict judge(const Puzzle& puzzle, const uint8_t ply, const Position& pos, Move& move) {
  if (ply >= puzzle.count) return Verdict::Wrong;
  const Move& listed = puzzle.moves[ply];
  move.promo = EMPTY;
  if (promotes(pos, move.from, move.to)) {
    move.promo = listed.from == move.from && listed.to == move.to ? listed.promo : static_cast<uint8_t>(QUEEN);
  }
  if (!isLegal(pos, move)) return Verdict::Wrong;
  if (move == listed) return Verdict::Right;
  Position next = pos;
  apply(next, move);
  return checkmate(next) ? Verdict::Mate : Verdict::Wrong;
}

}  // namespace x4::play::chess
