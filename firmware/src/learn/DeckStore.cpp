#include "DeckStore.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Item.h"
#include "LearnPaths.h"

namespace x4::decks {

namespace {

constexpr size_t DECK_EXT_LEN = sizeof(paths::DECK_EXT) - 1;
constexpr size_t CARDS_EXT_LEN = sizeof(paths::CARDS_EXT) - 1;

bool endsWith(const char* name, const size_t len, const char* ext, const size_t extLen) {
  return len > extLen && std::strcmp(name + len - extLen, ext) == 0;
}

// Deck ids are [A-Za-z0-9._-] (docs/DATA.md).
bool validDeckId(const char* id, const size_t len) {
  if (len == 0 || len > ID_MAX) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = id[i];
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

bool readHeader(HalFile& file, StateHeader& header) {
  if (!file.seekSet(0) || file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) return false;
  return header.magic == STATE_MAGIC && header.version == STATE_VERSION && header.recordSize == sizeof(CardRecord) &&
         header.count <= DECK_CARDS_MAX;
}

bool hashFile(HalFile& file, LineBuffer& buffer, uint64_t& out) {
  if (!file.seekSet(0)) return false;
  uint64_t hash = FNV_OFFSET;
  while (true) {
    const int got = file.read(buffer.chunk, READ_CHUNK);
    if (got < 0) return false;
    if (got == 0) break;
    hash = fnv1a64(buffer.chunk, static_cast<size_t>(got), hash);
  }
  out = hash;
  return true;
}

// Old records of a deck, sorted by key for lookup during a rebuild.
struct OldIndex {
  std::unique_ptr<uint64_t[]> keys;
  std::unique_ptr<uint16_t[]> order;
  uint32_t count = 0;

  bool load(HalFile& file, const StateHeader& header) {
    if (header.count == 0) return true;
    keys = makeUniqueNoThrow<uint64_t[]>(header.count);
    order = makeUniqueNoThrow<uint16_t[]>(header.count);
    if (!keys || !order) {
      LOG_ERR("DECK", "OOM: old index (%u cards)", static_cast<unsigned>(header.count));
      return false;
    }
    CardRecord record{};
    for (uint32_t i = 0; i < header.count; ++i) {
      if (file.read(&record, sizeof(record)) != static_cast<int>(sizeof(record))) {
        LOG_ERR("DECK", "Truncated state file; deck starts fresh");
        return true;  // count stays 0: nothing carried over
      }
      keys[i] = record.key;
      order[i] = static_cast<uint16_t>(i);
    }
    count = header.count;
    const uint64_t* k = keys.get();
    std::sort(order.get(), order.get() + count, [k](const uint16_t a, const uint16_t b) { return k[a] < k[b]; });
    return true;
  }

  // Record index of `key` in the old file, or -1.
  int find(const uint64_t key) const {
    const uint64_t* k = keys.get();
    const uint16_t* begin = order.get();
    const uint16_t* end = begin + count;
    const uint16_t* it =
        std::lower_bound(begin, end, key, [k](const uint16_t i, const uint64_t v) { return k[i] < v; });
    return it != end && k[*it] == key ? *it : -1;
  }
};

bool byName(const DeckId& a, const DeckId& b) { return std::strcmp(a.id, b.id) < 0; }

void tmpPath(const char* state, char (&out)[72]) { snprintf(out, sizeof(out), "%s.tmp", state); }

bool stateMatches(const char* deckId, LineBuffer& buffer, const bool deep) {
  char deck[64];
  char state[64];
  deckPath(deckId, deck);
  statePath(deckId, state);
  HalFile stateFile;
  if (!Storage.openFileForRead("DECK", state, stateFile)) return false;
  StateHeader header{};
  const bool headerOk = readHeader(stateFile, header);
  stateFile.close();
  if (!headerOk) return false;

  HalFile deckFile;
  if (!Storage.openFileForRead("DECK", deck, deckFile)) return false;
  bool same = deckFile.fileSize() == header.deckSize && deckFile.modificationTime() == header.deckMtime;
  if (same && deep) {
    uint64_t hash = 0;
    same = hashFile(deckFile, buffer, hash) && hash == header.contentHash;
  }
  deckFile.close();
  return same;
}

}  // namespace

void deckPath(const char* deckId, char (&out)[64]) {
  snprintf(out, sizeof(out), "%s/%s%s", paths::DECKS, deckId, paths::DECK_EXT);
}

void statePath(const char* deckId, char (&out)[64]) {
  snprintf(out, sizeof(out), "%s/%s%s", paths::CARDS, deckId, paths::CARDS_EXT);
}

uint8_t list(DeckId* out, const uint8_t capacity) {
  HalFile dir = Storage.open(paths::DECKS);
  if (!dir || !dir.isDirectory()) return 0;
  uint8_t count = 0;
  char name[ID_MAX + DECK_EXT_LEN + 8];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDir = entry.isDirectory();
    entry.getName(name, sizeof(name));
    entry.close();
    const size_t len = std::strlen(name);
    if (isDir || name[0] == '.' || !endsWith(name, len, paths::DECK_EXT, DECK_EXT_LEN)) continue;
    const size_t stem = len - DECK_EXT_LEN;
    if (len + 1 >= sizeof(name) || !validDeckId(name, stem)) {
      LOG_ERR("DECK", "Bad deck id: %s", name);
      continue;
    }
    name[stem] = '\0';
    DeckId* slot = out + count;
    if (count == capacity) {
      // Full: keep the first `capacity` in name order, whatever the directory order.
      slot = std::max_element(out, out + count, byName);
      const bool keep = capacity > 0 && std::strcmp(name, slot->id) < 0;
      LOG_ERR("DECK", "More than %u decks; ignoring %s", static_cast<unsigned>(capacity), keep ? slot->id : name);
      if (!keep) continue;
    } else {
      ++count;
    }
    std::memcpy(slot->id, name, stem + 1);
  }
  dir.close();
  std::sort(out, out + count, byName);
  return count;
}

bool rebuild(const char* deckId, LineBuffer& buffer) {
  char deck[64];
  char state[64];
  char tmp[72];
  deckPath(deckId, deck);
  statePath(deckId, state);
  tmpPath(state, tmp);
  Storage.mkdir(paths::STATE);
  Storage.mkdir(paths::CARDS);
  // The tmp may be the only copy of the deck's history; never truncate it unread.
  recoverState(deckId);

  OldIndex old;
  HalFile oldFile;
  StateHeader oldHeader{};
  if (Storage.exists(state) && Storage.openFileForRead("DECK", state, oldFile)) {
    if (!readHeader(oldFile, oldHeader)) {
      // Unreadable or older format: start the deck fresh rather than fail forever.
      oldFile.close();
    } else if (!old.load(oldFile, oldHeader)) {
      // Keep the old state (and its history) until there is memory to carry it over.
      oldFile.close();
      return false;
    }
  }

  HalFile deckFile;
  if (!Storage.openFileForRead("DECK", deck, deckFile)) {
    LOG_ERR("DECK", "Cannot open %s", deck);
    return false;
  }
  StateHeader header{STATE_MAGIC, STATE_VERSION, sizeof(CardRecord), 0, 0, 0, 0, 0};
  header.shiftSeq = oldHeader.shiftSeq;  // the records carried over keep their shift
  header.deckSize = static_cast<uint32_t>(deckFile.fileSize());
  header.deckMtime = deckFile.modificationTime();
  if (!hashFile(deckFile, buffer, header.contentHash)) {
    LOG_ERR("DECK", "Cannot read %s", deck);
    return false;
  }

  HalFile out;
  if (!Storage.openFileForWrite("DECK", tmp, out)) return false;
  // The real header goes in last, so a tmp cut short never reads as complete.
  StateHeader unfinished = header;
  unfinished.magic = 0;
  bool ok = out.write(&unfinished, sizeof(unfinished)) == sizeof(unfinished);

  JsonlReader reader(deckFile, buffer);
  ok = ok && reader.seek(0);
  size_t length = 0;
  uint32_t offset = 0;
  char id[ID_MAX + 1];
  CardType type{};
  JsonlReader::Line line;
  while (ok && (line = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (line != JsonlReader::Line::Ok) continue;
    if (!scanCard(buffer.text, length, id, type)) continue;  // header line or invalid card
    if (header.count == DECK_CARDS_MAX) {
      LOG_ERR("DECK", "%s has more than %u cards", deckId, static_cast<unsigned>(DECK_CARDS_MAX));
      break;
    }
    CardRecord record{};
    record.key = cardKeyHash(deckId, id);
    const int previous = old.count ? old.find(record.key) : -1;
    if (previous >= 0) {
      ok = oldFile.seekSet(sizeof(StateHeader) + static_cast<size_t>(previous) * sizeof(CardRecord)) &&
           oldFile.read(&record, sizeof(record)) == static_cast<int>(sizeof(record));
    }
    record.offset = offset;
    record.type = static_cast<uint8_t>(type);
    ok = ok && out.write(&record, sizeof(record)) == sizeof(record);
    ++header.count;
  }
  deckFile.close();
  if (oldFile) oldFile.close();
  ok = ok && out.seekSet(0) && out.write(&header, sizeof(header)) == sizeof(header);
  ok = out.close() && ok;
  if (!ok || !Storage.replaceFile(tmp, state)) {
    LOG_ERR("DECK", "Rebuild of %s failed", deckId);
    Storage.remove(tmp);
    return false;
  }
  LOG_INF("DECK", "Indexed %s: %u cards", deckId, static_cast<unsigned>(header.count));
  return true;
}

namespace {

uint32_t shifted(const uint32_t day, const int32_t delta) {
  if (day == 0) return 0;
  const int64_t moved = static_cast<int64_t>(day) + delta;
  return moved < 1 ? 1 : static_cast<uint32_t>(moved);
}

}  // namespace

bool shiftDays(const char* deckId, const int32_t delta, const uint32_t seq) {
  char state[64];
  char tmp[72];
  statePath(deckId, state);
  tmpPath(state, tmp);
  recoverState(deckId);
  HalFile in;
  if (!Storage.exists(state) || !Storage.openFileForRead("DECK", state, in)) return true;  // nothing to move
  StateHeader header{};
  if (!readHeader(in, header) || header.version != STATE_VERSION || header.recordSize != sizeof(CardRecord)) {
    in.close();
    return true;  // rebuilt from the deck on the next sync
  }
  if (header.shiftSeq == seq) {
    in.close();
    return true;
  }
  HalFile out;
  if (!Storage.openFileForWrite("DECK", tmp, out)) {
    in.close();
    return false;
  }
  StateHeader unfinished = header;
  unfinished.magic = 0;
  bool ok = out.write(&unfinished, sizeof(unfinished)) == sizeof(unfinished);
  for (uint32_t i = 0; ok && i < header.count; ++i) {
    CardRecord record{};
    ok = in.read(&record, sizeof(record)) == static_cast<int>(sizeof(record));
    record.dueDay = shifted(record.dueDay, delta);
    record.lastDay = shifted(record.lastDay, delta);
    ok = ok && out.write(&record, sizeof(record)) == sizeof(record);
  }
  in.close();
  header.shiftSeq = seq;
  ok = ok && out.seekSet(0) && out.write(&header, sizeof(header)) == sizeof(header);
  ok = out.close() && ok;
  if (!ok || !Storage.replaceFile(tmp, state)) {
    LOG_ERR("DECK", "Day shift of %s failed", deckId);
    Storage.remove(tmp);
    return false;
  }
  return true;
}

bool recoverState(const char* deckId) {
  char state[64];
  char tmp[72];
  statePath(deckId, state);
  tmpPath(state, tmp);
  if (Storage.exists(state)) return true;
  if (!Storage.exists(tmp)) return false;
  HalFile file;
  if (!Storage.openFileForRead("DECK", tmp, file)) return false;
  StateHeader header{};
  const bool complete =
      readHeader(file, header) && file.fileSize() == sizeof(StateHeader) + header.count * sizeof(CardRecord);
  file.close();
  if (!complete || !Storage.rename(tmp, state)) return false;
  LOG_INF("DECK", "Recovered the state of %s", deckId);
  return true;
}

bool sync(const DeckId* ids, const uint8_t count, LineBuffer& buffer, const bool deep, bool* complete) {
  bool changed = false;
  for (uint8_t i = 0; i < count; ++i) {
    recoverState(ids[i].id);
    if (stateMatches(ids[i].id, buffer, deep)) continue;
    if (rebuild(ids[i].id, buffer)) {
      changed = true;
    } else if (complete) {
      *complete = false;
    }
  }

  HalFile dir = Storage.open(paths::CARDS);
  if (!dir || !dir.isDirectory()) return changed;
  char name[ID_MAX + CARDS_EXT_LEN + 8];
  char orphan[64];
  bool removed = true;
  // Removing while iterating can disturb the directory walk; restart after each removal.
  while (removed) {
    removed = false;
    dir.rewindDirectory();
    while (true) {
      HalFile entry = dir.openNextFile();
      if (!entry) break;
      entry.getName(name, sizeof(name));
      entry.close();
      const size_t len = std::strlen(name);
      if (!endsWith(name, len, paths::CARDS_EXT, CARDS_EXT_LEN)) continue;
      name[len - CARDS_EXT_LEN] = '\0';
      const bool listed = std::any_of(ids, ids + count, [&](const DeckId& d) { return std::strcmp(d.id, name) == 0; });
      if (listed) continue;
      // A deck list() left out (over MAX_DECKS) still has its file: keep its history.
      deckPath(name, orphan);
      if (Storage.exists(orphan)) continue;
      statePath(name, orphan);
      Storage.remove(orphan);
      removed = true;
      changed = true;
      break;
    }
  }
  dir.close();
  return changed;
}

bool readRecord(const char* deckId, const uint16_t index, CardRecord& out) {
  char state[64];
  statePath(deckId, state);
  HalFile file;
  if (!Storage.openFileForRead("DECK", state, file)) return false;
  StateHeader header{};
  const bool ok = readHeader(file, header) && index < header.count &&
                  file.seekSet(sizeof(StateHeader) + static_cast<size_t>(index) * sizeof(CardRecord)) &&
                  file.read(&out, sizeof(out)) == static_cast<int>(sizeof(out));
  file.close();
  return ok;
}

bool writeRecord(const char* deckId, const uint16_t index, const CardRecord& record) {
  char state[64];
  statePath(deckId, state);
  HalFile file = Storage.open(state, O_RDWR);
  if (!file) return false;
  StateHeader header{};
  bool ok = readHeader(file, header) && index < header.count &&
            file.seekSet(sizeof(StateHeader) + static_cast<size_t>(index) * sizeof(CardRecord)) &&
            file.write(&record, sizeof(record)) == sizeof(record);
  ok = file.close() && ok;
  if (!ok) LOG_ERR("DECK", "Cannot write record %u of %s", static_cast<unsigned>(index), deckId);
  return ok;
}

RecordCursor::RecordCursor(const char* deckId, uint8_t* chunk, const size_t chunkSize)
    : chunk(chunk), chunkRecords(chunkSize / sizeof(CardRecord)) {
  char state[64];
  statePath(deckId, state);
  opened = chunkRecords > 0 && Storage.openFileForRead("DECK", state, file) && readHeader(file, header);
}

bool RecordCursor::next(CardRecord& out, uint16_t& index) {
  if (!opened || read >= header.count) return false;
  if (chunkPos >= inChunk) {
    const size_t want = std::min<size_t>(chunkRecords, header.count - read);
    const int got = file.read(chunk, want * sizeof(CardRecord));
    if (got != static_cast<int>(want * sizeof(CardRecord))) {
      opened = false;
      return false;
    }
    inChunk = want;
    chunkPos = 0;
  }
  std::memcpy(&out, chunk + chunkPos * sizeof(CardRecord), sizeof(CardRecord));
  ++chunkPos;
  index = static_cast<uint16_t>(read++);
  return true;
}

}  // namespace x4::decks
