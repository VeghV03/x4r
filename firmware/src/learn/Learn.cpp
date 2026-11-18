#include "Learn.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "BookStats.h"
#include "DeckStore.h"
#include "LearnPaths.h"
#include "ReadList.h"
#include "StateFile.h"

namespace x4::learn {

namespace {

// ---- learn.bin: two CRC-checked slots, written alternately -----------------

constexpr uint32_t STATE_MAGIC = 0x53433458;  // "X4CS"
constexpr uint16_t STATE_VERSION = 2;
constexpr uint16_t STATE_VERSION_SETS = 1;  // counted sets; read once and moved to days (fromSets)

struct StateSlot {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t seq;
  LearnState state;
  uint32_t crc;
};

uint32_t crc32(const void* data, const size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

// ---- seen.bin: hashes of stream items already shown ------------------------

constexpr uint32_t SEEN_MAGIC = 0x53533458;  // "X4SS"
constexpr uint16_t SEEN_MAX = 512;
constexpr uint16_t STREAM_SCAN_MAX = 512;
// Bounds the highlights scan for the sleep screen.
constexpr uint16_t SLEEP_HIGHLIGHT_SCAN = 256;

struct SeenHeader {
  uint32_t magic;
  uint32_t count;
};

// What ensureReady() needs, heap-owned while it runs (about 13 KB).
struct Work {
  LineBuffer buffer;
  decks::DeckId decks[decks::MAX_DECKS];
  uint64_t seen[SEEN_MAX];
  uint8_t keep[SEEN_MAX / 8];
};

LearnState g{};
bool loaded = false;
bool bootChecked = false;
bool hostDataChanged = false;
bool hostClock = false;
uint32_t stateSeq = 0;

void freshState() {
  g = LearnState{};
  g.retention = -1.0f;
}

void fromSets();

void loadState() {
  if (loaded) return;
  loaded = true;
  statefile::recover(paths::SEEN_TMP, paths::SEEN);
  statefile::recover(paths::STATS_TMP, paths::STATS);
  freshState();
  HalFile file;
  if (!Storage.openFileForRead("LEARN", paths::LEARN_STATE, file)) return;
  StateSlot best{};
  bool found = false;
  for (int i = 0; i < 2; ++i) {
    StateSlot slot{};
    if (!file.seekSet(i * sizeof(StateSlot)) || file.read(&slot, sizeof(slot)) != static_cast<int>(sizeof(slot))) {
      continue;
    }
    if (slot.magic != STATE_MAGIC || (slot.version != STATE_VERSION && slot.version != STATE_VERSION_SETS) ||
        slot.size != sizeof(LearnState) ||
        slot.crc != crc32(&slot, offsetof(StateSlot, crc))) {
      continue;
    }
    if (!found || slot.seq > best.seq) {
      best = slot;
      found = true;
    }
  }
  file.close();
  if (!found) {
    LOG_ERR("LEARN", "No valid learn state; starting fresh");
    return;
  }
  g = best.state;
  stateSeq = best.seq;
  if (best.version == STATE_VERSION_SETS) fromSets();
}

void saveState() {
  StateSlot slot{STATE_MAGIC, STATE_VERSION, sizeof(LearnState), ++stateSeq, g, 0};
  slot.crc = crc32(&slot, offsetof(StateSlot, crc));
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  HalFile file = Storage.open(paths::LEARN_STATE, O_RDWR | O_CREAT);
  bool ok = static_cast<bool>(file);
  if (ok && file.fileSize() < 2 * sizeof(StateSlot)) {
    // New file: fill both slots so the alternate slot can be seeked to later.
    ok = file.seekSet(0) && file.write(&slot, sizeof(slot)) == sizeof(slot) &&
         file.write(&slot, sizeof(slot)) == sizeof(slot);
  } else if (ok) {
    ok = file.seekSet((stateSeq & 1u) * sizeof(StateSlot)) && file.write(&slot, sizeof(slot)) == sizeof(slot);
  }
  if (file) ok = file.close() && ok;
  if (!ok) LOG_ERR("LEARN", "Cannot save learn state");
}

// ---- the day (DATA.md "The day") -------------------------------------------

// Finishes the shift of every deck and of books.bin. False while a file still lacks it; the next
// power-on tries again.
bool applyShift() {
  if (g.shiftDelta == 0) return true;
  auto ids = makeUniqueNoThrow<decks::DeckId[]>(decks::MAX_DECKS);
  if (!ids) return false;
  const uint8_t count = decks::list(ids.get(), decks::MAX_DECKS);
  bool ok = true;
  for (uint8_t i = 0; i < count; ++i) ok = decks::shiftDays(ids[i].id, g.shiftDelta, g.shiftSeq) && ok;
  ok = books::shiftDays(g.shiftDelta, g.shiftSeq) && ok;
  if (!ok) {
    LOG_ERR("LEARN", "Day shift of %d unfinished", static_cast<int>(g.shiftDelta));
    return false;
  }
  LOG_INF("LEARN", "Cards and books moved by %d days", static_cast<int>(g.shiftDelta));
  g.shiftDelta = 0;
  saveState();
  return true;
}

// Every card and book moves by `delta` days with the day, so no interval changes. Saved with
// the day by the caller, then applied.
void beginShift(const int32_t delta) {
  if (delta == 0) return;
  applyShift();  // one shift at a time
  ++g.shiftSeq;
  g.shiftDelta = delta;
}

// The day is the date. The first date of an undated day, or one behind the day counted offline
// (X26), moves every card and book with it.
void noteClock(const int64_t now, const int32_t tzMinutes) {
  bool changed = g.tzMinutes != tzMinutes;
  g.tzMinutes = tzMinutes;
  const int64_t date = now > 0 ? civilDay(now, tzMinutes) : 0;
  if (date > 0 && date <= static_cast<int64_t>(UINT32_MAX)) {
    const auto day = static_cast<uint32_t>(date);
    if (g.day != 0 && (!g.dated || day < g.day)) beginShift(static_cast<int32_t>(date - g.day));
    changed = changed || !g.dated || g.countMs != 0 || day != g.day;
    g.dated = 1;
    g.countMs = 0;
    if (day != g.day) startDay(g, day);
  }
  if (!changed) return;
  saveState();
  applyShift();
}

// Version 1 counted sets (X27): a card due in n sets is due in n days, counted from the day the
// X4 last knew the date. A state that never saw a clock keeps counting, undated.
void fromSets() {
  const uint32_t set = g.day;
  const int64_t lastClock = g.unused1;  // version 1's streak anchor: the last time a clock was seen
  g.dated = 0;
  g.unused0 = 0;
  g.countMs = 0;
  g.shiftSeq = 0;
  g.unused1 = 0;
  g.shiftDelta = 0;
  const int64_t date = lastClock > 0 ? civilDay(lastClock, g.tzMinutes) : 0;
  if (set != 0 && date > 0 && date <= static_cast<int64_t>(UINT32_MAX)) {
    beginShift(static_cast<int32_t>(date - set));
    g.day = static_cast<uint32_t>(date);
    g.dated = 1;
  }
  LOG_INF("LEARN", "Set %u is now day %u", static_cast<unsigned>(set), static_cast<unsigned>(g.day));
  saveState();
  applyShift();
}

// ---- seen-set --------------------------------------------------------------

uint16_t loadSeen(uint64_t* out) {
  HalFile file;
  if (!Storage.exists(paths::SEEN) || !Storage.openFileForRead("LEARN", paths::SEEN, file)) return 0;
  SeenHeader header{};
  uint16_t count = 0;
  if (file.read(&header, sizeof(header)) == static_cast<int>(sizeof(header)) && header.magic == SEEN_MAGIC) {
    const uint32_t want = std::min<uint32_t>(header.count, SEEN_MAX);
    const int got = file.read(out, want * sizeof(uint64_t));
    count = got > 0 ? static_cast<uint16_t>(static_cast<size_t>(got) / sizeof(uint64_t)) : 0;
  }
  file.close();
  return count;
}

bool writeSeenSet(const uint64_t* hashes, const uint16_t count) {
  Storage.mkdir(paths::STATE);
  const SeenHeader header{SEEN_MAGIC, count};
  return statefile::replace(paths::SEEN_TMP, paths::SEEN,
                            {{&header, sizeof(header)}, {hashes, count * sizeof(uint64_t)}});
}

void addSeen(const uint64_t hash) {
  Storage.mkdir(paths::STATE);
  HalFile file = Storage.open(paths::SEEN, O_RDWR | O_CREAT);
  if (!file) return;
  SeenHeader header{};
  if (file.fileSize() < sizeof(header) || file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header)) ||
      header.magic != SEEN_MAGIC) {
    header = {SEEN_MAGIC, 0};
    if (!file.seekSet(0) || file.write(&header, sizeof(header)) != sizeof(header) || !file.truncate(sizeof(header))) {
      file.close();
      LOG_ERR("LEARN", "Cannot start seen-set");
      return;
    }
  }
  if (header.count >= SEEN_MAX) {
    // Full: the oldest entry is the one the host most likely dropped already.
    file.close();
    auto hashes = makeUniqueNoThrow<uint64_t[]>(SEEN_MAX);
    if (!hashes) return;
    const uint16_t count = loadSeen(hashes.get());
    if (count == 0) return;
    std::memmove(hashes.get(), hashes.get() + 1, (count - 1) * sizeof(uint64_t));
    hashes[count - 1] = hash;
    writeSeenSet(hashes.get(), count);
    return;
  }
  ++header.count;
  bool ok = file.seekSet(sizeof(header) + (header.count - 1) * sizeof(uint64_t)) &&
            file.write(&hash, sizeof(hash)) == sizeof(hash) && file.seekSet(0) &&
            file.write(&header, sizeof(header)) == sizeof(header);
  ok = file.close() && ok;
  if (!ok) LOG_ERR("LEARN", "Cannot update seen-set");
}

// ---- totals and building ---------------------------------------------------

// Drops the seen ids no longer in the stream: those items can never come back.
void pruneSeen(Work& w) {
  const uint16_t seenCount = loadSeen(w.seen);
  if (seenCount == 0) return;
  std::memset(w.keep, 0, sizeof(w.keep));
  // Pruning needs every line read: a seen id past the cap or behind a failed open is still live.
  bool wholeStream = !Storage.exists(paths::STREAM);
  HalFile file;
  if (!wholeStream && Storage.openFileForRead("LEARN", paths::STREAM, file)) {
    JsonlReader reader(file, w.buffer);
    size_t length = 0;
    uint32_t offset = 0;
    JsonlReader::Line line = JsonlReader::Line::Ok;
    uint16_t scanned = 0;
    char id[ID_MAX + 1];
    while (scanned < STREAM_SCAN_MAX && (line = reader.next(length, offset)) != JsonlReader::Line::End) {
      if (line != JsonlReader::Line::Ok) continue;
      ++scanned;
      if (!scanId(w.buffer.text, length, id)) continue;
      const uint64_t* seenEnd = w.seen + seenCount;
      const uint64_t* hit = std::find(static_cast<const uint64_t*>(w.seen), seenEnd, itemIdHash(id));
      if (hit == seenEnd) continue;
      const size_t at = static_cast<size_t>(hit - w.seen);
      w.keep[at / 8] |= static_cast<uint8_t>(1u << (at % 8));
    }
    wholeStream = line == JsonlReader::Line::End;
    file.close();
  }
  if (!wholeStream) return;
  uint16_t kept = 0;
  for (uint16_t i = 0; i < seenCount; ++i) {
    if (w.keep[i / 8] & (1u << (i % 8))) w.seen[kept++] = w.seen[i];
  }
  if (kept != seenCount) writeSeenSet(w.seen, kept);
}

struct Totals {
  uint32_t cards = 0;
  uint32_t due = 0;
  uint32_t reviewed = 0;
  double recall = 0.0;
};

void applyTotals(const Totals& totals) {
  g.totalsDay = g.day;
  g.cards = totals.cards;
  g.due = totals.due;
  g.retention = totals.reviewed ? static_cast<float>(totals.recall / totals.reviewed) : -1.0f;
}

// What a totals-only scan needs (about 1.6 KB).
struct ScanWork {
  decks::DeckId decks[decks::MAX_DECKS];
  uint8_t chunk[READ_CHUNK];
};

// One pass over every deck's records.
Totals countCards(const decks::DeckId* ids, const uint8_t deckCount, uint8_t* chunk) {
  Totals totals;
  CardRecord record{};
  uint16_t index = 0;
  for (uint8_t d = 0; d < deckCount; ++d) {
    decks::RecordCursor cursor(ids[d].id, chunk, READ_CHUNK);
    while (cursor.next(record, index)) {
      ++totals.cards;
      if (record.reps == 0) continue;
      ++totals.reviewed;
      totals.recall += fsrs::recordRetrievability(record, g.day);
      if (record.dueDay <= g.day && record.lastDay != g.day) ++totals.due;
    }
  }
  return totals;
}

void clearHostChanged() {
  hostDataChanged = false;
  if (Storage.exists(paths::HOST_CHANGED) && !Storage.remove(paths::HOST_CHANGED)) {
    LOG_ERR("LEARN", "Cannot clear the host-change marker");
  }
}

void refreshTotals() {
  auto w = makeUniqueNoThrow<ScanWork>();
  if (!w) {
    LOG_ERR("LEARN", "OOM: totals (%u bytes)", static_cast<unsigned>(sizeof(ScanWork)));
    return;
  }
  const uint8_t deckCount = decks::list(w->decks, decks::MAX_DECKS);
  applyTotals(countCards(w->decks, deckCount, w->chunk));
}

bool loadEntry(LineBuffer& buffer, const StepLine& entry, Item& item) {
  size_t length = 0;
  if (entry.kind == static_cast<uint8_t>(EntryKind::Card)) {
    CardRecord record{};
    char deck[64];
    decks::deckPath(entry.deck, deck);
    if (!decks::readRecord(entry.deck, entry.record, record) || record.offset != entry.offset) return false;
    if (!readLineAt(deck, entry.offset, buffer, length) || !parseCard(buffer.text, length, item)) return false;
    return cardKeyHash(entry.deck, item.id) == record.key;
  }
  if (!readLineAt(paths::STREAM, entry.offset, buffer, length) || !parseStreamItem(buffer.text, length, item)) {
    return false;
  }
  if (item.shape != Shape::Highlight || item.body.length > 0) return true;
  if (entry.highlightAt == 0) return false;
  return readLineAt(paths::HIGHLIGHTS, entry.highlightAt - 1, buffer, length) &&
         parseHighlightInto(buffer.text, length, item);
}

void bookStats(JsonObject out) {
  books::addTo(out);
  addBookStats(out);
}

bool statsStale = false;

void writeStatsNow() {
  statsStale = false;
  if (g.totalsDay != g.day) refreshTotals();  // answered since they were counted
  writeStats(g, &bookStats, &addPlayStats);
}

}  // namespace

void ensureReady() {
  loadState();
  const bool firstCall = !bootChecked;
  bootChecked = true;
  if (firstCall && Storage.exists(paths::HOST_CHANGED)) hostDataChanged = true;
  // Stays set until a sync succeeds, so a failed one is retried.
  const bool deep = hostDataChanged;
  if (firstCall) applyShift();
  // A clock the host set before deep sleep is still the host's clock.
  int32_t tz = g.tzMinutes;
  if (firstCall && platformHostClock(tz)) noteClock(platformNow(), tz);
  if (firstCall && openFirstDay(g)) saveState();
  if (!firstCall && !deep) return;

  auto w = makeUniqueNoThrow<Work>();
  if (!w) {
    LOG_ERR("LEARN", "OOM: deck sync (%u bytes)", static_cast<unsigned>(sizeof(Work)));
    return;
  }
  const uint8_t deckCount = decks::list(w->decks, decks::MAX_DECKS);
  bool complete = true;
  const bool decksChanged = decks::sync(w->decks, deckCount, w->buffer, deep, &complete);
  if (deep) pruneSeen(*w);
  if (decksChanged || deep || g.totalsDay != g.day) {
    applyTotals(countCards(w->decks, deckCount, w->buffer.chunk));
    saveState();
    w.reset();
    writeStatsNow();
  }
  if (deep && complete) clearHostChanged();
}

const LearnState& state() {
  loadState();
  return g;
}

namespace {

// Schedules the card and logs the review. False when the card changed under the step.
bool recordAnswer(const Step& step, const Item& item, const Answer& answer, const uint32_t ms) {
  CardRecord record{};
  if (!decks::readRecord(step.entry.deck, step.entry.record, record) || record.offset != step.entry.offset) {
    return false;
  }
  const bool wasNew = record.reps == 0;
  fsrs::schedule(record, g.day, answer.grade);
  decks::writeRecord(step.entry.deck, step.entry.record, record);
  writeReview(step.entry.deck, item.id, static_cast<CardType>(step.entry.sub), answer.grade, answer.correct,
              answer.pick, ms, g.day, wallClock());
  if (wasNew) ++g.newToday;
  return true;
}

// ---- questions ---------------------------------------------------------------

// Scratch for a question scan (about 2.6 KB).
struct QuestionScan {
  decks::DeckId decks[decks::MAX_DECKS];
  uint8_t chunk[READ_CHUNK];
};

const char* keyOf(const char* path) { return *path == '/' ? path + 1 : path; }

enum class DeckOwner : uint8_t { Other, Book, Nobody };

// Whose questions a deck holds, from its header line.
DeckOwner deckOwner(const char* deckId, const char* bookKey, LineBuffer& buffer) {
  char path[64];
  decks::deckPath(deckId, path);
  size_t length = 0;
  if (!readLineAt(path, 0, buffer, length)) return DeckOwner::Other;
  JsonDocument doc;
  if (deserializeJson(doc, buffer.text, length) || !doc["deck"].is<const char*>()) return DeckOwner::Other;
  const char* book = doc["book"].as<const char*>();
  if (!book || !*book) return DeckOwner::Nobody;
  return std::strcmp(keyOf(book), bookKey) == 0 ? DeckOwner::Book : DeckOwner::Other;
}

// The first question for `bookPath` in `out`; returns how many are waiting.
// Tiers, asked in order: the book's due cards, the bookless due ones (after an
// article), the book's new cards, the bookless new ones. Due cards go most likely
// forgotten first, new ones in deck order.
uint16_t findQuestion(const char* bookPath, LineBuffer& buffer, QuestionScan& scan, StepLine& out) {
  loadState();
  const char* key = keyOf(bookPath);
  const bool article = readlist::isReadBook(bookPath);
  const bool freshAllowed = g.newToday < NEW_PER_DAY;
  const uint8_t deckCount = decks::list(scan.decks, decks::MAX_DECKS);
  uint8_t bestTier = 4;
  float bestRank = 0.0f;
  uint16_t waiting = 0;
  CardRecord record{};
  uint16_t index = 0;
  for (uint8_t d = 0; d < deckCount; ++d) {
    const DeckOwner owner = deckOwner(scan.decks[d].id, key, buffer);
    if (owner == DeckOwner::Other || (owner == DeckOwner::Nobody && !article)) continue;
    const uint8_t nobody = owner == DeckOwner::Nobody ? 1 : 0;
    decks::RecordCursor cursor(scan.decks[d].id, scan.chunk, READ_CHUNK);
    while (cursor.next(record, index)) {
      if (record.reps > 0 && record.lastDay == g.day) continue;  // answered today
      uint8_t tier = 0;
      float rank = 0.0f;
      if (record.reps > 0 && record.dueDay <= g.day) {
        tier = nobody;
        rank = fsrs::recordRetrievability(record, g.day);
      } else if (record.reps == 0 && freshAllowed) {
        tier = static_cast<uint8_t>(2 + nobody);
      } else {
        continue;
      }
      ++waiting;
      if (tier > bestTier || (tier == bestTier && rank >= bestRank)) continue;
      bestTier = tier;
      bestRank = rank;
      out = StepLine{};
      out.kind = static_cast<uint8_t>(EntryKind::Card);
      out.sub = record.type;
      out.record = index;
      out.offset = record.offset;
      std::memcpy(out.deck, scan.decks[d].id, sizeof(scan.decks[d].id));
    }
  }
  return waiting;
}

}  // namespace

bool loadQuestion(const char* bookPath, LineBuffer& buffer, Item& item, Step& step, uint16_t& left) {
  left = 0;
  auto scan = makeUniqueNoThrow<QuestionScan>();
  if (!scan) {
    LOG_ERR("LEARN", "OOM: questions (%u bytes)", static_cast<unsigned>(sizeof(QuestionScan)));
    return false;
  }
  StepLine entry{};
  const uint16_t waiting = findQuestion(bookPath, buffer, *scan, entry);
  if (waiting == 0 || !loadEntry(buffer, entry, item)) return false;
  step.entry = entry;
  left = waiting;
  return true;
}

bool hasQuestion(const char* bookPath) {
  auto buffer = makeUniqueNoThrow<LineBuffer>();
  auto scan = makeUniqueNoThrow<QuestionScan>();
  if (!buffer || !scan) {
    LOG_ERR("LEARN", "OOM: question check (%u bytes)", static_cast<unsigned>(sizeof(LineBuffer) + sizeof(QuestionScan)));
    return false;
  }
  StepLine entry{};
  return findQuestion(bookPath, *buffer, *scan, entry) > 0;
}

void answerQuestion(const Step& step, const Item& item, const Answer& answer, const uint32_t ms) {
  if (!recordAnswer(step, item, answer, ms)) return;
  g.totalsDay = 0;  // counted again before stats.json is written
  statsStale = true;
  saveState();
}

void markSeen(const Item& item) {
  addSeen(itemIdHash(item.id));
  writeSeen(item.id, g.day, wallClock());
}

void markSolved(const Item& item, const bool correct, const int8_t pick, const uint32_t ms) {
  addSeen(itemIdHash(item.id));
  writeSeenResult(item.id, correct, pick, ms, g.day, wallClock());
}

// ---- the bite and the book puzzles ------------------------------------------------

namespace {

// Unseen candidates looked at per call: a highlight whose text is gone is passed over.
constexpr uint8_t UNSEEN_CANDIDATES = 8;

struct Candidate {
  StepLine entry{};
  uint64_t highlight = 0;  // a highlight item without its text: the highlight's id hash
};

uint8_t findUnseen(LineBuffer& buffer, bool (*wanted)(Shape), Candidate (&out)[UNSEEN_CANDIDATES]) {
  auto seen = makeUniqueNoThrow<uint64_t[]>(SEEN_MAX);
  if (!seen) return 0;
  const uint16_t seenCount = loadSeen(seen.get());
  HalFile file;
  if (!Storage.exists(paths::STREAM) || !Storage.openFileForRead("LEARN", paths::STREAM, file)) return 0;
  JsonlReader reader(file, buffer);
  size_t length = 0;
  uint32_t offset = 0;
  JsonlReader::Line line;
  uint16_t scanned = 0;
  uint8_t count = 0;
  Item item;
  while (count < UNSEEN_CANDIDATES && scanned < STREAM_SCAN_MAX &&
         (line = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (line != JsonlReader::Line::Ok) continue;
    ++scanned;
    if (!parseStreamItem(buffer.text, length, item) || !wanted(item.shape)) continue;
    const uint64_t hash = itemIdHash(item.id);
    if (std::find(seen.get(), seen.get() + seenCount, hash) != seen.get() + seenCount) continue;
    Candidate& c = out[count++];
    c.entry.kind = static_cast<uint8_t>(EntryKind::Item);
    c.entry.offset = offset;
    if (item.shape == Shape::Highlight && item.body.length == 0) c.highlight = itemIdHash(item.highlightId);
  }
  file.close();
  return count;
}

// Where each candidate's highlight is in highlights.jsonl; the last line with an id wins.
void resolveHighlights(LineBuffer& buffer, Candidate* candidates, const uint8_t count) {
  bool any = false;
  for (uint8_t i = 0; i < count; ++i) any = any || candidates[i].highlight != 0;
  HalFile file;
  if (!any || !Storage.exists(paths::HIGHLIGHTS) || !Storage.openFileForRead("LEARN", paths::HIGHLIGHTS, file)) return;
  JsonlReader reader(file, buffer);
  size_t length = 0;
  uint32_t offset = 0;
  JsonlReader::Line line;
  char id[ID_MAX + 1];
  while ((line = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (line != JsonlReader::Line::Ok || !scanId(buffer.text, length, id)) continue;
    const uint64_t hash = itemIdHash(id);
    for (uint8_t i = 0; i < count; ++i) {
      if (candidates[i].highlight == hash) candidates[i].entry.highlightAt = offset + 1;
    }
  }
  file.close();
}

}  // namespace

bool loadUnseen(LineBuffer& buffer, bool (*wanted)(Shape), Item& item, Step& step) {
  loadState();
  Candidate candidates[UNSEEN_CANDIDATES];
  const uint8_t count = findUnseen(buffer, wanted, candidates);
  resolveHighlights(buffer, candidates, count);
  for (uint8_t i = 0; i < count; ++i) {
    if (candidates[i].highlight != 0 && candidates[i].entry.highlightAt == 0) continue;
    if (!loadEntry(buffer, candidates[i].entry, item)) continue;
    step = Step{};
    step.entry = candidates[i].entry;
    return true;
  }
  return false;
}

bool hasUnseen(bool (*wanted)(Shape)) {
  auto buffer = makeUniqueNoThrow<LineBuffer>();
  if (!buffer) return false;
  Item item;
  Step step;
  return loadUnseen(*buffer, wanted, item, step);
}

uint32_t today() {
  loadState();
  const int64_t now = wallClock();
  if (now > 0 && civilDay(now, g.tzMinutes) != static_cast<int64_t>(g.day)) noteClock(now, g.tzMinutes);
  return g.day;
}

bool biteDue() {
  HalFile file;
  if (!Storage.exists(paths::BITE_DAY) || !Storage.openFileForRead("LEARN", paths::BITE_DAY, file)) return true;
  uint32_t shown = 0;
  const bool read = file.read(&shown, sizeof(shown)) == static_cast<int>(sizeof(shown));
  file.close();
  if (!read) return true;
  const uint32_t day = today();
  return day != 0 && day != shown;
}

void noteBite() {
  const uint32_t day = today();
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  if (!statefile::replace(paths::BITE_TMP, paths::BITE_DAY, {{&day, sizeof(day)}})) {
    LOG_ERR("LEARN", "Cannot note the bite");
  }
}

// ---- predict: the guesses of the paper's pieces --------------------------------

namespace {

constexpr uint32_t GUESS_MAGIC = 0x54433458;  // "X4CT": a table; the single guess before it is dropped
constexpr uint8_t GUESSES_MAX = 48;
constexpr uint8_t GUESS_PIECE = 1;
constexpr uint8_t GUESS_DONE = 2;
constexpr uint8_t GUESS_FORECAST = 4;

// A guess that waits for its piece's article. The newest is last.
struct GuessEntry {
  uint32_t day;  // when it was saved
  uint32_t ms;
  uint64_t item;  // itemIdHash of the article's id
  uint64_t book;  // itemIdHash of its book key
  uint32_t linkMs;
  int8_t pick;
  int8_t link;
  uint8_t read;
  uint8_t flags;
};
static_assert(sizeof(GuessEntry) == 32, "GuessEntry is an on-card format");

struct GuessTable {
  uint32_t magic;
  uint32_t count;
  GuessEntry entries[GUESSES_MAX];
};

std::unique_ptr<GuessTable> loadGuesses() {
  auto table = makeUniqueNoThrow<GuessTable>();
  if (!table) {
    LOG_ERR("LEARN", "OOM: guesses (%u bytes)", static_cast<unsigned>(sizeof(GuessTable)));
    return table;
  }
  table->magic = GUESS_MAGIC;
  table->count = 0;
  statefile::recover(paths::GUESS_TMP, paths::GUESS);
  HalFile file;
  if (!Storage.exists(paths::GUESS) || !Storage.openFileForRead("LEARN", paths::GUESS, file)) return table;
  uint32_t head[2] = {0, 0};
  if (file.read(head, sizeof(head)) == static_cast<int>(sizeof(head)) && head[0] == GUESS_MAGIC) {
    const uint32_t count = std::min<uint32_t>(head[1], GUESSES_MAX);
    const int got = file.read(table->entries, count * sizeof(GuessEntry));
    table->count = got > 0 ? static_cast<uint32_t>(static_cast<size_t>(got) / sizeof(GuessEntry)) : 0;
  }
  file.close();
  return table;
}

void storeGuesses(const GuessTable& table) {
  Storage.mkdir(paths::STATE);
  if (!statefile::replace(paths::GUESS_TMP, paths::GUESS,
                          {{&table, 2 * sizeof(uint32_t) + table.count * sizeof(GuessEntry)}})) {
    LOG_ERR("LEARN", "Cannot save the guesses");
  }
}

// A piece's guess holds as long as the piece is kept. One without the flag was a daily set's.
bool holds(const GuessEntry& entry) { return (entry.flags & GUESS_PIECE) != 0; }

int findGuess(const GuessTable& table, const uint64_t item) {
  for (uint32_t i = 0; i < table.count; ++i) {
    if (table.entries[i].item == item && holds(table.entries[i])) return static_cast<int>(i);
  }
  return -1;
}

void dropGuess(GuessTable& table, const uint32_t at) {
  std::memmove(&table.entries[at], &table.entries[at + 1], (table.count - at - 1) * sizeof(GuessEntry));
  --table.count;
}

// A page turned in the article, or its end reached: its answer is earned.
void noteArticleRead(const char* bookPath) {
  const auto table = loadGuesses();
  if (!table) return;
  const uint64_t book = itemIdHash(keyOf(bookPath));
  bool changed = false;
  for (uint32_t i = 0; i < table->count; ++i) {
    GuessEntry& entry = table->entries[i];
    if (entry.book != book || entry.read || !holds(entry)) continue;
    entry.read = 1;
    changed = true;
  }
  if (changed) storeGuesses(*table);
}

}  // namespace

Guess guessFor(const Item& article) {
  loadState();
  Guess out;
  const auto table = loadGuesses();
  const int at = table ? findGuess(*table, itemIdHash(article.id)) : -1;
  if (at < 0) return out;
  const GuessEntry& entry = table->entries[at];
  out.pick = entry.pick;
  out.read = entry.read != 0;
  out.link = entry.link;
  out.ms = entry.ms;
  out.linkMs = entry.linkMs;
  out.piece = (entry.flags & GUESS_PIECE) != 0;
  out.done = (entry.flags & GUESS_DONE) != 0;
  out.forecast = (entry.flags & GUESS_FORECAST) != 0;
  return out;
}

void saveGuess(const Item& article, const char* book, const Guess& guess) {
  loadState();
  const auto table = loadGuesses();
  if (!table) return;
  const uint64_t item = itemIdHash(article.id);
  int at = findGuess(*table, item);
  // What the reader did in the article meanwhile is on the card, not in `guess`.
  const bool read = guess.read || (at >= 0 && table->entries[at].read);
  if (at < 0) {
    // Out of room: a piece that is done goes first, else the oldest.
    if (table->count >= GUESSES_MAX) {
      uint32_t out = 0;
      for (uint32_t i = 0; i < table->count; ++i) {
        if (table->entries[i].flags & GUESS_DONE) {
          out = i;
          break;
        }
      }
      dropGuess(*table, out);
    }
    at = static_cast<int>(table->count++);
  }
  uint8_t flags = 0;
  if (guess.piece) flags |= GUESS_PIECE;
  if (guess.done) flags |= GUESS_DONE;
  if (guess.forecast) flags |= GUESS_FORECAST;
  table->entries[at] = GuessEntry{g.day,      guess.ms,   item,                            itemIdHash(keyOf(book)),
                                  guess.linkMs, guess.pick, guess.link, read ? uint8_t{1} : uint8_t{0}, flags};
  storeGuesses(*table);
}

void noteArticleFinished(const char* bookPath) {
  loadState();
  noteArticleRead(bookPath);
}

namespace {

// A highlight for the sleep screen, a different one each day.
bool pickSleepHighlight(LineBuffer& buffer, Item& item) {
  HalFile file;
  if (!Storage.exists(paths::HIGHLIGHTS) || !Storage.openFileForRead("LEARN", paths::HIGHLIGHTS, file)) return false;
  JsonlReader reader(file, buffer);
  size_t length = 0;
  uint32_t offset = 0;
  uint32_t chosen = 0;
  uint16_t lines = 0;
  JsonlReader::Line line;
  // Two passes keep RAM flat: count, then fetch the chosen line.
  while (lines < SLEEP_HIGHLIGHT_SCAN && (line = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (line == JsonlReader::Line::Ok && !isHighlightDeletion(buffer.text, length)) ++lines;
  }
  if (lines == 0) {
    file.close();
    return false;
  }
  const uint16_t target = static_cast<uint16_t>((g.day * 7919u) % lines);
  reader.seek(0);
  uint16_t at = 0;
  while ((line = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (line != JsonlReader::Line::Ok || isHighlightDeletion(buffer.text, length)) continue;
    if (at++ == target) {
      chosen = offset;
      break;
    }
  }
  file.close();
  item = Item{};
  return readLineAt(paths::HIGHLIGHTS, chosen, buffer, length) && parseHighlightInto(buffer.text, length, item);
}

}  // namespace

bool pickSleepItem(LineBuffer& buffer, Item& item) {
  ensureReady();
  // The due card most likely forgotten. A new card, or the weakest card not due yet, is the
  // last resort, after a highlight.
  StepLine spare{};
  bool haveSpare = false;
  {
    auto w = makeUniqueNoThrow<ScanWork>();
    if (!w) {
      LOG_ERR("LEARN", "OOM: sleep card (%u bytes)", static_cast<unsigned>(sizeof(ScanWork)));
      return false;
    }
    const uint8_t deckCount = decks::list(w->decks, decks::MAX_DECKS);
    StepLine due{};
    float dueRank = 2.0f;  // retrievability is at most 1
    float aheadRank = 2.0f;
    StepLine ahead{};
    CardRecord record{};
    uint16_t index = 0;
    for (uint8_t d = 0; d < deckCount; ++d) {
      decks::RecordCursor cursor(w->decks[d].id, w->chunk, sizeof(w->chunk));
      while (cursor.next(record, index)) {
        if (record.lastDay == g.day && record.reps != 0) continue;
        StepLine entry{};
        entry.kind = static_cast<uint8_t>(EntryKind::Card);
        entry.sub = record.type;
        entry.record = index;
        entry.offset = record.offset;
        std::memcpy(entry.deck, w->decks[d].id, sizeof(w->decks[d].id));
        if (record.reps == 0) {
          if (!haveSpare) {
            spare = entry;
            haveSpare = true;
          }
          continue;
        }
        const float rank = fsrs::recordRetrievability(record, g.day);
        if (record.dueDay <= g.day && rank < dueRank) {
          due = entry;
          dueRank = rank;
        } else if (record.dueDay > g.day && rank < aheadRank) {
          ahead = entry;
          aheadRank = rank;
        }
      }
    }
    if (dueRank <= 1.0f && loadEntry(buffer, due, item)) return true;
    if (!haveSpare && aheadRank <= 1.0f) {
      spare = ahead;
      haveSpare = true;
    }
  }

  if (pickSleepHighlight(buffer, item)) return true;
  return haveSpare && loadEntry(buffer, spare, item);
}

void addReading(const char* bookPath, const uint32_t ms, const uint32_t pages) {
  if (ms == 0 && pages == 0) return;
  loadState();
  openFirstDay(g);  // a book read before the first power-on's day still reads on day 1
  // Without a clock, reading makes the days (D1).
  if (wallClock() <= 0) {
    g.countMs = g.countMs > UINT32_MAX - ms ? UINT32_MAX : g.countMs + ms;
    if (g.countMs >= DAY_READ_MS) {
      const uint32_t days = g.countMs / DAY_READ_MS;
      g.countMs %= DAY_READ_MS;
      startDay(g, g.day + days);
    }
  }
  saveState();
  const char* key = *bookPath == '/' ? bookPath + 1 : bookPath;
  books::add(key, ms, pages, g.day, wallClock());
  statsStale = true;
  if (pages > 0) noteArticleRead(bookPath);
}

bool appendHighlight(const HighlightInput& input, char (&id)[16]) {
  loadState();
  return writeHighlight(input, g.day, wallClock(), id);
}

bool appendLookup(const char* word, const char* bookPath, const char* title) {
  if (!word || !*word || !bookPath) return false;
  loadState();
  return writeLookup(word, *bookPath == '/' ? bookPath + 1 : bookPath, title, g.day, wallClock());
}

bool forgetHighlight(const char* id) {
  loadState();
  return writeHighlightDeletion(id, g.day, wallClock());
}

void onHostTime(const int64_t now, const int32_t tzMinutes) {
  loadState();
  if (now <= 0) return;
  hostClock = true;
  noteClock(now, tzMinutes);
}

void onHostDataChanged() {
  hostDataChanged = true;
  if (Storage.exists(paths::HOST_CHANGED)) return;
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  HalFile marker = Storage.open(paths::HOST_CHANGED, O_RDWR | O_CREAT);
  if (!marker || !marker.close()) LOG_ERR("LEARN", "Cannot write the host-change marker");
}

void helloFields(JsonObject out) {
  loadState();
  out["day"] = g.day;
  // Recall over reviewed cards, 0 to 1; left out until a card is reviewed. Today no longer shows it.
  if (g.retention >= 0.0f) out["retention"] = std::round(g.retention * 100.0f) / 100.0f;
}

void refreshStats() { statsStale = true; }

void flushStats() {
  if (!statsStale) return;
  loadState();
  writeStatsNow();
}

int64_t wallClock() {
  int32_t tz = 0;
  return hostClock || platformHostClock(tz) ? platformNow() : 0;
}

bool localTime(CivilTime& out) {
  const int64_t now = wallClock();
  if (now <= 0) return false;
  loadState();
  out = civilTime(now, g.tzMinutes);
  return true;
}

#ifdef X4_LEARN_HOST_TEST
void forgetPowerOn() {
  g = LearnState{};
  loaded = false;
  bootChecked = false;
  hostDataChanged = false;
  hostClock = false;
  stateSeq = 0;
}
#endif

}  // namespace x4::learn
