#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "JsonlReader.h"
#include "LearnTypes.h"

// Decks and their scheduler state. Each deck /x4/decks/<id>.jsonl has a state
// file /x4/state/cards/<id>.bin: a header, then one CardRecord per valid card
// in deck order. The state file doubles as the deck's index (the line offset
// of every card), so a deck is never held in RAM.
namespace x4::decks {

// docs/DATA.md. Each id costs 33 bytes in the transient set-build and scan buffers.
inline constexpr uint8_t MAX_DECKS = 64;

struct DeckId {
  char id[ID_MAX + 1];
};

struct StateHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t recordSize;
  uint32_t count;
  uint32_t deckSize;      // deck file size when indexed
  uint32_t deckMtime;     // deck file modification time when indexed
  uint32_t shiftSeq;      // the last day shift its records got (shiftDays)
  uint64_t contentHash;   // FNV-1a 64 of the deck file
};
static_assert(sizeof(StateHeader) == 32, "StateHeader is an on-card format");

inline constexpr uint32_t STATE_MAGIC = 0x44433458;  // "X4CD"
inline constexpr uint16_t STATE_VERSION = 1;

void deckPath(const char* deckId, char (&out)[64]);
void statePath(const char* deckId, char (&out)[64]);

// Deck ids in name order. Skips hidden files and names over ID_MAX bytes.
// Past `capacity` decks, keeps the first `capacity` in name order.
uint8_t list(DeckId* out, uint8_t capacity);

// Re-indexes every deck whose file changed (size or modification time; with
// `deep`, also content) and removes the state of decks whose file is gone.
// `ids`/`count` come from list(). Returns true when anything was rebuilt;
// `complete` (optional) turns false when a rebuild failed.
bool sync(const DeckId* ids, uint8_t count, LineBuffer& buffer, bool deep, bool* complete = nullptr);

// Re-indexes one deck, keeping the state of every card whose id survived.
bool rebuild(const char* deckId, LineBuffer& buffer);

// A rebuild writes <state>.tmp, then removes the state file and renames the
// tmp over it. After a power cut between those two steps only a complete tmp
// is left: this renames it back. True when the state file exists afterwards.
bool recoverState(const char* deckId);

// Moves every card's due and last day by `delta` (DATA.md "The day"), unless the state already
// carries `seq`, and marks it with `seq`. The state is written whole and renamed over the old one,
// so a cut by power loss leaves the deck shifted or not. True when the deck carries `seq` after.
bool shiftDays(const char* deckId, int32_t delta, uint32_t seq);

bool readRecord(const char* deckId, uint16_t index, CardRecord& out);
bool writeRecord(const char* deckId, uint16_t index, const CardRecord& record);

// Streams a deck's records in order through a caller buffer.
class RecordCursor {
 public:
  RecordCursor(const char* deckId, uint8_t* chunk, size_t chunkSize);
  ~RecordCursor() { file.close(); }
  bool ok() const { return opened; }
  uint32_t count() const { return header.count; }
  bool next(CardRecord& out, uint16_t& index);

 private:
  HalFile file;
  StateHeader header{};
  uint8_t* chunk;
  size_t chunkRecords;
  size_t inChunk = 0;
  size_t chunkPos = 0;
  uint32_t read = 0;
  bool opened = false;
};

}  // namespace x4::decks
