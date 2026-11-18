#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "Words.h"
#include "play/Game.h"

// What the word games share on screen: letter tiles and the on-screen keyboard.
namespace x4::play::words {

// How a tile looks: empty and typed tiles are outlined thin, Present thick,
// Correct inverted; Absent letters stand without a box.
enum class Tile : uint8_t { Empty, Typed, Absent, Present, Correct, Reveal };

struct Box {
  int x;
  int y;
  int width;
  int height;
};

// One tile with `letter` (0 for none) drawn upper-case.
void drawTile(const GfxRenderer& r, const Box& box, Letter letter, Tile tile);
// A line through a row of tiles: the word is not in the list.
void strikeThrough(const GfxRenderer& r, const Box& first, int count, int pitch);

enum class EnterIcon : uint8_t { Check, Next };

// The alphabet in rows of COLUMNS, with Delete and Enter at the end of the
// last row. Arrows move the cursor; Left and Right run through the keys in order.
class Keyboard {
 public:
  static constexpr uint8_t COLUMNS = 10;
  static constexpr int DELETE = -1;
  static constexpr int ENTER = -2;

  void setLang(Lang lang);
  // Moves the cursor. False when Up leaves the top row and `wrapUp` is off.
  bool move(const Keys& keys, bool wrapUp);
  // The alphabet index under the cursor, or DELETE or ENTER.
  int key() const;
  // The letter under the cursor; 0 on Delete and Enter.
  Letter letter() const;
  uint8_t cursor() const { return at; }
  void setCursor(uint8_t key) { at = key < total() ? key : 0; }
  uint8_t rows() const;

  // `marks` holds one Mark per alphabet letter (null: none). The key under
  // the cursor is framed `pen` thick (0: not at all).
  void draw(const GfxRenderer& r, const Box& area, const Mark* marks, int pen, EnterIcon enter) const;
  // Height for `width` pixels.
  int height(int width) const;
  // Index of `letter` in the alphabet, or -1.
  int indexOf(Letter letter) const;

 private:
  const Letter* letters = nullptr;
  uint8_t count = 0;
  uint8_t at = 0;

  uint8_t total() const { return static_cast<uint8_t>(count + 2); }  // letters, Delete, Enter
  void place(uint8_t key, int& row, int& col) const;
};

}  // namespace x4::play::words
