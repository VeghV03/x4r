#include "Sentences.h"

#include <cstdint>
#include <cstring>

namespace sentences {

namespace {

// Words that end with a full stop without ending a sentence. Lower case;
// compared after lower-casing ASCII and Latin-1 capitals.
constexpr const char* ABBREVIATIONS[] = {
    // English
    "mr", "mrs", "ms", "dr", "prof", "st", "sr", "jr", "mt", "vs", "etc", "cf", "approx", "dept", "inc", "ltd", "co",
    "corp", "gen", "gov", "capt", "col", "lt", "sgt", "messrs", "al", "ca", "viz", "ibid", "jan", "feb", "mar", "apr",
    "jun", "jul", "aug", "sep", "sept", "oct", "nov", "dec",
    // Hungarian
    "pl", "stb", "kb", "ill", "ún", "vö", "uo", "ua", "ld", "ifj", "id", "özv", "szül", "ford", "szerk", "tkp", "kft",
    "zrt", "nyrt", "bt", "krt", "febr", "márc", "ápr", "máj", "jún", "júl", "szept", "okt", "évf", "ált", "ker"};

// Abbreviations that stand before a number: "No. 5", "p. 12", "4. sz. 3".
constexpr const char* BEFORE_NUMBER[] = {"no", "nos", "p", "pp", "vol", "fig", "ch", "art", "sz"};

struct Codepoint {
  uint32_t value = 0;
  size_t length = 0;
};

Codepoint first(const std::string_view s) {
  if (s.empty()) return {};
  const auto c = static_cast<uint8_t>(s[0]);
  size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
  if (n > s.size()) n = 1;
  uint32_t value = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
  for (size_t i = 1; i < n; ++i) {
    const auto b = static_cast<uint8_t>(s[i]);
    if ((b & 0xC0) != 0x80) return {c, 1};
    value = (value << 6) | (b & 0x3F);
  }
  return {value, n};
}

Codepoint last(const std::string_view s) {
  if (s.empty()) return {};
  size_t start = s.size() - 1;
  while (start > 0 && s.size() - start < 4 && (static_cast<uint8_t>(s[start]) & 0xC0) == 0x80) --start;
  const Codepoint cp = first(s.substr(start));
  return cp.length == s.size() - start ? cp : Codepoint{static_cast<uint8_t>(s.back()), 1};
}

bool isCloser(const uint32_t c) {
  return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}' || c == 0x2019 || c == 0x201C || c == 0x201D ||
         c == 0x00AB || c == 0x00BB || c == 0x2039 || c == 0x203A;
}

bool isOpener(const uint32_t c) {
  return c == '"' || c == '\'' || c == '(' || c == '[' || c == '{' || c == '-' || c == 0x2013 || c == 0x2014 ||
         c == 0x2018 || c == 0x201A || c == 0x201C || c == 0x201E || c == 0x00AB || c == 0x00BB || c == 0x2039 ||
         c == 0x203A || c == 0x00A1 || c == 0x00BF;
}

bool isTerminal(const uint32_t c) { return c == '.' || c == '!' || c == '?' || c == 0x2026; }

bool isDigit(const uint32_t c) { return c >= '0' && c <= '9'; }

bool isLetter(const uint32_t c) {
  if (c < 0x80) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
  // Latin, Greek and Cyrillic letters; general punctuation and symbols are not.
  return c >= 0xC0 && c != 0xD7 && c != 0xF7 && c < 0x2000;
}

bool isLower(const uint32_t c) {
  if (c < 0x80) return c >= 'a' && c <= 'z';
  if (c >= 0xDF && c <= 0xFF) return c != 0xF7;
  if (c >= 0x100 && c <= 0x17F) {
    // Latin Extended-A pairs capitals and small letters, with the parity flipped
    // in two runs.
    if (c == 0x130) return false;
    if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return true;
    if (c == 0x178) return false;
    const bool flipped = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
    return ((c & 1) != 0) != flipped;
  }
  return (c >= 0x3B1 && c <= 0x3C9) || (c >= 0x430 && c <= 0x45F);
}

std::string_view trimStart(std::string_view s, bool (*drop)(uint32_t)) {
  while (!s.empty()) {
    const Codepoint cp = first(s);
    if (!drop(cp.value)) break;
    s.remove_prefix(cp.length);
  }
  return s;
}

std::string_view trimEnd(std::string_view s, bool (*drop)(uint32_t)) {
  while (!s.empty()) {
    const Codepoint cp = last(s);
    if (!drop(cp.value)) break;
    s.remove_suffix(cp.length);
  }
  return s;
}

bool all(std::string_view s, bool (*test)(uint32_t)) {
  if (s.empty()) return false;
  while (!s.empty()) {
    const Codepoint cp = first(s);
    if (!test(cp.value)) return false;
    s.remove_prefix(cp.length);
  }
  return true;
}

bool closersOnly(const std::string_view s) { return all(s, &isCloser); }

bool isRomanDigit(const uint32_t c) {
  return c == 'I' || c == 'V' || c == 'X' || c == 'L' || c == 'C' || c == 'D' || c == 'M';
}

size_t codepoints(std::string_view s) {
  size_t n = 0;
  while (!s.empty()) {
    s.remove_prefix(first(s).length);
    ++n;
  }
  return n;
}

// "e.g", "i.e", "U.S", "Ph.D": letters in short dotted groups.
bool dottedAcronym(std::string_view core) {
  if (core.find('.') == std::string_view::npos) return false;
  while (!core.empty()) {
    const size_t dot = core.find('.');
    const std::string_view group = core.substr(0, dot);
    if (!all(group, &isLetter) || codepoints(group) > 2) return false;
    if (dot == std::string_view::npos) break;
    core.remove_prefix(dot + 1);
  }
  return true;
}

bool inList(const std::string_view core, const char* const* list, const size_t count) {
  // Longest entry is 6 bytes; a longer core is no abbreviation.
  char lower[8];
  if (core.size() >= sizeof(lower)) return false;
  for (size_t i = 0; i < core.size(); ++i) {
    auto c = static_cast<uint8_t>(core[i]);
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<uint8_t>(c + 32);
    } else if (i > 0 && static_cast<uint8_t>(core[i - 1]) == 0xC3 && c >= 0x80 && c <= 0x9E && c != 0x97) {
      c = static_cast<uint8_t>(c + 32);  // Latin-1 capitals: Á É Í Ó Ö Ú Ü
    }
    lower[i] = static_cast<char>(c);
  }
  const std::string_view key(lower, core.size());
  for (size_t i = 0; i < count; ++i) {
    if (key == list[i]) return true;
  }
  return false;
}

template <size_t N>
bool inList(const std::string_view core, const char* const (&list)[N]) {
  return inList(core, list, N);
}

// What a full stop after `core` means when it is not the end of a sentence.
bool keepsSentence(const std::string_view core, const bool paragraphStart, const std::string_view next) {
  if (inList(core, ABBREVIATIONS) || dottedAcronym(core)) return true;
  if (inList(core, BEFORE_NUMBER) && isDigit(first(trimStart(next, &isOpener)).value)) return true;
  // Initials ("J. R. R. Tolkien") and Roman ordinals ("XIV. Lajos").
  if (codepoints(core) == 1 && isLetter(first(core).value)) return true;
  if (core.size() <= 6 && all(core, &isRomanDigit)) return true;
  // A list marker opening a paragraph: "1. Introduction".
  return paragraphStart && core.size() <= 3 && all(core, &isDigit);
}

}  // namespace

bool canStart(const std::string_view text) {
  const Codepoint cp = first(trimStart(text, &isOpener));
  return cp.length != 0 && !isLower(cp.value);
}

bool endsAt(const WordAt word, const void* ctx, const size_t count, const size_t index) {
  if (index + 1 >= count) return true;
  const Word next = word(ctx, index + 1);
  if (next.paragraphStart) return true;
  // A closing mark set apart belongs to the sentence before it.
  if (closersOnly(next.text)) return false;

  Word current = word(ctx, index);
  for (size_t back = index; closersOnly(current.text) && back > 0 && index - back < 3;) {
    current = word(ctx, --back);
  }
  const std::string_view ending = trimEnd(current.text, &isCloser);
  const Codepoint mark = last(ending);
  if (!isTerminal(mark.value)) return false;

  // A lower-case word after the mark continues the sentence: "Run!" he said.
  for (size_t k = index + 1; k < count && k <= index + 3; ++k) {
    const Word w = word(ctx, k);
    if (k > index + 1 && w.paragraphStart) break;
    const std::string_view text = trimStart(w.text, &isOpener);
    if (text.empty()) continue;
    if (isLower(first(text).value)) return false;
    break;
  }

  if (mark.value != '.') return true;
  std::string_view core = ending.substr(0, ending.size() - 1);
  if (!core.empty() && core.back() == '.') return true;  // "..."
  core = trimStart(core, &isOpener);
  return core.empty() || !keepsSentence(core, current.paragraphStart, next.text);
}

}  // namespace sentences
