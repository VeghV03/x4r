// Chess tactics from the Lichess puzzle database (/x4/chess/puzzles.txt). The
// device plays the opponent's first move; the player finds the answer. A right
// move gets the device's reply, until the line ends. One wrong move loses and
// shows the right one as an arrow. Arrows move over the board, Confirm picks a
// piece and then its square; Back drops the piece.
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "Chess.h"
#include "ChessPieces.h"
#include "Words.h"
#include "fontIds.h"
#include "play/Game.h"
#include "play/Play.h"
#include "play/PlayPaths.h"
#include "play/ui/PlayUi.h"

namespace x4::play {

bool chessReady(Lang) { return Storage.exists(paths::CHESS_PUZZLES); }

namespace {

using chess::Move;
using chess::NO_SQUARE;

constexpr uint8_t STATE_VERSION = 1;
// The file's rating bands, easiest first: level 1 plays from the first.
constexpr uint8_t BANDS = LEVEL_MAX;
// The device's reply waits this long, so the player's own move shows first.
constexpr uint32_t REPLY_MS = 700;
constexpr int STATUS_FONT = UI_12_FONT_ID;
constexpr int GAP = 12;

enum End : uint8_t { PLAYING = 0, SOLVED = 1, MISSED = 2 };

// The saved game. The puzzle is kept as its line and parsed again on restore.
struct State {
  uint8_t version;
  uint8_t ply;       // moves of the line on the board
  uint8_t cursor;    // square
  uint8_t selected;  // square of the picked piece, or NO_SQUARE
  uint8_t end;
  uint8_t failed;  // no puzzle could be read
  uint8_t replyDue;  // the device's reply comes on the next tick
  uint8_t reserved;
  Move last;   // marked on the board
  Move shown;  // the right move, after a miss
  chess::Position pos;
  char line[chess::PUZZLE_LINE_MAX];
};
static_assert(sizeof(State) <= SAVE_MAX, "State fits a saved game");

// Fills the area with a gray of `level` quarters of black, in a pattern fixed to
// the screen so neighbouring areas line up.
void shade(const GfxRenderer& r, const int x, const int y, const int w, const int h, const int level) {
  for (int row = y; row < y + h; ++row) {
    for (int col = x; col < x + w; ++col) {
      bool black;
      switch (level) {
        case 1:
          black = !(col & 1) && !(row & 1);
          break;
        case 2:
          black = ((col ^ row) & 1) == 0;
          break;
        default:
          black = (col & 1) || (row & 1);
          break;
      }
      r.drawPixel(col, row, black);
    }
  }
}

// A frame of `pen` black pixels inwards from the square's edge, edged white on both sides.
// `clear` draws all of it with drawPixel(true), which in a gray plane takes it out of the gray.
void frame(const GfxRenderer& r, const int x, const int y, const int size, const int pen, const bool clear = false) {
  for (int i = 0; i <= pen + 1; ++i) {
    r.drawRect(x + i, y + i, size - 2 * i, size - 2 * i, clear || (i > 0 && i <= pen));
  }
}

bool pixelOf(const uint8_t* bits, const int x, const int y) {
  return (bits[y * chess::pieces::STRIDE + x / 8] >> (7 - x % 8)) & 1;
}

// The piece's halo, drawn with drawPixel(true): in a gray plane it keeps the piece out of the gray.
void clearPiece(const GfxRenderer& r, const int x, const int y, const uint8_t piece) {
  const auto& b = chess::pieces::PIECES[chess::typeOf(piece) - 1];
  for (int py = 0; py < chess::pieces::SIZE; ++py) {
    for (int px = 0; px < chess::pieces::SIZE; ++px) {
      if (pixelOf(b.halo, px, py)) r.drawPixel(x + px, y + py, true);
    }
  }
}

void drawPiece(const GfxRenderer& r, const int x, const int y, const uint8_t piece) {
  const auto& b = chess::pieces::PIECES[chess::typeOf(piece) - 1];
  const bool black = chess::isBlack(piece);
  for (int py = 0; py < chess::pieces::SIZE; ++py) {
    for (int px = 0; px < chess::pieces::SIZE; ++px) {
      if (!pixelOf(b.halo, px, py)) continue;
      bool ink;
      if (black) {
        ink = pixelOf(b.fill, px, py) && !pixelOf(b.detail, px, py);
      } else {
        ink = pixelOf(b.line, px, py);
      }
      r.drawPixel(x + px, y + py, ink);
    }
  }
}

void triangle(const GfxRenderer& r, const float tipX, const float tipY, const float dx, const float dy,
              const float length, const float half, const bool state) {
  const float baseX = tipX - dx * length;
  const float baseY = tipY - dy * length;
  const int xs[3] = {static_cast<int>(tipX), static_cast<int>(baseX - dy * half), static_cast<int>(baseX + dy * half)};
  const int ys[3] = {static_cast<int>(tipY), static_cast<int>(baseY + dx * half), static_cast<int>(baseY - dx * half)};
  r.fillPolygon(xs, ys, 3, state);
}

// A black arrow edged white, from the centre of one square to the other's.
// `clear` draws its white edge with drawPixel(true) too, which in a gray plane takes the arrow out of the gray.
void arrow(const GfxRenderer& r, const int x1, const int y1, const int x2, const int y2, const int sq,
           const bool clear = false) {
  const float len = std::sqrt(static_cast<float>((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)));
  if (len < 1) return;
  const float dx = (x2 - x1) / len;
  const float dy = (y2 - y1) / len;
  const float head = sq * 0.42f;
  const int shaftX = static_cast<int>(x2 - dx * head * 0.8f);
  const int shaftY = static_cast<int>(y2 - dy * head * 0.8f);
  r.drawLine(x1, y1, shaftX, shaftY, 12, clear);
  triangle(r, x2 + dx * 4, y2 + dy * 4, dx, dy, head + 8, sq * 0.3f + 5, clear);
  r.drawLine(x1, y1, shaftX, shaftY, 6, true);
  triangle(r, x2, y2, dx, dy, head, sq * 0.3f, true);
}

class ChessGame final : public Game {
 public:
  void start(const uint32_t seed, const uint8_t level) override {
    s = State{};
    s.version = STATE_VERSION;
    s.selected = NO_SQUARE;
    const uint8_t band = std::clamp<uint8_t>(level, LEVEL_MIN, LEVEL_MAX) - 1;
    if (!words::pickBand(paths::CHESS_PUZZLES, seed, band, BANDS, s.line, sizeof(s.line), &isPuzzle) || !parse()) {
      LOG_ERR("PLAY", "No chess puzzle in %s", paths::CHESS_PUZZLES);
      s.failed = 1;
      return;
    }
    LOG_INF("PLAY", "Chess %s, rating %u", puzzle.id, puzzle.rating);
    s.pos = puzzle.start;
    s.ply = 0;
    s.cursor = puzzle.moves[0].to;
    reply();
  }

  size_t save(uint8_t* out) const override {
    std::memcpy(out, &s, sizeof(s));
    return sizeof(s);
  }

  bool restore(const uint8_t* in, const size_t size) override {
    if (size != sizeof(State)) return false;
    std::memcpy(&s, in, sizeof(s));
    if (s.version != STATE_VERSION || s.cursor >= chess::SQUARES ||
        (s.selected != NO_SQUARE && s.selected >= chess::SQUARES) || s.end > MISSED ||
        !std::memchr(s.line, '\0', sizeof(s.line)) || !parse() || s.ply > puzzle.count) {
      return false;
    }
    // The position must read back as a legal one.
    char fen[chess::FEN_MAX];
    chess::Position check;
    chess::toFen(s.pos, fen, sizeof(fen));
    if (!chess::parseFen(fen, std::strlen(fen), check)) return false;
    if (s.replyDue) reply();
    return true;
  }

  bool input(const Keys& keys, const uint32_t now) override {
    if (s.end) return false;
    if (keys.up || keys.down || keys.left || keys.right) {
      // Screen column and row, row 0 at the top.
      int col = screenCol(s.cursor);
      int row = screenRow(s.cursor);
      if (keys.up) row = (row + 7) % 8;
      if (keys.down) row = (row + 1) % 8;
      if (keys.left) col = (col + 7) % 8;
      if (keys.right) col = (col + 1) % 8;
      s.cursor = squareAt(col, row);
      return true;
    }
    if (!keys.confirm || s.replyDue) return false;
    const uint8_t at = s.cursor;
    if (s.selected != NO_SQUARE && at != s.selected && ((chess::targets(s.pos, s.selected) >> at) & 1)) {
      return play(at, now);
    }
    if (at == s.selected) {
      s.selected = NO_SQUARE;
      return true;
    }
    if (!chess::targets(s.pos, at)) return false;
    s.selected = at;
    return true;
  }

  bool back() override {
    if (s.end || s.selected == NO_SQUARE) return false;
    s.selected = NO_SQUARE;
    return true;
  }

  bool tick(const uint32_t now) override {
    if (!s.replyDue || static_cast<int32_t>(now - replyAt) < 0) return false;
    reply();
    return true;
  }

  bool paced() const override { return s.replyDue != 0; }
  // The reply is saved as due and played on restore.
  bool resumable() const override { return true; }

  void draw(const GfxRenderer& r, const x4::ui::Frame& f) const override {
    const Board b = board(r, f);
    // With a gray pass, dark squares are a true gray drawn there; without one, dithered here.
    const bool grays = x4::ui::grayPassRuns(r);
    const uint64_t marked = s.selected != NO_SQUARE ? chess::targets(s.pos, s.selected) : 0;

    if (!s.end) {
      r.drawCenteredText(STATUS_FONT, b.y0 - GAP - b.lineHeight,
                         s.pos.black ? tr(STR_BLACK_TO_MOVE) : tr(STR_WHITE_TO_MOVE));
    }
    r.drawRect(b.x0 - 2, b.y0 - 2, b.size + 4, b.size + 4, 2, true);
    for (uint8_t square = 0; square < chess::SQUARES; ++square) {
      const int x = b.x0 + screenCol(square) * b.sq;
      const int y = b.y0 + screenRow(square) * b.sq;
      // Light squares white, dark ones half gray; the last move's squares a quarter darker.
      const int level = shadeOf(square);
      if (grays) {
        if (level == 3) r.fillRect(x, y, b.sq, b.sq, true);
      } else if (level) {
        shade(r, x, y, b.sq, b.sq, level);
      }
      const uint8_t piece = s.pos.board[square];
      if (piece) drawPiece(r, x + b.inset, y + b.inset, piece);
      if ((marked >> square) & 1) {
        if (piece) {
          corners(r, x, y, b.sq);
        } else {
          dot(r, x + b.sq / 2, y + b.sq / 2, b.sq / 8);
        }
      }
    }
    if (s.end == MISSED && hasMove(s.shown)) {
      const int half = b.sq / 2;
      arrow(r, b.x0 + screenCol(s.shown.from) * b.sq + half, b.y0 + screenRow(s.shown.from) * b.sq + half,
            b.x0 + screenCol(s.shown.to) * b.sq + half, b.y0 + screenRow(s.shown.to) * b.sq + half, b.sq);
    }
    if (s.selected != NO_SQUARE) {
      frame(r, b.x0 + screenCol(s.selected) * b.sq, b.y0 + screenRow(s.selected) * b.sq, b.sq, ui::REST_PEN);
    }
    if (!s.end) frame(r, b.x0 + screenCol(s.cursor) * b.sq, b.y0 + screenRow(s.cursor) * b.sq, b.sq, ui::FOCUS_PEN);
  }

  bool drawsGray() const override { return true; }

  // Dark squares dark gray, a light square of the last move light gray. Whatever draw() put on a
  // square (pieces with their halo, marks, frames, the arrow) is cleared back out of the gray.
  void drawGray(const GfxRenderer& r, const x4::ui::Frame& f) const override {
    const Board b = board(r, f);
    const bool msb = r.getRenderMode() == GfxRenderer::GRAYSCALE_MSB;
    const uint64_t marked = s.selected != NO_SQUARE ? chess::targets(s.pos, s.selected) : 0;
    for (uint8_t square = 0; square < chess::SQUARES; ++square) {
      const int x = b.x0 + screenCol(square) * b.sq;
      const int y = b.y0 + screenRow(square) * b.sq;
      const int level = shadeOf(square);
      // Gray planes: the light level sets the MSB plane only, the dark level both.
      if (level == 2 || (level == 1 && msb)) r.fillRect(x, y, b.sq, b.sq, false);
      const uint8_t piece = s.pos.board[square];
      if (piece) clearPiece(r, x + b.inset, y + b.inset, piece);
      if ((marked >> square) & 1) {
        if (piece) {
          corners(r, x, y, b.sq);
        } else {
          dot(r, x + b.sq / 2, y + b.sq / 2, b.sq / 8, true);
        }
      }
    }
    if (s.end == MISSED && hasMove(s.shown)) {
      const int half = b.sq / 2;
      arrow(r, b.x0 + screenCol(s.shown.from) * b.sq + half, b.y0 + screenRow(s.shown.from) * b.sq + half,
            b.x0 + screenCol(s.shown.to) * b.sq + half, b.y0 + screenRow(s.shown.to) * b.sq + half, b.sq, true);
    }
    if (s.selected != NO_SQUARE) {
      frame(r, b.x0 + screenCol(s.selected) * b.sq, b.y0 + screenRow(s.selected) * b.sq, b.sq, ui::REST_PEN, true);
    }
    if (!s.end) {
      frame(r, b.x0 + screenCol(s.cursor) * b.sq, b.y0 + screenRow(s.cursor) * b.sq, b.sq, ui::FOCUS_PEN, true);
    }
  }

  bool playable() const override { return !s.failed; }

  Outcome outcome() const override {
    if (!s.end) return Outcome::Playing;
    return s.end == SOLVED ? Outcome::Won : Outcome::Lost;
  }

  // The puzzle's Lichess rating.
  int16_t score() const override { return puzzle.count ? static_cast<int16_t>(puzzle.rating) : -1; }

 private:
  State s{};
  chess::Puzzle puzzle;
  uint32_t replyAt = 0;

  static bool isPuzzle(const char* line, void*) {
    chess::Puzzle p;
    return chess::parsePuzzle(line, std::strlen(line), p);
  }

  static bool hasMove(const Move& m) { return m.from != m.to; }

  bool parse() { return chess::parsePuzzle(s.line, std::strlen(s.line), puzzle); }

  // The player has the side not moving first, at the bottom of the board.
  bool flipped() const { return puzzle.start.black == 0; }
  int screenCol(const uint8_t square) const { return flipped() ? 7 - chess::fileOf(square) : chess::fileOf(square); }
  int screenRow(const uint8_t square) const { return flipped() ? chess::rankOf(square) : 7 - chess::rankOf(square); }
  uint8_t squareAt(const int col, const int row) const {
    return static_cast<uint8_t>(flipped() ? row * 8 + (7 - col) : (7 - row) * 8 + col);
  }

  // The next move of the line, the device's.
  void reply() {
    s.replyDue = 0;
    if (s.ply >= puzzle.count) return;
    s.last = puzzle.moves[s.ply++];
    chess::apply(s.pos, s.last);
  }

  bool play(const uint8_t to, const uint32_t now) {
    Move move{s.selected, to, chess::EMPTY};
    s.selected = NO_SQUARE;
    const chess::Verdict verdict = chess::judge(puzzle, s.ply, s.pos, move);
    if (verdict == chess::Verdict::Wrong) {
      s.shown = puzzle.moves[s.ply];
      s.end = MISSED;
      return true;
    }
    chess::apply(s.pos, move);
    s.last = move;
    ++s.ply;
    if (verdict == chess::Verdict::Mate || s.ply >= puzzle.count) {
      s.end = SOLVED;
      return true;
    }
    s.replyDue = 1;
    replyAt = now + REPLY_MS;
    return true;
  }

  struct Board {
    int lineHeight;
    int sq;
    int size;
    int x0;
    int y0;
    int inset;
  };

  // The board in the margins the other boards keep.
  static Board board(const GfxRenderer& r, const x4::ui::Frame& f) {
    Board b{};
    const int width = r.getScreenWidth();
    b.lineHeight = r.getLineHeight(STATUS_FONT);
    const int room = f.bottom - f.top + 40 - b.lineHeight - GAP;
    b.sq = std::min((width - 48) / 8, room / 8);
    b.size = b.sq * 8;
    b.x0 = (width - b.size) / 2;
    b.y0 = f.top - 20 + b.lineHeight + GAP + (room - b.size) / 2;
    b.inset = (b.sq - chess::pieces::SIZE) / 2;
    return b;
  }

  // Quarters of black: 2 for a dark square, one more on the last move's squares.
  int shadeOf(const uint8_t square) const {
    const bool dark = (chess::fileOf(square) + chess::rankOf(square)) % 2 == 0;
    const bool last = hasMove(s.last) && (square == s.last.from || square == s.last.to);
    return (dark ? 2 : 0) + (last ? 1 : 0);
  }

  // A legal capture: the four corners of the square.
  static void corners(const GfxRenderer& r, const int x, const int y, const int sq) {
    const int c = sq / 4;
    const int x1 = x + sq - 1;
    const int y1 = y + sq - 1;
    const int xs[4][3] = {{x, x + c, x}, {x1, x1 - c, x1}, {x, x + c, x}, {x1, x1 - c, x1}};
    const int ys[4][3] = {{y, y, y + c}, {y, y, y + c}, {y1, y1, y1 - c}, {y1, y1, y1 - c}};
    for (int k = 0; k < 4; ++k) r.fillPolygon(xs[k], ys[k], 3, true);
  }

  // A legal move to an empty square: a black dot ringed white. `clear` draws its whole disc
  // with drawPixel(true), which in a gray plane takes it out of the gray.
  static void dot(const GfxRenderer& r, const int cx, const int cy, const int radius, const bool clear = false) {
    const int outer = radius + 2;
    for (int dy = -outer; dy <= outer; ++dy) {
      for (int dx = -outer; dx <= outer; ++dx) {
        const int d = dx * dx + dy * dy;
        if (d <= outer * outer) r.drawPixel(cx + dx, cy + dy, clear || d <= radius * radius);
      }
    }
  }
};

}  // namespace

std::unique_ptr<Game> makeChess() { return std::unique_ptr<Game>(new (std::nothrow) ChessGame()); }

}  // namespace x4::play
