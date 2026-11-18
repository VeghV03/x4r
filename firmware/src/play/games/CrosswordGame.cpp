// Crossword: 7x7 puzzles (5x5 ones still play) from /x4/crosswords/<lang>.jsonl in file order.
// Puzzles appended to the file come next; once all are played, earlier ones
// come back by seed until more arrive.
// Two cursors, one moved at a time: the square on the grid and the key on the
// keyboard. On the grid the arrows pick a square (an arrow across the word
// turns it first) and Confirm goes to the keyboard. There Confirm types the
// letter and steps on through the word; Back returns to the grid. The arrow
// key and holding Confirm go to the next word. Solved or given up, the grid
// shows the solution and, under it, the books and articles its words came from,
// until Confirm.
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include "Keyboard.h"
#include "Words.h"
#include "fontIds.h"
#include "learn/ui/LearnUi.h"
#include "play/Game.h"
#include "play/Play.h"
#include "play/Rng.h"
#include "play/ui/PlayUi.h"

namespace x4::play {

using words::Box;
using words::CELLS;
using words::Crossword;
using words::Keyboard;
using words::Letter;
using words::Tile;

bool crosswordReady(const Lang lang) {
  char path[words::PATH_SIZE];
  words::crosswordPath(lang, path);
  return Storage.exists(path);
}

namespace {

constexpr uint8_t STATE_VERSION = 3;  // 3: 7x7
constexpr int CLUE_LINES = 3;
constexpr int CLUE_FONT = NOTOSANS_16_FONT_ID;
constexpr int GAP = 20;

struct State {
  uint8_t version;
  uint8_t lang;
  uint8_t cell;      // the cursor on the grid
  uint8_t across;    // the direction typing goes
  uint8_t onKeys;    // the arrows move the keyboard cursor, not the grid's
  uint8_t key;       // keyboard cursor
  uint8_t done;
  uint8_t unused;  // was "reviewed"; keeps older saves readable
  uint8_t gaveUp;
  uint8_t reserved[3];
  uint32_t index;      // line in the crosswords file
  uint32_t puzzleKey;  // the puzzle on that line
  Letter letters[CELLS];  // the player's; 0 = empty
};

class CrosswordGame final : public Game {
 public:
  void start(const uint32_t seed, uint8_t) override {
    s = State{};
    s.version = STATE_VERSION;
    s.lang = static_cast<uint8_t>(language);
    s.across = 1;
    keyboard.setLang(language);
    uint32_t next = words::crosswordNext(language);
    uint32_t count = 0;
    load(next, &count);
    // A file shorter than where play got to is another file: it starts over.
    if (next > count) next = 0;
    const uint32_t replay = mix32(seed);
    // A damaged line is skipped; the next one is tried.
    for (uint32_t k = 0; k < std::min<uint32_t>(count, 8); ++k) {
      const bool fresh = next + k < count;
      const uint32_t index = fresh ? next + k : (replay + k) % count;
      if (load(index, nullptr)) {
        s.index = index;
        s.puzzleKey = puzzleKey();
        if (fresh) words::setCrosswordNext(language, index + 1);
        go(words::firstSpot(puzzle));
        return;
      }
    }
    LOG_ERR("PLAY", "No crossword for %s", langCode(language));
  }

  bool holdsConfirm() const override { return true; }

  size_t save(uint8_t* out) const override {
    State copy = s;
    copy.key = keyboard.cursor();
    std::memcpy(out, &copy, sizeof(copy));
    return sizeof(copy);
  }

  bool restore(const uint8_t* in, const size_t size) override {
    if (size != sizeof(State)) return false;
    std::memcpy(&s, in, sizeof(s));
    if (s.version != STATE_VERSION || s.lang >= LANGS || s.cell >= CELLS) return false;
    language = static_cast<Lang>(s.lang);
    keyboard.setLang(language);
    keyboard.setCursor(s.key);
    // The file may have changed since: only the same puzzle resumes.
    return load(s.index, nullptr) && puzzleKey() == s.puzzleKey && s.cell < puzzle.cellCount() &&
           puzzle.cells[s.cell] != 0;
  }

  bool input(const Keys& keys, uint32_t) override {
    if (s.done) return false;
    if (keys.confirmHold) return go(words::nextWord(puzzle, s.letters, spot()));
    if (!s.onKeys) {
      if (keys.up || keys.down) return go(words::moveSpot(puzzle, spot(), false, keys.down ? 1 : -1));
      if (keys.left || keys.right) return go(words::moveSpot(puzzle, spot(), true, keys.right ? 1 : -1));
      if (!keys.confirm) return false;
      s.onKeys = 1;
      return true;
    }
    if (keyboard.move(keys, true)) return true;
    if (!keys.confirm) return false;
    switch (keyboard.key()) {
      case Keyboard::ENTER:
        return go(words::nextWord(puzzle, s.letters, spot()));
      case Keyboard::DELETE:
        go(words::eraseSpot(puzzle, s.letters, spot()));
        s.letters[s.cell] = 0;
        return true;
      default:
        s.letters[s.cell] = keyboard.letter();
        if (std::equal(s.letters, s.letters + CELLS, puzzle.cells)) {
          s.done = 1;
          return true;
        }
        return go(words::afterLetter(puzzle, s.letters, spot()));
    }
  }

  bool playable() const override { return puzzle.size != 0; }

  bool back() override {
    if (s.done || !s.onKeys) return false;
    s.onKeys = 0;
    return true;
  }

  void draw(const GfxRenderer& r, const x4::ui::Frame& f) const override {
    if (!playable()) return;  // the host leaves first; never divide by an empty grid
    const int width = r.getScreenWidth();
    const int lineHeight = r.getLineHeight(CLUE_FONT);
    const int kbWidth = std::min(width - 24, 560);
    const int kbHeight = keyboard.height(kbWidth);
    const int clueHeight = CLUE_LINES * lineHeight;
    const int room = f.bottom - f.top - kbHeight - clueHeight - 2 * GAP;
    const int n = puzzle.size;
    const int cells = puzzle.cellCount();
    const int cell = std::min(room, f.width) / n;
    const int gridSize = cell * n;
    const int x0 = (width - gridSize) / 2;
    const int y0 = f.top + (room - gridSize) / 2;
    const bool shown = s.done;  // the solution is on the grid

    // The word being typed is shaded.
    const int active = shown ? -1 : puzzle.entryAt(s.cell, s.across);
    bool full = true;
    for (int i = 0; i < cells; ++i) full = full && (puzzle.cells[i] == 0 || s.letters[i] != 0);
    for (int i = 0; i < cells; ++i) {
      const int cx = x0 + (i % n) * cell;
      const int cy = y0 + (i / n) * cell;
      if (puzzle.cells[i] == 0) {
        r.fillRect(cx, cy, cell, cell, true);
        continue;
      }
      if (active >= 0 && puzzle.entryAt(i, s.across) == active) {
        r.fillRectDither(cx + 1, cy + 1, cell - 1, cell - 1, Color::LightGray);
      }
      const Letter mine = s.letters[i];
      const bool wrong = mine != puzzle.cells[i];
      const Box box{cx + 2, cy + cell / 8, cell - 4, cell - cell / 8};
      if (shown) {
        words::drawTile(r, box, puzzle.cells[i], wrong && s.gaveUp ? Tile::Reveal : Tile::Absent);
      } else if (mine) {
        words::drawTile(r, box, mine, Tile::Absent);
        if (full && wrong) r.fillRect(cx + cell / 4, cy + cell - cell / 6, cell / 2, 2, true);
      }
      if (puzzle.numbers[i]) {
        char n[4];
        snprintf(n, sizeof(n), "%u", static_cast<unsigned>(puzzle.numbers[i]));
        r.drawText(SMALL_FONT_ID, cx + 4, cy + 2, n);
      }
    }
    for (int k = 0; k <= n; ++k) {
      r.fillRect(x0 + k * cell - 1, y0, 2, gridSize, true);
      r.fillRect(x0, y0 + k * cell - 1, gridSize, 2, true);
    }
    // The square typing goes to: thick against the grid lines while the arrows
    // move it, a thin box inside it while they move the keyboard's.
    if (!shown) {
      const int cx = x0 + (s.cell % n) * cell;
      const int cy = y0 + (s.cell / n) * cell;
      if (s.onKeys) {
        const int inset = 1 + ui::FOCUS_PEN;
        ui::drawCursor(r, cx + inset, cy + inset, cell - 2 * inset, cell - 2 * inset, ui::REST_PEN);
      } else {
        ui::drawCursor(r, cx + 1, cy + 1, cell - 2, cell - 2, ui::FOCUS_PEN);
      }
    }

    const int clueTop = y0 + gridSize + GAP;
    if (shown) {
      drawSources(r, f, clueTop);
      return;
    }
    // The clue of the word being typed.
    if (active >= 0) {
      x4::ui::TextBox box{CLUE_FONT, EpdFontFamily::REGULAR, f.left, clueTop, f.width, lineHeight,
                          clueTop + clueHeight};
      x4::ui::drawTextBox(r, box, puzzle.entries[active].clue);
    }
    keyboard.draw(r, {(width - kbWidth) / 2, clueTop + clueHeight + GAP, kbWidth, kbHeight}, nullptr,
                  s.onKeys ? ui::FOCUS_PEN : ui::REST_PEN, words::EnterIcon::Next);
  }

  Outcome outcome() const override {
    if (!s.done) return Outcome::Playing;
    return s.gaveUp ? Outcome::Lost : Outcome::Won;
  }

  const char* lang() const override { return langCode(language); }

 private:
  State s{};
  Crossword puzzle;
  Keyboard keyboard;

  bool load(const uint32_t index, uint32_t* count) {
    char path[words::PATH_SIZE];
    words::crosswordPath(language, path);
    auto line = makeUniqueNoThrow<char[]>(words::CROSSWORD_LINE_MAX);
    if (!line) {
      LOG_ERR("PLAY", "OOM: crossword line (%u bytes)", static_cast<unsigned>(words::CROSSWORD_LINE_MAX));
      return false;
    }
    if (!words::readLine(path, index, line.get(), words::CROSSWORD_LINE_MAX, count)) return false;
    return words::parseCrossword(language, line.get(), std::strlen(line.get()), puzzle);
  }

  // The puzzle's id and solution, hashed (FNV-1a): a rewritten file reuses ids.
  uint32_t puzzleKey() const {
    uint32_t h = 2166136261u;
    const auto add = [&h](const uint8_t byte) { h = (h ^ byte) * 16777619u; };
    for (const char* p = puzzle.id; *p; ++p) add(static_cast<uint8_t>(*p));
    for (const Letter letter : puzzle.cells) {
      add(static_cast<uint8_t>(letter));
      add(static_cast<uint8_t>(letter >> 8));
    }
    return h;
  }

  words::Spot spot() const { return {s.cell, s.across != 0}; }

  // Each book or article the words came from, in bold, over its words. Words from
  // the word list have no source and are not listed.
  void drawSources(const GfxRenderer& r, const x4::ui::Frame& f, int y) const {
    const int lineHeight = r.getLineHeight(CLUE_FONT);
    for (uint8_t k = 0; k < puzzle.sourceCount && y + 2 * lineHeight <= f.bottom; ++k) {
      const std::string title = r.truncatedText(CLUE_FONT, puzzle.sources[k], f.width, EpdFontFamily::BOLD);
      r.drawText(CLUE_FONT, f.left, y, title.c_str(), true, EpdFontFamily::BOLD);
      // The words, upper-case, joined by ", "; what does not fit the line is cut.
      char line[160];
      size_t used = 0;
      line[0] = '\0';
      for (uint8_t e = 0; e < puzzle.entryCount; ++e) {
        const words::Entry& entry = puzzle.entries[e];
        if (entry.source != k + 1) continue;
        Letter word[words::GRID_MAX];
        for (uint8_t i = 0; i < entry.length; ++i) word[i] = words::upper(puzzle.cells[puzzle.cellOf(entry, i)]);
        char text[words::GRID_MAX * 3 + 1];
        const size_t bytes = words::encode(word, entry.length, text, sizeof(text));
        if (used + bytes + 3 > sizeof(line)) break;
        used += snprintf(line + used, sizeof(line) - used, "%s%s", used ? ", " : "", text);
      }
      const std::string answers = r.truncatedText(CLUE_FONT, line, f.width);
      r.drawText(CLUE_FONT, f.left, y + lineHeight, answers.c_str());
      y += 2 * lineHeight + GAP / 2;
    }
  }

  bool go(const words::Spot to) {
    s.cell = to.cell;
    s.across = to.across;
    return true;
  }
};

}  // namespace

std::unique_ptr<Game> makeCrossword() { return std::unique_ptr<Game>(new (std::nothrow) CrosswordGame()); }

}  // namespace x4::play
