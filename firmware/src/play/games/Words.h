#pragma once

#include <cstddef>
#include <cstdint>

#include "play/Play.h"

// Rules and word files of the word games (docs/DATA.md, "Word lists" and
// "Crosswords"). Pure C++ over HalStorage, so the host tests cover them.
namespace x4::play::words {

// One letter is one Unicode code point: Hungarian ő is a single letter.
using Letter = uint16_t;

inline constexpr uint8_t WORD_LEN = 5;
inline constexpr uint8_t GUESSES = 6;
inline constexpr uint8_t LETTERS_MAX = 8;  // longest word a list holds
inline constexpr uint8_t ALPHABET_MAX = 35;
inline constexpr size_t PATH_SIZE = 40;
inline constexpr size_t LINE_SIZE = 128;  // longest line of a word list

// The letters of `lang` in keyboard order.
uint8_t alphabet(Lang lang, const Letter** out);
bool inAlphabet(Lang lang, Letter letter);
Letter upper(Letter letter);

// UTF-8 to letters. -1 when the text has more than `max` letters or one
// outside the alphabet.
int decode(Lang lang, const char* text, size_t length, Letter* out, int max);
// Letters to NUL-terminated UTF-8; returns the bytes written.
size_t encode(const Letter* letters, int count, char* out, size_t size);

// /x4/words/<lang>/<name>
void listPath(Lang lang, const char* name, char (&out)[PATH_SIZE]);
// /x4/crosswords/<lang>.jsonl
void crosswordPath(Lang lang, char (&out)[PATH_SIZE]);

// Whether `word` is a line of the byte-sorted file at `path`. Binary search
// over byte offsets: a few short reads, never the whole file.
bool contains(const char* path, const char* word, size_t length);
bool contains(const char* path, const Letter* word, int count);

// A line from the part of a most-common-first file that `level` selects
// (1 the most common, 10 the rarest), chosen by `seed`. Lines `accept` refuses
// are skipped; the search wraps around the file once.
using LineFilter = bool (*)(const char* line, void* ctx);
bool pickLine(const char* path, uint32_t seed, uint8_t level, char* out, size_t size, LineFilter accept = nullptr,
              void* ctx = nullptr);
// A line from band `band` (0-based) of `bands` equal slices of the file's
// bytes, chosen by `seed`: a file sorted into equal parts, such as the chess
// puzzles by rating. Skips and wraps like pickLine.
bool pickBand(const char* path, uint32_t seed, uint8_t band, uint8_t bands, char* out, size_t size,
              LineFilter accept = nullptr, void* ctx = nullptr);

// ---- five-letter word -------------------------------------------------------------

enum class Mark : uint8_t { None, Absent, Present, Correct };
// Marks `guess` against `answer`. A repeated letter is Present only as often
// as the answer still has it after the Correct ones.
void score(const Letter* answer, const Letter* guess, Mark* out);

// ---- crossword --------------------------------------------------------------------------------

// 7x7 grids; 5x5 lines written before still play.
inline constexpr uint8_t GRID_MAX = 7;
inline constexpr uint8_t CELLS = GRID_MAX * GRID_MAX;
inline constexpr uint8_t ENTRIES_MAX = 28;  // at most two words per row and per column
inline constexpr size_t CLUE_MAX = 96;      // bytes; longer clues are cut
inline constexpr size_t SRC_MAX = 48;       // bytes of a source title; longer ones are cut
inline constexpr uint8_t SOURCES_MAX = 6;   // distinct sources per puzzle; entries past them have none
inline constexpr size_t ID_MAX = 15;
inline constexpr size_t CROSSWORD_LINE_MAX = 4096;

struct Entry {
  uint8_t number;
  bool across;
  uint8_t start;  // cell
  uint8_t length;
  uint8_t source;  // 1 + index into Crossword::sources; 0 for a word from the word list
  char clue[CLUE_MAX + 1];
};

struct Crossword {
  char id[ID_MAX + 1] = {};
  uint8_t size = 0;          // 5 or 7
  Letter cells[CELLS] = {};  // row by row, size x size; 0 is a black square
  uint8_t numbers[CELLS] = {};
  Entry entries[ENTRIES_MAX] = {};
  uint8_t entryCount = 0;
  // The books and articles the entries came from, each once, in entry order.
  char sources[SOURCES_MAX][SRC_MAX + 1] = {};
  uint8_t sourceCount = 0;

  uint8_t cellCount() const { return static_cast<uint8_t>(size * size); }
  // The entry through `cell` in the direction, or -1.
  int entryAt(uint8_t cell, bool across) const;
  // Cell `k` of entry `e`.
  uint8_t cellOf(const Entry& e, uint8_t k) const {
    return static_cast<uint8_t>(e.across ? e.start + k : e.start + k * size);
  }
};
// One crosswords file line. The clue numbers must match the grid.
bool parseCrossword(Lang lang, const char* json, size_t length, Crossword& out);
// Line `index` (wrapping past the end) of the file; `count` gets the line count.
bool readLine(const char* path, uint32_t index, char* out, size_t size, uint32_t* count = nullptr);

// Where typing goes on a crossword: a white cell and the way its word runs.
struct Spot {
  uint8_t cell = 0;
  bool across = true;
};
// The first white cell, across where a word runs that way.
Spot firstSpot(const Crossword& c);
// An arrow on the grid, `step` 1 or -1. Left and Right run through the white
// cells in reading order, Up and Down in column order, both wrapping. An arrow
// across the typing direction first turns it, where a word runs that way here.
Spot moveSpot(const Crossword& c, Spot at, bool horizontal, int step);
// After a letter at `at` (already in `letters`): the next empty cell of the
// word; in a full word the next cell, and past its end the next word.
Spot afterLetter(const Crossword& c, const Letter* letters, Spot at);
// The cell Delete clears: this one, or the one before it in the word when this one is empty.
Spot eraseSpot(const Crossword& c, const Letter* letters, Spot at);
// The next word in clue order with an empty cell, at its first empty cell;
// in a full grid the next word's first cell.
Spot nextWord(const Crossword& c, const Letter* letters, Spot at);

// Where the crosswords of `lang` continue; device-private state.
uint32_t crosswordNext(Lang lang);
void setCrosswordNext(Lang lang, uint32_t index);

}  // namespace x4::play::words
