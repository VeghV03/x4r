#include "Words.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "play/PlayPaths.h"

namespace x4::play::words {

namespace {

constexpr Letter EN[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
                         'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
constexpr Letter HU[] = {'a', 0xE1, 'b', 'c',  'd',  'e',   0xE9, 'f',  'g',   'h', 'i', 0xED,
                         'j', 'k',  'l', 'm',  'n',  'o',   0xF3, 0xF6, 0x151, 'p', 'q', 'r',
                         's', 't',  'u', 0xFA, 0xFC, 0x171, 'v',  'w',  'x',   'y', 'z'};
static_assert(sizeof(HU) / sizeof(HU[0]) == ALPHABET_MAX, "Hungarian has 35 letters here");

// Next code point of UTF-8 at p (advanced), or 0xFFFF for a broken sequence.
uint32_t nextCodepoint(const unsigned char*& p, const unsigned char* end) {
  const unsigned char c = *p++;
  if (c < 0x80) return c;
  int more = 0;
  uint32_t cp = 0;
  if ((c & 0xE0) == 0xC0) {
    more = 1;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    more = 2;
    cp = c & 0x0F;
  } else {
    return 0xFFFF;
  }
  for (; more > 0; --more) {
    if (p >= end || (*p & 0xC0) != 0x80) return 0xFFFF;
    cp = (cp << 6) | (*p++ & 0x3F);
  }
  return cp;
}

bool readAt(HalFile& file, const size_t pos, char* buf, const size_t want, size_t& got) {
  got = 0;
  if (!file.seekSet(pos)) return false;
  const int n = file.read(buf, want);
  if (n < 0) return false;
  got = static_cast<size_t>(n);
  return true;
}

// The first line start at or after `pos`; `end` when there is none.
size_t lineStart(HalFile& file, const size_t pos, const size_t end) {
  if (pos == 0) return 0;
  char buf[LINE_SIZE];
  size_t at = pos - 1;
  while (at < end) {
    size_t got = 0;
    if (!readAt(file, at, buf, sizeof(buf), got) || got == 0) return end;
    const void* nl = std::memchr(buf, '\n', got);
    if (nl) return at + (static_cast<const char*>(nl) - buf) + 1;
    at += got;
  }
  return end;
}

// The line at `pos` without its line end into `out`; returns the bytes it
// takes in the file (0 at the end or on a read error).
size_t lineAt(HalFile& file, const size_t pos, char* out, const size_t size) {
  size_t got = 0;
  if (size < 2 || !readAt(file, pos, out, size - 1, got) || got == 0) return 0;
  const char* nl = static_cast<const char*>(std::memchr(out, '\n', got));
  size_t length = nl ? static_cast<size_t>(nl - out) : got;
  const size_t taken = nl ? length + 1 : got;
  if (!nl && got == size - 1) length = 0;  // too long for `out`: skipped as an empty line
  if (length > 0 && out[length - 1] == '\r') --length;
  out[length] = '\0';
  return taken;
}

int compareLine(const char* line, const size_t lineLength, const char* word, const size_t length) {
  const int c = std::memcmp(line, word, std::min(lineLength, length));
  if (c != 0) return c;
  return lineLength < length ? -1 : lineLength > length ? 1 : 0;
}

bool isBlack(const Crossword& c, const int row, const int col) {
  return row < 0 || col < 0 || row >= c.size || col >= c.size || c.cells[row * c.size + col] == 0;
}

// Copies at most N bytes, never cutting a UTF-8 sequence.
template <size_t N>
void copyText(char (&out)[N + 1], const char* text) {
  size_t n = std::strlen(text);
  if (n > N) {
    n = N;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
  }
  std::memcpy(out, text, n);
  out[n] = '\0';
}

// 1 + the index of source title `text` in `c.sources`, added if new; 0 for none or a full table.
uint8_t sourceOf(Crossword& c, const char* text) {
  if (!*text) return 0;
  char title[SRC_MAX + 1];
  copyText<SRC_MAX>(title, text);
  for (uint8_t k = 0; k < c.sourceCount; ++k) {
    if (std::strcmp(c.sources[k], title) == 0) return k + 1;
  }
  if (c.sourceCount == SOURCES_MAX) return 0;
  std::memcpy(c.sources[c.sourceCount], title, sizeof(title));
  return ++c.sourceCount;
}

void progressPath(const Lang lang, char (&out)[PATH_SIZE]) {
  snprintf(out, sizeof(out), "%s/crossword-%s.bin", paths::STATE, langCode(lang));
}

}  // namespace

uint8_t alphabet(const Lang lang, const Letter** out) {
  if (lang == Lang::Hu) {
    *out = HU;
    return sizeof(HU) / sizeof(HU[0]);
  }
  *out = EN;
  return sizeof(EN) / sizeof(EN[0]);
}

bool inAlphabet(const Lang lang, const Letter letter) {
  const Letter* letters = nullptr;
  const uint8_t n = alphabet(lang, &letters);
  return std::find(letters, letters + n, letter) != letters + n;
}

Letter upper(const Letter l) {
  if (l >= 'a' && l <= 'z') return static_cast<Letter>(l - 0x20);
  if (l >= 0xE0 && l <= 0xFE && l != 0xF7) return static_cast<Letter>(l - 0x20);
  // Latin Extended-A pairs: capital first, small second.
  const bool oddSmall = (l >= 0x100 && l <= 0x137) || (l >= 0x14A && l <= 0x177);
  const bool evenSmall = (l >= 0x139 && l <= 0x148) || (l >= 0x179 && l <= 0x17E);
  if ((oddSmall && (l & 1)) || (evenSmall && !(l & 1))) return static_cast<Letter>(l - 1);
  return l;
}

int decode(const Lang lang, const char* text, const size_t length, Letter* out, const int max) {
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  const auto* end = p + length;
  int n = 0;
  while (p < end) {
    const uint32_t cp = nextCodepoint(p, end);
    if (n == max || cp > 0xFFFF || !inAlphabet(lang, static_cast<Letter>(cp))) return -1;
    out[n++] = static_cast<Letter>(cp);
  }
  return n;
}

size_t encode(const Letter* letters, const int count, char* out, const size_t size) {
  size_t n = 0;
  for (int i = 0; i < count; ++i) {
    const Letter l = letters[i];
    const size_t need = l < 0x80 ? 1 : l < 0x800 ? 2 : 3;
    if (n + need >= size) break;
    if (need == 1) {
      out[n++] = static_cast<char>(l);
    } else if (need == 2) {
      out[n++] = static_cast<char>(0xC0 | (l >> 6));
      out[n++] = static_cast<char>(0x80 | (l & 0x3F));
    } else {
      out[n++] = static_cast<char>(0xE0 | (l >> 12));
      out[n++] = static_cast<char>(0x80 | ((l >> 6) & 0x3F));
      out[n++] = static_cast<char>(0x80 | (l & 0x3F));
    }
  }
  if (size > 0) out[n] = '\0';
  return n;
}

void listPath(const Lang lang, const char* name, char (&out)[PATH_SIZE]) {
  snprintf(out, sizeof(out), "%s/words/%s/%s", paths::ROOT, langCode(lang), name);
}

void crosswordPath(const Lang lang, char (&out)[PATH_SIZE]) {
  snprintf(out, sizeof(out), "%s/crosswords/%s.jsonl", paths::ROOT, langCode(lang));
}

bool contains(const char* path, const char* word, const size_t length) {
  HalFile file;
  if (length == 0 || length >= LINE_SIZE || !Storage.exists(path) || !Storage.openFileForRead("WORDS", path, file)) {
    return false;
  }
  char line[LINE_SIZE + 1];
  size_t lo = 0;
  size_t hi = file.fileSize();
  // The line that would hold `word` starts in [lo, hi).
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const size_t start = lineStart(file, mid, hi);
    if (start >= hi) {
      hi = mid;
      continue;
    }
    const size_t taken = lineAt(file, start, line, sizeof(line));
    if (taken == 0) return false;
    const int c = compareLine(line, std::strlen(line), word, length);
    if (c == 0) return true;
    if (c < 0) {
      lo = start + taken;
    } else {
      hi = mid;
    }
  }
  return false;
}

bool contains(const char* path, const Letter* word, const int count) {
  char text[LETTERS_MAX * 3 + 1];
  const size_t n = encode(word, std::min<int>(count, LETTERS_MAX), text, sizeof(text));
  return contains(path, text, n);
}

namespace {

// A line starting in [from, to) of the open file, chosen by `seed`; see pickLine.
bool pickBetween(HalFile& file, const size_t total, const size_t from, const size_t to, const uint32_t seed, char* out,
                 const size_t size, const LineFilter accept, void* ctx) {
  const size_t origin = lineStart(file, from + seed % (to - from), total);
  size_t pos = origin;
  bool wrapped = false;
  while (true) {
    if (pos >= total) {
      if (wrapped) break;
      wrapped = true;
      pos = 0;
    }
    if (wrapped && pos >= origin) break;
    const size_t taken = lineAt(file, pos, out, size);
    if (taken == 0) break;
    if (out[0] && (!accept || accept(out, ctx))) return true;
    pos += taken;
  }
  return false;
}

}  // namespace

bool pickLine(const char* path, const uint32_t seed, const uint8_t level, char* out, const size_t size,
              const LineFilter accept, void* ctx) {
  HalFile file;
  if (!Storage.exists(path) || !Storage.openFileForRead("WORDS", path, file)) return false;
  const size_t total = file.fileSize();
  if (total == 0) return false;
  const uint8_t l = std::clamp<uint8_t>(level, LEVEL_MIN, LEVEL_MAX);
  // Level 1 draws from the first ~30% of the file, level 10 from the last ~55%.
  const size_t from = total * (l - 1) / 20;
  size_t to = std::min(total, total * (l + 3) / 13);
  if (to <= from) to = total;
  if (pickBetween(file, total, from, to, seed, out, size, accept, ctx)) return true;
  LOG_ERR("WORDS", "No line to pick in %s", path);
  return false;
}

bool pickBand(const char* path, const uint32_t seed, const uint8_t band, const uint8_t bands, char* out,
              const size_t size, const LineFilter accept, void* ctx) {
  HalFile file;
  if (bands == 0 || band >= bands || !Storage.exists(path) || !Storage.openFileForRead("WORDS", path, file)) {
    return false;
  }
  const size_t total = file.fileSize();
  if (total == 0) return false;
  const size_t from = total * band / bands;
  size_t to = total * (band + 1) / bands;
  if (to <= from) to = total;
  if (pickBetween(file, total, from, to, seed, out, size, accept, ctx)) return true;
  LOG_ERR("WORDS", "No line to pick in %s", path);
  return false;
}

void score(const Letter* answer, const Letter* guess, Mark* out) {
  bool used[WORD_LEN] = {};
  for (int i = 0; i < WORD_LEN; ++i) {
    out[i] = guess[i] == answer[i] ? Mark::Correct : Mark::Absent;
    used[i] = out[i] == Mark::Correct;
  }
  for (int i = 0; i < WORD_LEN; ++i) {
    if (out[i] == Mark::Correct) continue;
    for (int j = 0; j < WORD_LEN; ++j) {
      if (!used[j] && answer[j] == guess[i]) {
        used[j] = true;
        out[i] = Mark::Present;
        break;
      }
    }
  }
}

int Crossword::entryAt(const uint8_t cell, const bool across) const {
  for (uint8_t e = 0; e < entryCount; ++e) {
    const Entry& entry = entries[e];
    if (entry.across != across) continue;
    for (uint8_t k = 0; k < entry.length; ++k) {
      if (cellOf(entry, k) == cell) return e;
    }
  }
  return -1;
}

bool parseCrossword(const Lang lang, const char* json, const size_t length, Crossword& out) {
  out = Crossword{};
  JsonDocument doc;
  if (deserializeJson(doc, json, length)) return false;
  const int size = doc["size"] | 0;
  if (size != 5 && size != GRID_MAX) return false;
  out.size = static_cast<uint8_t>(size);
  JsonArrayConst rows = doc["rows"].as<JsonArrayConst>();
  if (rows.size() != out.size) return false;
  for (uint8_t r = 0; r < out.size; ++r) {
    const char* row = rows[r] | "";
    const auto* p = reinterpret_cast<const unsigned char*>(row);
    const auto* end = p + std::strlen(row);
    uint8_t c = 0;
    while (p < end) {
      const uint32_t cp = nextCodepoint(p, end);
      if (c == out.size) return false;
      if (cp != '.' && (cp > 0xFFFF || !inAlphabet(lang, static_cast<Letter>(cp)))) return false;
      out.cells[r * out.size + c++] = cp == '.' ? 0 : static_cast<Letter>(cp);
    }
    if (c != out.size) return false;
  }
  std::strncpy(out.id, doc["id"] | "", ID_MAX);

  // Numbers in reading order: a white cell that starts a run of two or more.
  uint8_t next = 1;
  const uint8_t cells = out.cellCount();
  for (uint8_t i = 0; i < cells; ++i) {
    const int r = i / out.size;
    const int c = i % out.size;
    if (isBlack(out, r, c)) continue;
    const bool across = isBlack(out, r, c - 1) && !isBlack(out, r, c + 1);
    const bool down = isBlack(out, r - 1, c) && !isBlack(out, r + 1, c);
    if (across || down) out.numbers[i] = next++;
  }

  for (const bool across : {true, false}) {
    for (JsonObjectConst item : doc[across ? "across" : "down"].as<JsonArrayConst>()) {
      const int n = item["n"] | 0;
      const uint8_t* at = std::find(out.numbers, out.numbers + cells, n);
      if (n <= 0 || at == out.numbers + cells || out.entryCount == ENTRIES_MAX) return false;
      Entry& e = out.entries[out.entryCount];
      e.number = static_cast<uint8_t>(n);
      e.across = across;
      e.start = static_cast<uint8_t>(at - out.numbers);
      int r = e.start / out.size;
      int c = e.start % out.size;
      if (!isBlack(out, across ? r : r - 1, across ? c - 1 : c)) return false;
      e.length = 0;
      while (!isBlack(out, r, c)) {
        ++e.length;
        (across ? c : r) += 1;
      }
      if (e.length < 2) return false;
      copyText<CLUE_MAX>(e.clue, item["clue"] | "");
      e.source = sourceOf(out, item["src"] | "");
      ++out.entryCount;
    }
  }
  return out.entryCount > 0;
}

bool readLine(const char* path, const uint32_t index, char* out, const size_t size, uint32_t* count) {
  HalFile file;
  if (!Storage.exists(path) || !Storage.openFileForRead("WORDS", path, file)) return false;
  const size_t total = file.fileSize();
  // One pass counts the lines and finds line `index` if the file is long enough.
  char buf[256];
  uint32_t lines = 0;
  size_t start = 0;
  size_t found = total;
  bool open = false;  // a line has begun and not ended
  for (size_t pos = 0; pos < total;) {
    size_t got = 0;
    if (!readAt(file, pos, buf, std::min(sizeof(buf), total - pos), got) || got == 0) return false;
    for (size_t i = 0; i < got; ++i) {
      if (!open) {
        open = true;
        start = pos + i;
      }
      if (buf[i] == '\n') {
        if (lines == index) found = start;
        ++lines;
        open = false;
      }
    }
    pos += got;
  }
  if (open) {
    if (lines == index) found = start;
    ++lines;
  }
  if (count) *count = lines;
  if (lines == 0) return false;
  if (found == total) {
    // Past the end: wrap around. The second pass stops at the line.
    const uint32_t target = index % lines;
    uint32_t line = 0;
    found = 0;
    for (size_t pos = 0; pos < total && line < target;) {
      size_t got = 0;
      if (!readAt(file, pos, buf, std::min(sizeof(buf), total - pos), got) || got == 0) return false;
      for (size_t i = 0; i < got && line < target; ++i) {
        if (buf[i] == '\n' && ++line == target) found = pos + i + 1;
      }
      pos += got;
    }
  }
  return lineAt(file, found, out, size) > 0 && out[0] != '\0';
}

namespace {

// `cell` with the direction `prefer` where a word runs that way, else the other where one does.
Spot settle(const Crossword& c, const uint8_t cell, const bool prefer) {
  if (c.entryAt(cell, prefer) < 0 && c.entryAt(cell, !prefer) >= 0) return {cell, !prefer};
  return {cell, prefer};
}

// Position of `cell` in entry `e`, or -1.
int indexIn(const Crossword& c, const Entry& e, const uint8_t cell) {
  for (uint8_t k = 0; k < e.length; ++k) {
    if (c.cellOf(e, k) == cell) return k;
  }
  return -1;
}

int firstEmpty(const Crossword& c, const Entry& e, const Letter* letters) {
  for (uint8_t k = 0; k < e.length; ++k) {
    if (!letters[c.cellOf(e, k)]) return k;
  }
  return -1;
}

}  // namespace

Spot firstSpot(const Crossword& c) {
  uint8_t cell = 0;
  while (cell + 1 < c.cellCount() && c.cells[cell] == 0) ++cell;
  return settle(c, cell, true);
}

Spot moveSpot(const Crossword& c, const Spot at, const bool horizontal, const int step) {
  if (at.across != horizontal && c.entryAt(at.cell, horizontal) >= 0) return {at.cell, horizontal};
  // Walk the order the arrow runs in: row by row, or column by column.
  const int n = c.size;
  const int cells = c.cellCount();
  int order = horizontal ? at.cell : (at.cell % n) * n + at.cell / n;
  uint8_t cell = at.cell;
  for (int k = 0; k < cells; ++k) {
    order = (order + cells + step) % cells;
    const uint8_t next = static_cast<uint8_t>(horizontal ? order : (order % n) * n + order / n);
    if (c.cells[next]) {
      cell = next;
      break;
    }
  }
  return settle(c, cell, horizontal);
}

Spot afterLetter(const Crossword& c, const Letter* letters, const Spot at) {
  const int e = c.entryAt(at.cell, at.across);
  if (e < 0) return at;
  const Entry& entry = c.entries[e];
  const int k = indexIn(c, entry, at.cell);
  for (int j = k + 1; j < entry.length; ++j) {
    if (!letters[c.cellOf(entry, j)]) return {c.cellOf(entry, j), at.across};
  }
  const int empty = firstEmpty(c, entry, letters);
  if (empty >= 0) return {c.cellOf(entry, empty), at.across};
  if (k + 1 < entry.length) return {c.cellOf(entry, k + 1), at.across};
  return nextWord(c, letters, at);
}

Spot eraseSpot(const Crossword& c, const Letter* letters, const Spot at) {
  if (letters[at.cell]) return at;
  const int e = c.entryAt(at.cell, at.across);
  if (e < 0) return at;
  const int k = indexIn(c, c.entries[e], at.cell);
  return k > 0 ? Spot{c.cellOf(c.entries[e], k - 1), at.across} : at;
}

Spot nextWord(const Crossword& c, const Letter* letters, const Spot at) {
  if (c.entryCount == 0) return at;
  const int current = std::max(0, c.entryAt(at.cell, at.across));
  for (uint8_t i = 1; i <= c.entryCount; ++i) {
    const Entry& entry = c.entries[(current + i) % c.entryCount];
    const int empty = firstEmpty(c, entry, letters);
    if (empty >= 0) return {c.cellOf(entry, empty), entry.across};
  }
  const Entry& entry = c.entries[(current + 1) % c.entryCount];
  return {entry.start, entry.across};
}

uint32_t crosswordNext(const Lang lang) {
  char path[PATH_SIZE];
  progressPath(lang, path);
  HalFile file;
  uint32_t next = 0;
  if (!Storage.exists(path) || !Storage.openFileForRead("WORDS", path, file)) return 0;
  if (file.read(&next, sizeof(next)) != static_cast<int>(sizeof(next))) next = 0;
  return next;
}

void setCrosswordNext(const Lang lang, const uint32_t index) {
  char path[PATH_SIZE];
  progressPath(lang, path);
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  HalFile file = Storage.open(path, O_RDWR | O_CREAT);
  if (!file || !file.seekSet(0) || file.write(&index, sizeof(index)) != sizeof(index)) {
    LOG_ERR("WORDS", "Cannot save crossword progress for %s", langCode(lang));
  }
}

}  // namespace x4::play::words
