// Arithmetic chain: a start number, then one operation per screen, then the
// result typed on three digit wheels. Self-paced: Confirm shows the next step.
// Left and Right pick a wheel, Up and Down turn it, Confirm checks. The typed
// number then stays, checked or crossed with the result under it, until Confirm.
#include <cstdio>
#include <cstring>
#include <new>

#include "MemoryGames.h"
#include "play/Game.h"
#include "play/Play.h"
#include "play/ui/PlayUi.h"

namespace x4::play {

namespace {

constexpr uint8_t STATE_VERSION = 3;
constexpr uint8_t DIGITS = 3;
constexpr int MARK = 40;     // check and cross beside the numbers
constexpr int CHEVRON = 14;  // half the width of the arrows above and below the wheel

struct State {
  uint8_t version;
  uint8_t step;            // 0 = the start number, 1..count = the operations, count + 1 = typing
  uint8_t digits[DIGITS];  // hundreds, tens, units
  uint8_t wheel;           // the digit Up and Down turn
  int16_t typed;           // -1 until checked
  uint8_t unused;          // was "reviewed"; keeps older saves readable
  uint8_t gaveUp;
  memory::ArithChain chain;
};

class ArithGame final : public Game {
 public:
  void start(const uint32_t seed, const uint8_t level) override {
    s = State{};
    s.version = STATE_VERSION;
    s.typed = -1;
    memory::arithMake(seed, level, s.chain);
  }

  size_t save(uint8_t* out) const override {
    std::memcpy(out, &s, sizeof(s));
    return sizeof(s);
  }

  bool restore(const uint8_t* in, const size_t size) override {
    if (size != sizeof(State)) return false;
    std::memcpy(&s, in, sizeof(s));
    if (s.version != STATE_VERSION || s.chain.count == 0 || s.chain.count > memory::CHAIN_MAX ||
        s.typed < -1 || s.typed > memory::VALUE_MAX || s.wheel >= DIGITS) {
      return false;
    }
    for (const uint8_t digit : s.digits) {
      if (digit > 9) return false;
    }
    // The chain lives in the head: a round left midway starts over.
    if (s.typed < 0) {
      s.step = 0;
      std::memset(s.digits, 0, sizeof(s.digits));
      s.wheel = 0;
    }
    return true;
  }

  bool input(const Keys& keys, uint32_t) override {
    if (s.typed >= 0) return false;
    if (!typing()) {
      if (!keys.confirm) return false;
      ++s.step;
      return true;
    }
    if (keys.left || keys.right) {
      s.wheel = static_cast<uint8_t>((s.wheel + (keys.left ? DIGITS - 1 : 1)) % DIGITS);
      return true;
    }
    if (keys.up || keys.down) {
      uint8_t& digit = s.digits[s.wheel];
      digit = static_cast<uint8_t>((digit + (keys.up ? 1 : 9)) % 10);
      return true;
    }
    if (!keys.confirm) return false;
    s.typed = entered();
    return true;
  }

  // restore() starts an unchecked chain over: only its start and a checked number are saved.
  bool resumable() const override { return s.typed >= 0 || s.step == 0; }

  void draw(const GfxRenderer& r, const x4::ui::Frame& f) const override {
    const int centreX = r.getScreenWidth() / 2;
    const int centreY = f.top + (f.bottom - f.top) * 2 / 5;
    char text[12];
    if (!typing()) {
      const int scale = ui::scaleForHeight(r, 96);
      if (s.step == 0) {
        snprintf(text, sizeof(text), "%d", s.chain.start);
      } else {
        const memory::ArithStep& op = s.chain.steps[s.step - 1];
        snprintf(text, sizeof(text), "%c %u", op.op, static_cast<unsigned>(op.value));
      }
      ui::drawBigText(r, text, centreX, centreY + ui::glyphHeight(r, scale) / 2, scale);
      x4::ui::drawProgressDots(r, s.step, s.chain.count + 1, f.dotsY);
      return;
    }
    if (s.typed < 0) {
      drawWheels(r, centreX, centreY);
      return;
    }
    // The typed number, checked or crossed; when wrong, the result checked under it.
    const bool right = s.typed == s.chain.result;
    const int scale = ui::scaleForHeight(r, 72);
    const int height = ui::glyphHeight(r, scale);
    const int rowHeight = height + 64;
    const int rows = right ? 1 : 2;
    const int top = centreY - rowHeight * rows / 2;
    const int markX = centreX - f.width / 3;
    for (int row = 0; row < rows; ++row) {
      const int y = top + row * rowHeight;
      snprintf(text, sizeof(text), "%d", row == 0 ? s.typed : s.chain.result);
      ui::drawBigText(r, text, centreX, y + (rowHeight + height) / 2, scale);
      const int markY = y + (rowHeight - MARK) / 2;
      if (row == 0 && !right) {
        x4::ui::drawCross(r, markX, markY, MARK);
      } else {
        x4::ui::drawCheck(r, markX, markY, MARK);
      }
    }
  }

  bool marksOutcome() const override { return true; }

  Outcome outcome() const override {
    if (s.gaveUp) return Outcome::Lost;
    if (s.typed < 0) return Outcome::Playing;
    return s.typed == s.chain.result ? Outcome::Won : Outcome::Lost;
  }

 private:
  State s{};

  bool typing() const { return s.step > s.chain.count; }

  int16_t entered() const { return static_cast<int16_t>(s.digits[0] * 100 + s.digits[1] * 10 + s.digits[2]); }

  // Three framed digits side by side, the turning one framed thick between
  // an arrow up and an arrow down.
  void drawWheels(const GfxRenderer& r, const int centreX, const int centreY) const {
    const int scale = ui::scaleForHeight(r, 96);
    const int height = ui::glyphHeight(r, scale);
    const int cellWidth = ui::bigTextWidth(r, "0", scale) + 40;
    const int cellHeight = height + 56;
    const int gap = 16;
    const int left = centreX - (DIGITS * cellWidth + (DIGITS - 1) * gap) / 2;
    const int top = centreY - cellHeight / 2;
    char digit[2] = {};
    for (uint8_t k = 0; k < DIGITS; ++k) {
      const int x = left + k * (cellWidth + gap);
      digit[0] = static_cast<char>('0' + s.digits[k]);
      ui::drawBigText(r, digit, x + cellWidth / 2, top + (cellHeight + height) / 2, scale);
      ui::drawCursor(r, x, top, cellWidth, cellHeight, k == s.wheel ? ui::FOCUS_PEN : ui::REST_PEN);
    }
    const int cx = left + s.wheel * (cellWidth + gap) + cellWidth / 2;
    const int above = top - gap;
    const int below = top + cellHeight + gap;
    const int upX[] = {cx - CHEVRON, cx + CHEVRON, cx};
    const int upY[] = {above, above, above - CHEVRON};
    const int downY[] = {below, below, below + CHEVRON};
    r.fillPolygon(upX, upY, 3);
    r.fillPolygon(upX, downY, 3);
  }
};

}  // namespace

std::unique_ptr<Game> makeArith() { return std::unique_ptr<Game>(new (std::nothrow) ArithGame()); }

}  // namespace x4::play
