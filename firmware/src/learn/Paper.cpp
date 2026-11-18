#include "Paper.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Journal.h"
#include "Learn.h"
#include "LearnPaths.h"
#include "StateFile.h"

namespace x4::paper {

namespace {

const char* text(JsonVariantConst v) {
  const char* s = v.as<const char*>();
  return s ? s : "";
}

bool parseHeader(const char* line, const size_t length, Front& out) {
  JsonDocument doc;
  if (deserializeJson(doc, line, length) || !doc.is<JsonObject>()) return false;
  const char* date = doc["date"].as<const char*>();
  const JsonArrayConst sections = doc["sections"].as<JsonArrayConst>();
  if (!date || std::strlen(date) != 10 || sections.isNull()) return false;
  out = Front{};
  std::memcpy(out.date, date, 11);
  for (const JsonVariantConst s : sections) {
    if (out.count >= SECTIONS_MAX) break;
    const char* id = text(s["id"]);
    const char* name = text(s["name"]);
    if (!*id || std::strlen(id) > SECTION_ID_BYTES || !*name) continue;
    Section& section = out.sections[out.count++];
    std::memcpy(section.id, id, std::strlen(id) + 1);
    readlist::copyCut(section.name, name, SECTION_NAME_BYTES);
  }
  return true;
}

struct Line {
  const char* section;
  const char* head;
  const char* lead;
  const char* source;
  const char* id;
};

// A piece's line: the Read list's entry and the paper's fields. `doc` owns the strings.
bool parsePiece(const char* line, const size_t length, JsonDocument& doc, readlist::Entry& entry, Line& out) {
  if (!readlist::parseEntry(line, length, entry)) return false;
  if (deserializeJson(doc, line, length)) return false;
  out = Line{text(doc["section"]), text(doc["head"]), text(doc["lead"]), text(doc["source"]), text(doc["id"])};
  return true;
}

bool onCard(const readlist::Entry& entry) {
  char path[readlist::BOOK_BYTES + 2];
  snprintf(path, sizeof(path), "/%s", entry.book);
  return Storage.exists(path);
}

// Calls `each(entry, line)` for every piece whose article is on the card. `front` gets the
// header when the file has one; false when it has none.
template <typename Each>
bool scan(LineBuffer& buffer, Front* front, Each each) {
  HalFile file;
  if (!Storage.exists(paths::READ_LIST) || !Storage.openFileForRead("PAPER", paths::READ_LIST, file)) return false;
  JsonlReader reader(file, buffer);
  size_t length = 0;
  uint32_t offset = 0;
  JsonlReader::Line got;
  bool first = true;
  bool header = false;
  readlist::Entry entry;
  Line line{};
  while ((got = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (got != JsonlReader::Line::Ok) continue;
    if (first) {
      first = false;
      Front parsed;
      if (parseHeader(buffer.text, length, parsed)) {
        header = true;
        if (front) *front = parsed;
        continue;
      }
      break;  // no header: not an issue
    }
    JsonDocument doc;
    if (!parsePiece(buffer.text, length, doc, entry, line)) continue;
    if (!onCard(entry)) continue;
    each(entry, line);
  }
  file.close();
  return header;
}

}  // namespace

int Front::find(const char* sectionId) const {
  for (uint8_t i = 0; i < count; ++i) {
    if (std::strcmp(sections[i].id, sectionId) == 0) return i;
  }
  return -1;
}

uint8_t loadCompetitions(char (*out)[COMPETITION_BYTES + 1], const uint8_t max) {
  constexpr size_t CAP = 32 * 1024;  // as net::loadFootball reads it
  HalFile file;
  if (!Storage.exists(paths::FOOTBALL_JSON) || !Storage.openFileForRead("PAPER", paths::FOOTBALL_JSON, file)) return 0;
  std::string body;
  char chunk[512];
  int got = 0;
  while (body.size() < CAP && (got = file.read(chunk, sizeof(chunk))) > 0) body.append(chunk, static_cast<size_t>(got));
  file.close();
  JsonDocument filter;
  filter["tables"][0]["comp"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return 0;
  uint8_t n = 0;
  for (const JsonVariantConst t : doc["tables"].as<JsonArrayConst>()) {
    const char* comp = text(t["comp"]);
    if (n >= max) break;
    if (!*comp) continue;
    readlist::copyCut(out[n++], comp, COMPETITION_BYTES);
  }
  return n;
}

bool loadFront(LineBuffer& buffer, Front& out) {
  const readlist::Done done = readlist::loadFinished();
  out = Front{};
  return scan(buffer, &out, [&](const readlist::Entry& entry, const Line& line) {
    const int at = out.find(line.section);
    if (at < 0) return;
    Section& section = out.sections[at];
    if (section.pieces >= PIECES_MAX) return;
    ++section.pieces;
    if (section.head[0] || done.has(entry.book)) return;
    readlist::copyCut(section.head, *line.head ? line.head : entry.title, HEAD_BYTES);
  });
}

static uint8_t loadSectionPieces(LineBuffer& buffer, const char* sectionId, Piece* out, const uint8_t max) {
  const readlist::Done done = readlist::loadFinished();
  uint8_t count = 0;
  scan(buffer, nullptr, [&](const readlist::Entry& entry, const Line& line) {
    if (count >= max || std::strcmp(line.section, sectionId) != 0) return;
    Piece& piece = out[count++];
    piece = Piece{};
    readlist::copyCut(piece.id, line.id, ID_MAX);
    readlist::copyCut(piece.source, line.source, SOURCE_BYTES);
    std::memcpy(piece.title, entry.title, sizeof(piece.title));
    readlist::copyCut(piece.lead, line.lead, LEAD_BYTES);
    std::memcpy(piece.book, entry.book, sizeof(piece.book));
    piece.minutes = entry.minutes;
    piece.read = done.has(entry.book);
  });
  return count;
}

namespace {

bool isFootball(const Front& front, const uint8_t section) {
  return section < front.count && std::strcmp(front.sections[section].id, FOOTBALL) == 0;
}

Go openSection(Place& place, const uint8_t section) {
  place.screen = Screen::Section;
  place.section = section;
  place.row = section;
  place.piece = 0;
  return Go::Section;
}

Go pressFront(Place& place, const Front& front, const Key key) {
  const uint8_t count = rows(front);
  switch (key) {
    case Key::Up:
      place.row = place.row == 0 ? count - 1 : place.row - 1;
      return Go::Move;
    case Key::Down:
      place.row = place.row + 1 >= count ? 0 : place.row + 1;
      return Go::Move;
    case Key::Confirm:
      if (place.row < front.count) return openSection(place, place.row);
      switch (static_cast<Fixed>(place.row - front.count)) {
        case Fixed::Play:
          return Go::Play;
        case Fixed::Book:
          return Go::Book;
        default:
          return Go::Library;
      }
    default:
      return Go::Stay;
  }
}

Go pressSection(Place& place, const Front& front, const uint8_t pieces, const Key key) {
  switch (key) {
    case Key::Up:
      if (pieces < 2) return Go::Stay;
      place.piece = place.piece == 0 ? pieces - 1 : place.piece - 1;
      return Go::Move;
    case Key::Down:
      if (pieces < 2) return Go::Stay;
      place.piece = place.piece + 1 >= pieces ? 0 : place.piece + 1;
      return Go::Move;
    case Key::Left:
      if (place.section == 0) return Go::Stay;
      if (isFootball(front, place.section - 1)) {
        // The tables come before this section: arrive on the last of them.
        place.section -= 1;
        place.row = place.section;
        place.fromRight = true;
        return Go::Football;
      }
      return openSection(place, place.section - 1);
    case Key::Right:
      if (isFootball(front, place.section)) {
        place.fromRight = false;
        return Go::Football;
      }
      return place.section + 1 >= front.count ? Go::Stay : openSection(place, place.section + 1);
    case Key::Confirm:
      return place.piece < pieces ? Go::Piece : Go::Stay;
    case Key::Back:
      place.screen = Screen::Front;
      place.row = place.section;
      return Go::Front;
  }
  return Go::Stay;
}

}  // namespace

Go leaveFootball(Place& place, const Front& front, const bool right) {
  if (!right) return openSection(place, place.section);
  return place.section + 1 >= front.count ? Go::Stay : openSection(place, place.section + 1);
}

Go press(Place& place, const Front& front, const uint8_t pieces, const Key key) {
  return place.screen == Screen::Front ? pressFront(place, front, key) : pressSection(place, front, pieces, key);
}

}  // namespace x4::paper

// --- Kept pieces ---

namespace x4::paper {

namespace {

// A line of the kept state: the piece as the Library shows it. `link` marks one that arrived
// kept; let go, it stays as `gone` while its issue lists it, so it does not arrive again.
struct Entry {
  Piece piece;
  bool link = false;
  bool gone = false;
};

uint64_t hashOf(const char* bookKey) { return fnv1a64(bookKey, std::strlen(bookKey)); }

bool parseKept(const char* line, const size_t length, Entry& out) {
  JsonDocument doc;
  if (deserializeJson(doc, line, length) || !doc.is<JsonObject>()) return false;
  const char* id = text(doc["id"]);
  const char* book = text(doc["book"]);
  if (!*id || !*book || std::strlen(book) > readlist::BOOK_BYTES) return false;
  out = Entry{};
  readlist::copyCut(out.piece.id, id, ID_MAX);
  readlist::copyCut(out.piece.title, text(doc["title"]), readlist::TITLE_BYTES);
  readlist::copyCut(out.piece.source, text(doc["source"]), SOURCE_BYTES);
  readlist::copyCut(out.piece.lead, text(doc["lead"]), LEAD_BYTES);
  std::memcpy(out.piece.book, book, std::strlen(book) + 1);
  out.piece.minutes = doc["minutes"] | 0;
  out.link = doc["link"] | false;
  out.gone = doc["gone"] | false;
  return true;
}

bool writeLine(HalFile& file, const JsonDocument& doc) {
  char line[768];
  const size_t length = serializeJson(doc, line, sizeof(line) - 1);
  if (length == 0 || length >= sizeof(line) - 1) return false;
  line[length] = '\n';
  return file.write(reinterpret_cast<const uint8_t*>(line), length + 1) == length + 1;
}

bool writeKept(HalFile& file, const Entry& entry) {
  JsonDocument doc;
  doc["id"] = entry.piece.id;
  doc["title"] = entry.piece.title;
  doc["source"] = entry.piece.source;
  doc["lead"] = entry.piece.lead;
  doc["book"] = entry.piece.book;
  doc["minutes"] = entry.piece.minutes;
  if (entry.link) doc["link"] = true;
  if (entry.gone) doc["gone"] = true;
  return writeLine(file, doc);
}

// Reads the kept state: `date` gets the issue last settled (empty for none), `each(entry)` is
// called for every piece.
template <typename Each>
void readKept(LineBuffer& buffer, char (&date)[11], Each each, const bool recover = true) {
  date[0] = '\0';
  // Not while a rewrite has the tmp open: recover() would take it for a write cut short.
  if (recover) statefile::recover(paths::KEPT_TMP, paths::KEPT);
  HalFile file;
  if (!Storage.exists(paths::KEPT) || !Storage.openFileForRead("PAPER", paths::KEPT, file)) return;
  JsonlReader reader(file, buffer);
  size_t length = 0;
  uint32_t offset = 0;
  JsonlReader::Line got;
  bool first = true;
  Entry entry;
  while ((got = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (got != JsonlReader::Line::Ok) continue;
    if (first) {
      first = false;
      JsonDocument doc;
      if (!deserializeJson(doc, buffer.text, length) && doc["issue"].is<const char*>()) {
        readlist::copyCut(date, text(doc["issue"]), sizeof(date) - 1);
        continue;
      }
    }
    if (parseKept(buffer.text, length, entry)) each(entry);
  }
  file.close();
}

// The kept state rewritten: `date` first, then the entries `keep(entry)` lets through (it may
// change them), then `added` when it is not null. Streams, so no more than one entry is held.
template <typename Keep>
bool rewriteKept(LineBuffer& buffer, const char* date, Keep keep, const Entry* added) {
  Storage.mkdir(paths::ROOT);
  Storage.mkdir(paths::STATE);
  statefile::recover(paths::KEPT_TMP, paths::KEPT);
  HalFile out;
  if (!Storage.openFileForWrite("PAPER", paths::KEPT_TMP, out)) return false;
  JsonDocument head;
  head["issue"] = date;
  bool ok = writeLine(out, head);
  char was[11];
  readKept(
      buffer, was,
      [&](Entry& entry) {
        if (ok && keep(entry)) ok = writeKept(out, entry);
      },
      false);
  if (ok && added) ok = writeKept(out, *added);
  return statefile::commit(out, ok, paths::KEPT_TMP, paths::KEPT);
}

void logKept(const char* id, const bool kept) {
  JsonDocument doc;
  doc["id"] = id;
  doc["kept"] = kept;
  doc["day"] = learn::state().day;
  const int64_t at = learn::wallClock();
  if (at > 0) {
    doc["at"] = at;
  } else {
    doc["at"] = nullptr;
  }
  appendJson(paths::KEPT_LOG, doc);
}

struct Kept {
  uint64_t books[KEPT_MAX];
  uint8_t count = 0;
  bool has(const char* bookKey) const {
    const uint64_t hash = hashOf(bookKey);
    for (uint8_t i = 0; i < count; ++i) {
      if (books[i] == hash) return true;
    }
    return false;
  }
};

// The books of the pieces kept now (not let go).
Kept keptBooks(LineBuffer& buffer, char (&date)[11]) {
  Kept kept;
  readKept(buffer, date, [&](const Entry& entry) {
    if (!entry.gone && kept.count < KEPT_MAX) kept.books[kept.count++] = hashOf(entry.piece.book);
  });
  return kept;
}

// Removes the articles under /books/read that are not among `keep`.
void removeArticles(const uint64_t* keep, const uint8_t count) {
  char dir[sizeof(paths::READ_BOOKS)];
  std::memcpy(dir, paths::READ_BOOKS, sizeof(dir));
  dir[sizeof(dir) - 2] = '\0';  // without the trailing '/'
  std::vector<std::string> gone;
  if (HalFile folder = Storage.open(dir); folder && folder.isDirectory()) {
    char name[128];
    for (HalFile f = folder.openNextFile(); f; f = folder.openNextFile()) {
      if (f.isDirectory()) continue;
      f.getName(name, sizeof(name));
      const size_t n = std::strlen(name);
      if (n < 6 || std::strcmp(name + n - 5, ".epub") != 0) continue;
      const std::string key = std::string(dir + 1) + "/" + name;
      const uint64_t hash = hashOf(key.c_str());
      bool wanted = false;
      for (uint8_t i = 0; i < count && !wanted; ++i) wanted = keep[i] == hash;
      if (!wanted) gone.push_back("/" + key);
    }
  }
  for (const std::string& path : gone) Storage.remove(path.c_str());
}

void keptPath(const char* id, char (&out)[64]) { snprintf(out, sizeof(out), "%s/%s.json", paths::KEPT_DIR, id); }

// Leaves the issue's line of piece `id` (or, with `id` null, of the piece whose article is
// `bookKey`) in buffer.text. `idOut` may be null.
bool findInIssue(LineBuffer& buffer, const char* id, const char* bookKey, size_t& length, char* idOut) {
  HalFile file;
  if (!Storage.exists(paths::READ_LIST) || !Storage.openFileForRead("PAPER", paths::READ_LIST, file)) return false;
  JsonlReader reader(file, buffer);
  uint32_t offset = 0;
  JsonlReader::Line got;
  bool found = false;
  JsonDocument filter;
  filter["id"] = true;
  filter["book"] = true;
  while (!found && (got = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (got != JsonlReader::Line::Ok) continue;
    JsonDocument doc;
    if (deserializeJson(doc, buffer.text, length, DeserializationOption::Filter(filter))) continue;
    const char* lineId = text(doc["id"]);
    const char* book = text(doc["book"]);
    if (*book == '/') ++book;
    if (!*lineId || std::strlen(lineId) > ID_MAX) continue;
    found = id ? std::strcmp(lineId, id) == 0 : std::strcmp(book, bookKey) == 0;
    if (found && idOut) std::memcpy(idOut, lineId, std::strlen(lineId) + 1);
  }
  file.close();
  return found;
}

// A kept piece whole, so that it still asks and answers once its issue has gone.
void saveKeptLine(LineBuffer& buffer, const char* id) {
  size_t length = 0;
  if (!findInIssue(buffer, id, nullptr, length, nullptr)) return;
  Storage.mkdir(paths::STATE);
  Storage.mkdir(paths::KEPT_DIR);
  char path[64];
  keptPath(id, path);
  HalFile file;
  if (!Storage.openFileForWrite("PAPER", path, file)) return;
  const bool ok = file.write(reinterpret_cast<const uint8_t*>(buffer.text), length) == length;
  file.close();
  if (!ok) Storage.remove(path);
}

void removeKeptLine(const char* id) {
  char path[64];
  keptPath(id, path);
  if (Storage.exists(path)) Storage.remove(path);
}

bool loadKeptLine(LineBuffer& buffer, const char* id, size_t& length) {
  char path[64];
  keptPath(id, path);
  HalFile file;
  if (!Storage.exists(path) || !Storage.openFileForRead("PAPER", path, file)) return false;
  const int got = file.read(buffer.text, JSONL_LINE_MAX - 1);
  file.close();
  if (got <= 0) return false;
  length = static_cast<size_t>(got);
  buffer.text[length] = '\0';
  return true;
}

Entry entryOf(const Piece& piece, const bool link) {
  Entry entry;
  entry.piece = piece;
  entry.link = link;
  return entry;
}

}  // namespace

void settle(LineBuffer& buffer, const Front& front) {
  if (!front.date[0]) return;
  char date[11];
  const Kept kept = keptBooks(buffer, date);
  const bool fresh = std::strcmp(date, front.date) != 0;
  const readlist::Done done = readlist::loadFinished();

  // What the issue lists, and the links in it that arrive kept.
  auto listed = makeUniqueNoThrow<uint64_t[]>(readlist::MAX + KEPT_MAX);
  auto links = makeUniqueNoThrow<Entry[]>(LINKS_MAX);
  if (!listed || !links) {
    LOG_ERR("PAPER", "OOM: settling the issue");
    return;
  }
  uint8_t listedCount = 0;
  uint8_t linkCount = 0;
  {
    HalFile file;
    if (!Storage.openFileForRead("PAPER", paths::READ_LIST, file)) return;
    JsonlReader reader(file, buffer);
    size_t length = 0;
    uint32_t offset = 0;
    JsonlReader::Line got;
    readlist::Entry entry;
    Line line{};
    while ((got = reader.next(length, offset)) != JsonlReader::Line::End) {
      if (got != JsonlReader::Line::Ok) continue;
      JsonDocument doc;
      if (!parsePiece(buffer.text, length, doc, entry, line)) continue;
      if (listedCount < readlist::MAX) listed[listedCount++] = hashOf(entry.book);
      if (!(doc["kept"] | false) || linkCount >= LINKS_MAX || !onCard(entry)) continue;
      Piece piece;
      readlist::copyCut(piece.id, line.id, ID_MAX);
      readlist::copyCut(piece.source, line.source, SOURCE_BYTES);
      std::memcpy(piece.title, entry.title, sizeof(piece.title));
      readlist::copyCut(piece.lead, line.lead, LEAD_BYTES);
      std::memcpy(piece.book, entry.book, sizeof(piece.book));
      piece.minutes = entry.minutes;
      links[linkCount++] = entryOf(piece, true);
    }
    file.close();
  }
  const auto isListed = [&](const char* bookKey) {
    const uint64_t hash = hashOf(bookKey);
    for (uint8_t i = 0; i < listedCount; ++i) {
      if (listed[i] == hash) return true;
    }
    return false;
  };

  // New links first, one rewrite each.
  uint8_t held = kept.count;
  bool changed = false;
  for (uint8_t i = 0; i < linkCount; ++i) {
    bool known = false;
    char unused[11];
    readKept(buffer, unused, [&](const Entry& entry) {
      known = known || std::strcmp(entry.piece.book, links[i].piece.book) == 0;
    });
    if (known || held >= KEPT_MAX) continue;
    if (rewriteKept(buffer, fresh ? date : front.date, [](Entry&) { return true; }, &links[i])) {
      saveKeptLine(buffer, links[i].piece.id);
      ++held;
      changed = true;
    }
  }
  if (!fresh && !changed) return;

  // A new issue: what was finished or let go leaves, unless the issue still lists it.
  uint8_t keepCount = listedCount;
  const bool ok = rewriteKept(
      buffer, front.date,
      [&](Entry& entry) {
        const bool stays = isListed(entry.piece.book);
        if (fresh && !stays && (entry.gone || done.has(entry.piece.book))) {
          removeKeptLine(entry.piece.id);
          return false;
        }
        if (!entry.gone && keepCount < readlist::MAX + KEPT_MAX) listed[keepCount++] = hashOf(entry.piece.book);
        return true;
      },
      nullptr);
  if (ok && fresh) removeArticles(listed.get(), keepCount);
}

bool toggleKept(LineBuffer& buffer, const Piece& piece, bool& kept) {
  char date[11];
  bool was = false;
  bool link = false;
  uint8_t held = 0;
  readKept(buffer, date, [&](const Entry& entry) {
    if (!entry.gone) ++held;
    if (std::strcmp(entry.piece.book, piece.book) != 0) return;
    was = !entry.gone;
    link = entry.link;
  });
  bool ok = false;
  if (was) {
    // Let go: a link stays as gone while its issue lists it; any other piece just leaves the list.
    ok = rewriteKept(
        buffer, date,
        [&](Entry& entry) {
          if (std::strcmp(entry.piece.book, piece.book) != 0) return true;
          entry.gone = true;
          return entry.link;
        },
        nullptr);
  } else if (link) {
    ok = rewriteKept(
        buffer, date,
        [&](Entry& entry) {
          if (std::strcmp(entry.piece.book, piece.book) == 0) entry.gone = false;
          return true;
        },
        nullptr);
  } else if (held < KEPT_MAX) {
    const Entry added = entryOf(piece, false);
    ok = rewriteKept(buffer, date, [](Entry&) { return true; }, &added);
  }
  if (!ok) {
    kept = was;
    return false;
  }
  kept = !was;
  if (kept) {
    saveKeptLine(buffer, piece.id);  // a link kept again still has its own
  } else if (!link) {
    removeKeptLine(piece.id);
  }
  logKept(piece.id, kept);
  return true;
}

bool toggleKeptBook(LineBuffer& buffer, const char* bookKey, bool& kept) {
  if (*bookKey == '/') ++bookKey;
  Piece piece;
  bool found = false;
  char date[11];
  readKept(buffer, date, [&](const Entry& entry) {
    if (found || std::strcmp(entry.piece.book, bookKey) != 0) return;
    piece = entry.piece;
    found = true;
  });
  if (!found) {
    scan(buffer, nullptr, [&](const readlist::Entry& entry, const Line& line) {
      if (found || std::strcmp(entry.book, bookKey) != 0) return;
      readlist::copyCut(piece.id, line.id, ID_MAX);
      readlist::copyCut(piece.source, line.source, SOURCE_BYTES);
      std::memcpy(piece.title, entry.title, sizeof(piece.title));
      readlist::copyCut(piece.lead, line.lead, LEAD_BYTES);
      std::memcpy(piece.book, entry.book, sizeof(piece.book));
      piece.minutes = entry.minutes;
      found = true;
    });
  }
  if (!found) {
    kept = false;
    return false;
  }
  return toggleKept(buffer, piece, kept);
}

bool loadPieceLine(LineBuffer& buffer, const char* id, size_t& length) {
  return findInIssue(buffer, id, nullptr, length, nullptr) || loadKeptLine(buffer, id, length);
}

bool loadPieceLineOf(LineBuffer& buffer, const char* bookKey, size_t& length, char (&id)[ID_MAX + 1]) {
  if (*bookKey == '/') ++bookKey;
  if (findInIssue(buffer, nullptr, bookKey, length, id)) return true;
  // Not in the issue any more: a kept piece knows its id by its article.
  id[0] = '\0';
  char date[11];
  readKept(buffer, date, [&](const Entry& entry) {
    if (!id[0] && std::strcmp(entry.piece.book, bookKey) == 0) std::memcpy(id, entry.piece.id, sizeof(id));
  });
  return id[0] && loadKeptLine(buffer, id, length);
}

bool carriesMore(LineBuffer& buffer, const char* bookKey) {
  size_t length = 0;
  char id[ID_MAX + 1];
  if (!loadPieceLineOf(buffer, bookKey, length, id)) return false;
  JsonDocument filter;
  filter["link"]["answer"] = true;
  filter["forecast"]["id"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, buffer.text, length, DeserializationOption::Filter(filter))) return false;
  return !doc["link"].isNull() || !doc["forecast"].isNull();
}

bool isKept(LineBuffer& buffer, const char* bookKey) {
  if (*bookKey == '/') ++bookKey;
  char date[11];
  return keptBooks(buffer, date).has(bookKey);
}

uint8_t loadKept(LineBuffer& buffer, readlist::Entry* out, const uint8_t max, uint8_t& unfinished) {
  const readlist::Done done = readlist::loadFinished();
  unfinished = 0;
  uint8_t count = 0;
  char date[11];
  // The state is in the order kept: the newest goes on top, so each group is filled backwards.
  for (const bool wantFinished : {false, true}) {
    const uint8_t start = count;
    readKept(buffer, date, [&](const Entry& entry) {
      if (entry.gone || count >= max || done.has(entry.piece.book) != wantFinished) return;
      char path[readlist::BOOK_BYTES + 2];
      snprintf(path, sizeof(path), "/%s", entry.piece.book);
      if (!Storage.exists(path)) return;
      // Shift what is there down by one and put this one first.
      for (uint8_t i = count; i > start; --i) out[i] = out[i - 1];
      readlist::Entry& slot = out[start];
      slot = readlist::Entry{};
      std::memcpy(slot.title, entry.piece.title, sizeof(slot.title));
      std::memcpy(slot.book, entry.piece.book, sizeof(slot.book));
      slot.minutes = entry.piece.minutes;
      ++count;
    });
    if (!wantFinished) unfinished = count;
  }
  return count;
}

uint8_t loadSection(LineBuffer& buffer, const char* sectionId, Piece* out, const uint8_t max) {
  const uint8_t count = loadSectionPieces(buffer, sectionId, out, max);
  char date[11];
  const Kept kept = keptBooks(buffer, date);
  for (uint8_t i = 0; i < count; ++i) out[i].kept = kept.has(out[i].book);
  return count;
}

}  // namespace x4::paper
