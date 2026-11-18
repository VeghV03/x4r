#pragma once

#include <cstddef>
#include <cstdint>

// Shared limits, record layouts and keys for the learning core (docs/DATA.md).
namespace x4 {

// DATA.md limits.
inline constexpr size_t ID_MAX = 32;
inline constexpr size_t TEXT_MAX = 1000;
inline constexpr size_t BODY_MAX = 4000;
inline constexpr uint16_t DECK_CARDS_MAX = 1000;
inline constexpr uint8_t OPTIONS_MIN = 2;
inline constexpr uint8_t OPTIONS_MAX = 4;
inline constexpr uint8_t POINTS_MIN = 1;
inline constexpr uint8_t POINTS_MAX = 5;
// Order items, puzzle options and a piece's link options.
inline constexpr size_t SHORT_MAX = 200;
inline constexpr uint8_t ORDER_MIN = 3;
inline constexpr uint8_t ORDER_MAX = 6;
inline constexpr uint8_t ODD_MIN = 3;
inline constexpr uint8_t ODD_MAX = 5;
// Most rows one step lists (an order puzzle's items).
inline constexpr uint8_t LIST_MAX = 6;

// Longest JSONL line read. The worst valid card (explain: 1000-byte prompt, five
// 1000-byte points and a 1000-byte why) is about 7 KB of raw UTF-8.
inline constexpr size_t JSONL_LINE_MAX = 8192;
// The text arena holds one line, then the parsed item's strings (plus the two
// cloze variants), so it is the line plus room for those derived strings.
inline constexpr size_t ARENA_MAX = JSONL_LINE_MAX + 2 * (TEXT_MAX + 16);
inline constexpr size_t READ_CHUNK = 512;

// New cards asked a day at most.
inline constexpr uint16_t NEW_PER_DAY = 10;

enum class CardType : uint8_t { Flip, Choice, Cloze, Explain };

inline constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
inline constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;

inline uint64_t fnv1a64(const void* data, const size_t len, uint64_t hash = FNV_OFFSET) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

// Hash of the card key "<deck-id>/<card id>".
uint64_t cardKeyHash(const char* deckId, const char* cardId);
// Hash of a stream item id, for the seen-set.
uint64_t itemIdHash(const char* id);

// Scheduler state of one card, stored as-is (little-endian) in
// /x4/state/cards/<deck-id>.bin. 32 bytes: 1000 cards per deck is 32 KB.
struct CardRecord {
  uint64_t key;        // cardKeyHash()
  uint32_t offset;     // byte offset of the card's line in the deck file
  float stability;     // FSRS stability in days; 0 while new
  float difficulty;    // FSRS difficulty, 1..10
  uint32_t lastDay;    // day of the last review; 0 = never
  uint32_t dueDay;     // first day on which the card is due
  uint16_t reps;       // answers so far; 0 = new
  uint8_t lapses;      // Again answers after the first, saturating
  uint8_t type;        // CardType
};
static_assert(sizeof(CardRecord) == 32, "CardRecord is an on-card format");

enum class EntryKind : uint8_t { Card, Item };

// Where a step's line is: a card in its deck, or a stream item.
struct StepLine {
  uint8_t kind;          // EntryKind
  uint8_t sub;           // CardType
  uint16_t record;       // card: record index in the deck state file
  uint32_t offset;       // line offset in the deck file or stream.jsonl
  uint32_t highlightAt;  // highlight item: line offset in highlights.jsonl + 1; 0 = unresolved
  char deck[ID_MAX + 4]; // card: deck id, NUL-terminated
};

}  // namespace x4
