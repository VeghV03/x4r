#include "Play.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "PlayPaths.h"
#include "Rng.h"
#include "learn/Journal.h"

namespace x4::play {

namespace {

// ---- play.bin: one 32-byte record per game, updated in place ----------------

constexpr uint8_t RECORD_VERSION = 1;

struct Record {
  char id[GAME_ID_MAX + 1];
  uint8_t lang;  // Lang; before languages this byte ended the id and was always 0 (En)
  uint8_t level;
  uint8_t version;
  uint16_t check;  // over every other byte
  uint32_t played;
  uint32_t won;
  uint32_t dailyDay;  // a set number before the day keyed dailies: never a day, so open
  uint32_t practice;
};
static_assert(sizeof(Record) == 32, "Record is an on-card format");

uint32_t fnv32(const void* data, const size_t length, uint32_t hash = 2166136261u) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

uint16_t checkOf(const Record& r) {
  Record copy = r;
  copy.check = 0;
  const uint32_t h = fnv32(&copy, sizeof(copy));
  return static_cast<uint16_t>(h ^ (h >> 16));
}

bool sameId(const Record& r, const char* game) { return std::strncmp(r.id, game, GAME_ID_MAX + 1) == 0; }

// Finds `game` in play.bin. `slot` gets its index, or the first free one.
bool findRecord(const char* game, Record& out, uint32_t& slot) {
  slot = 0;
  HalFile file;
  if (!Storage.exists(paths::PLAY_STATE) || !Storage.openFileForRead("PLAY", paths::PLAY_STATE, file)) return false;
  Record r{};
  bool found = false;
  while (file.read(&r, sizeof(r)) == static_cast<int>(sizeof(r))) {
    if (sameId(r, game)) {
      found = r.version == RECORD_VERSION && r.check == checkOf(r);
      if (found) out = r;
      break;  // a damaged record is rewritten in its own slot
    }
    ++slot;
  }
  file.close();
  return found;
}

Record load(const char* game, uint32_t& slot) {
  Record r{};
  if (findRecord(game, r, slot)) return r;
  r = Record{};
  std::strncpy(r.id, game, GAME_ID_MAX);
  r.lang = static_cast<uint8_t>(DEFAULT_LANG);
  r.level = LEVEL_START;
  return r;
}

bool store(Record& r, const uint32_t slot) {
  if (slot >= GAMES_MAX) {
    LOG_ERR("PLAY", "No room for game %s", r.id);
    return false;
  }
  r.version = RECORD_VERSION;
  r.check = checkOf(r);
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  HalFile file = Storage.open(paths::PLAY_STATE, O_RDWR | O_CREAT);
  bool ok = static_cast<bool>(file);
  const size_t at = slot * sizeof(Record);
  // Never leave a hole: a slot is either the next one or an existing one.
  ok = ok && file.fileSize() >= at && file.seekSet(at) && file.write(&r, sizeof(r)) == sizeof(r);
  if (file) ok = file.close() && ok;
  if (!ok) LOG_ERR("PLAY", "Cannot save play state for %s", r.id);
  return ok;
}

uint32_t daySalt(const uint32_t day) { return mix32(day * 0x9E3779B9u + 0x632BE5ABu); }

uint32_t gameHash(const char* game) { return fnv32(game, std::strlen(game)); }

void savedPath(const char* game, char (&out)[48]) { snprintf(out, sizeof(out), "%s/play-%s.bin", paths::STATE, game); }

// ---- play-<game>.bin: the game in progress -------------------------------------

constexpr uint32_t SAVED_MAGIC = 0x50473458;  // "X4GP"
constexpr uint16_t SAVED_VERSION = 1;

struct SavedHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t mode;
  uint8_t daily;
  uint8_t level;
  uint8_t reserved;
  uint32_t day;  // a set number in saves from before the day keyed dailies
  uint32_t ms;
  uint32_t crc;  // over the header up to here, then the state
};
static_assert(sizeof(SavedHeader) == 24, "SavedHeader is an on-card format");

uint32_t savedCrc(const SavedHeader& h, const uint8_t* state) {
  return fnv32(state, h.size, fnv32(&h, offsetof(SavedHeader, crc)));
}

void savedTmpPath(const char* game, char (&out)[52]) {
  snprintf(out, sizeof(out), "%s/play-%s.bin.tmp", paths::STATE, game);
}

bool readSaved(const char* path, SavedHeader& h, uint8_t* state) {
  HalFile file;
  if (!Storage.exists(path) || !Storage.openFileForRead("PLAY", path, file)) return false;
  const bool ok = file.read(&h, sizeof(h)) == static_cast<int>(sizeof(h)) && h.magic == SAVED_MAGIC &&
                  h.version == SAVED_VERSION && h.size <= SAVE_MAX &&
                  file.read(state, h.size) == static_cast<int>(h.size) && h.crc == savedCrc(h, state) &&
                  h.mode <= static_cast<uint8_t>(Mode::Set);
  file.close();
  return ok;
}

}  // namespace

const char* langCode(const Lang lang) { return lang == Lang::Hu ? "hu" : "en"; }

const char* modeName(const Mode mode) {
  switch (mode) {
    case Mode::Daily:
      return "daily";
    case Mode::Set:
      return "set";
    case Mode::Practice:
      break;
  }
  return "practice";
}

Standing toStanding(const Record& r) {
  Standing s;
  s.level = r.level < LEVEL_MIN || r.level > LEVEL_MAX ? LEVEL_START : r.level;
  s.played = r.played;
  s.won = r.won;
  s.dailyDay = r.dailyDay;
  s.practice = r.practice;
  s.lang = r.lang < LANGS ? static_cast<Lang>(r.lang) : DEFAULT_LANG;
  return s;
}

Standing standing(const char* game) {
  uint32_t slot = 0;
  return toStanding(load(game, slot));
}

void standings(const char* const* games, const uint8_t count, Standing* out) {
  for (uint8_t i = 0; i < count; ++i) out[i] = Standing{};
  HalFile file;
  if (!Storage.exists(paths::PLAY_STATE) || !Storage.openFileForRead("PLAY", paths::PLAY_STATE, file)) return;
  bool seen[GAMES_MAX] = {};
  Record r{};
  while (file.read(&r, sizeof(r)) == static_cast<int>(sizeof(r))) {
    for (uint8_t i = 0; i < count && i < GAMES_MAX; ++i) {
      // The first record with the id counts, as in findRecord(); a damaged one reads as new.
      if (seen[i] || !sameId(r, games[i])) continue;
      seen[i] = true;
      if (r.version == RECORD_VERSION && r.check == checkOf(r)) out[i] = toStanding(r);
    }
  }
  file.close();
}

void setLang(const char* game, const Lang lang) {
  uint32_t slot = 0;
  Record r = load(game, slot);
  if (r.lang == static_cast<uint8_t>(lang)) return;
  r.lang = static_cast<uint8_t>(lang);
  store(r, slot);
}

uint32_t dailyDay(const uint32_t today) {
  if (today != 0) return today;
  uint32_t last = 0;
  HalFile file;
  if (!Storage.exists(paths::PLAY_STATE) || !Storage.openFileForRead("PLAY", paths::PLAY_STATE, file)) return 0;
  Record r{};
  while (file.read(&r, sizeof(r)) == static_cast<int>(sizeof(r))) {
    if (r.version == RECORD_VERSION && r.check == checkOf(r)) last = std::max(last, r.dailyDay);
  }
  file.close();
  return last;
}

bool dailyDone(const char* game, const uint32_t day) { return day != 0 && standing(game).dailyDay == day; }

uint32_t dailySeed(const char* game, const uint32_t day) { return mix32(gameHash(game) ^ daySalt(day)); }

uint32_t nextPracticeSeed(const char* game, const uint32_t day) {
  uint32_t slot = 0;
  Record r = load(game, slot);
  const uint32_t n = ++r.practice;
  store(r, slot);
  return mix32(gameHash(game) ^ daySalt(day) ^ mix32(0x7F4A7C15u * n));
}

uint8_t adapt(const uint8_t level, const bool won) {
  if (won) return level < LEVEL_MAX ? level + 1 : LEVEL_MAX;
  return level > LEVEL_MIN ? level - 1 : LEVEL_MIN;
}

uint8_t finish(const Result& result, const uint32_t day, const int64_t at) {
  uint32_t slot = 0;
  Record r = load(result.game, slot);
  r.level = adapt(result.level, result.won);
  if (result.mode != Mode::Practice) {
    ++r.played;
    if (result.won) ++r.won;
  }
  if (result.daily) r.dailyDay = day;
  store(r, slot);

  JsonDocument doc;
  doc["game"] = result.game;
  if (result.lang) doc["lang"] = result.lang;
  doc["mode"] = modeName(result.mode);
  doc["level"] = result.level;
  doc["won"] = result.won;
  if (result.score >= 0) doc["score"] = result.score;
  if (result.tries >= 0) doc["tries"] = result.tries;
  doc["ms"] = result.ms;
  doc["day"] = day;
  if (at > 0) {
    doc["at"] = at;
  } else {
    doc["at"] = nullptr;
  }
  appendJson(paths::PLAY_LOG, doc);
  return r.level;
}

void addStats(JsonObject play) {
  HalFile file;
  if (!Storage.exists(paths::PLAY_STATE) || !Storage.openFileForRead("PLAY", paths::PLAY_STATE, file)) return;
  Record r{};
  while (file.read(&r, sizeof(r)) == static_cast<int>(sizeof(r))) {
    if (r.version != RECORD_VERSION || r.check != checkOf(r)) continue;
    r.id[GAME_ID_MAX] = '\0';
    JsonObject entry = play[r.id].to<JsonObject>();
    entry["level"] = r.level;
    entry["played"] = r.played;
    entry["won"] = r.won;
  }
  file.close();
}

bool saveGame(const char* game, const Saved& saved, const uint8_t* state) {
  if (saved.size > SAVE_MAX) return false;
  SavedHeader h{SAVED_MAGIC,
                SAVED_VERSION,
                saved.size,
                static_cast<uint8_t>(saved.mode),
                static_cast<uint8_t>(saved.daily ? 1 : 0),
                saved.level,
                0,
                saved.day,
                saved.ms,
                0};
  h.crc = savedCrc(h, state);
  char path[48];
  savedPath(game, path);
  char tmp[52];
  savedTmpPath(game, tmp);
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  HalFile file;
  if (!Storage.openFileForWrite("PLAY", tmp, file)) return false;
  const bool written = file.write(&h, sizeof(h)) == sizeof(h) && file.write(state, saved.size) == saved.size;
  if (!file.close() || !written) {
    Storage.remove(tmp);
    LOG_ERR("PLAY", "Cannot save %s", game);
    return false;
  }
  // A complete .tmp that could not replace the save stays: loadGame reads it.
  if (!Storage.replaceFile(tmp, path)) {
    LOG_ERR("PLAY", "Cannot replace the save of %s", game);
    return false;
  }
  return true;
}

bool loadGame(const char* game, Saved& saved, uint8_t* state) {
  char path[48];
  savedPath(game, path);
  char tmp[52];
  savedTmpPath(game, tmp);
  SavedHeader h{};
  // A complete .tmp is always the newer save: replaceFile() removes the old one
  // before the rename, and a power cut can fall in between.
  if (!readSaved(tmp, h, state) && !readSaved(path, h, state)) return false;
  saved.mode = static_cast<Mode>(h.mode);
  saved.daily = h.daily != 0;
  saved.level = h.level < LEVEL_MIN || h.level > LEVEL_MAX ? LEVEL_START : h.level;
  saved.day = h.day;
  saved.ms = h.ms;
  saved.size = h.size;
  return true;
}

void clearGame(const char* game) {
  char path[48];
  savedPath(game, path);
  char tmp[52];
  savedTmpPath(game, tmp);
  // The .tmp first: left alone, it would bring the game back.
  if (Storage.exists(tmp)) Storage.remove(tmp);
  if (Storage.exists(path)) Storage.remove(path);
}

}  // namespace x4::play
