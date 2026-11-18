#include "PaperPlayer.h"

#include <cstdio>
#include <cstring>

#include "Learn.h"
#include "ReadList.h"
#include "StreamInput.h"
#include "Threads.h"

namespace paper = x4::paper;
namespace learn = x4::learn;

namespace {

const char* const WEEKDAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char* const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

}  // namespace

// The issue as it stands on the card now: the host may have written a new one.
void PaperPlayer::load() {
  learn::ensureReady();
  if (!buffer) buffer = std::make_unique<x4::LineBuffer>();
  if (!paper::loadFront(*buffer, front)) front = paper::Front{};
  if (on == On::Front) {
    // The front page is where a new issue is first seen.
    paper::settle(*buffer, front);
    if (place.row >= paper::rows(front)) place.row = 0;
  }
  if (on != On::Section) return;
  if (place.section >= front.count) {
    on = On::Front;
    place = paper::Place{};
    return;
  }
  count = paper::loadSection(*buffer, front.sections[place.section].id, pieces, paper::PIECES_MAX);
  if (place.piece >= count) place.piece = 0;
}

void PaperPlayer::go(const paper::Go to) {
  switch (to) {
    case paper::Go::Section:
      on = On::Section;
      load();
      break;
    case paper::Go::Front:
      on = On::Front;
      break;
    case paper::Go::Piece:
      fromKept = false;
      openPiece(pieces[place.piece].id, pieces[place.piece].book);
      break;
    case paper::Go::Football:
      on = On::Football;
      break;
    case paper::Go::Play:
      on = On::Play;
      break;
    case paper::Go::Book:
      on = On::Book;
      break;
    case paper::Go::Library:
      on = On::Library;
      keptRow = 0;
      break;
    default:
      break;
  }
}

bool PaperPlayer::press(const std::string& button, const bool hold) {
  static const struct {
    const char* name;
    paper::Key key;
  } KEYS[] = {{"back", paper::Key::Back},   {"confirm", paper::Key::Confirm}, {"left", paper::Key::Left},
              {"right", paper::Key::Right}, {"up", paper::Key::Up},           {"down", paper::Key::Down}};
  const paper::Key* key = nullptr;
  for (const auto& k : KEYS) {
    if (button == k.name) key = &k.key;
  }
  if (!key) return false;
  load();
  // Holding Back is the front page, from anywhere.
  if (hold && *key == paper::Key::Back) {
    on = On::Front;
    place.screen = paper::Screen::Front;
    return true;
  }
  switch (on) {
    case On::Front:
      go(paper::press(place, front, count, *key));
      break;
    case On::Section:
      if (hold && *key == paper::Key::Confirm) {
        bool kept = false;
        if (place.piece < count) paper::toggleKept(*buffer, pieces[place.piece], kept);
        load();
        break;
      }
      go(paper::press(place, front, count, *key));
      break;
    case On::Library:
      // The Kept tab: Up and Down move, a hold of Confirm lets the piece go, Confirm opens it.
      if (*key == paper::Key::Back) {
        on = On::Front;
      } else if (*key == paper::Key::Down || *key == paper::Key::Up) {
        x4::readlist::Entry kept[paper::KEPT_MAX];
        uint8_t unfinished = 0;
        const uint8_t n = paper::loadKept(*buffer, kept, paper::KEPT_MAX, unfinished);
        if (n > 0) keptRow = static_cast<uint8_t>((keptRow + (*key == paper::Key::Down ? 1 : n - 1)) % n);
      } else if (*key == paper::Key::Confirm) {
        x4::readlist::Entry kept[paper::KEPT_MAX];
        uint8_t unfinished = 0;
        const uint8_t n = paper::loadKept(*buffer, kept, paper::KEPT_MAX, unfinished);
        if (keptRow >= n) break;
        if (hold) {
          bool still = false;
          paper::toggleKeptBook(*buffer, kept[keptRow].book, still);
          keptRow = 0;
        } else {
          // A kept piece still carries what its issue gave it.
          fromKept = true;
          size_t length = 0;
          char id[x4::ID_MAX + 1] = {};
          paper::loadPieceLineOf(*buffer, kept[keptRow].book, length, id);
          openPiece(id, kept[keptRow].book);
        }
      }
      break;
    case On::Reader:
      if (menu) {
        if (*key == paper::Key::Confirm) {
          bool kept = false;
          paper::toggleKeptBook(*buffer, book.c_str(), kept);
        }
        if (*key == paper::Key::Confirm || *key == paper::Key::Back) menu = false;
        break;
      }
      if (*key == paper::Key::Confirm) {
        menu = true;
        break;
      }
      if (*key == paper::Key::Right) {
        if (turned) atEnd = true;
        if (!turned) learn::addReading(("/" + book).c_str(), 30000, 1);  // a page turned
        turned = true;
      } else if (*key == paper::Key::Back) {
        x4::readlist::onBookClosed(("/" + book).c_str(), atEnd ? 1.0f : turned ? 0.5f : 0.0f, atEnd, 60000);
        // Read, the piece may have something to follow it: the answer, then one more thing.
        run = learn::StepRun{};
        run.forPiece(pieceId, true);
        after = true;
        if (!pieceId.empty() && run.load(stepBuffer.get())) {
          on = On::Question;
        } else {
          leavePiece();
        }
      }
      break;
    case On::Thread:
      // The story so far: Confirm or Right goes on to the piece, Back returns to its section.
      if (*key == paper::Key::Confirm || *key == paper::Key::Right) {
        if (asksFirst) {
          on = On::Question;
        } else {
          openReader();
        }
      } else if (*key == paper::Key::Back) {
        leavePiece();
      }
      break;
    case On::Question:
      pressQuestion(button);
      break;
    case On::Football:
      if (*key == paper::Key::Left || *key == paper::Key::Right) {
        go(paper::leaveFootball(place, front, *key == paper::Key::Right));
      } else if (*key == paper::Key::Back) {
        on = On::Front;
        place.screen = paper::Screen::Front;
      }
      break;
    default:
      if (*key == paper::Key::Back) on = On::Front;
      break;
  }
  return true;
}

void PaperPlayer::openReader() {
  on = On::Reader;
  turned = atEnd = menu = false;
}

void PaperPlayer::leavePiece() {
  on = fromKept ? On::Library : On::Section;
  fromKept = false;
  load();
}

// Confirm on a piece: its story so far when that changed, its question when it has one not
// yet guessed, then the article.
void PaperPlayer::openPiece(const char* id, const char* bookKey) {
  if (!stepBuffer) stepBuffer = std::make_unique<x4::LineBuffer>();
  pieceId = id;
  book = bookKey;
  after = false;
  run = learn::StepRun{};
  run.forPiece(pieceId, false);
  asksFirst = !pieceId.empty() && run.load(stepBuffer.get());
  x4::threads::Thread thread;
  if (run.pieceThread()[0] && x4::threads::load(run.pieceThread(), thread)) {
    x4::threads::markShown(run.pieceThread(), thread);
    on = On::Thread;
    return;
  }
  if (asksFirst) {
    on = On::Question;
  } else {
    openReader();
  }
}

void PaperPlayer::pressQuestion(const std::string& button) {
  using After = learn::StepRun::After;
  x4::StreamKeys keys;
  keys.confirm = button == "confirm";
  keys.frontNext = button == "right";
  keys.frontPrevious = button == "left";
  keys.next = keys.frontNext || button == "down";
  keys.previous = keys.frontPrevious || button == "up";
  x4::StreamView view;
  view.phase = run.phase;
  view.shape = run.item.shape;
  view.firstOption = run.selected == 0;
  view.lastOption = run.selected + 1 >= run.item.optionCount;
  const After done = button == "back" ? run.apply(x4::StreamMove::Back, keys) : run.apply(x4::streamMove(view, keys), keys);
  if (done == After::Open) openReader();
  if (done == After::Leave) leavePiece();
}

void PaperPlayer::screen(JsonObject out) {
  load();
  static const char* const NAMES[] = {"Home",       "Section", "EpubReader", "Football", "Play",
                                      "EpubReader", "Library", "Questions",  "Thread"};
  out["activity"] = NAMES[static_cast<uint8_t>(on)];
  if (on == On::Question) {
    out["piece"] = pieceId;
    JsonObject step = out["step"].to<JsonObject>();
    run.describe(step);
    step.remove("done");
    step.remove("total");
    // The set calls both parts of an article step "article"; here they are told apart.
    if (run.item.shape == x4::Shape::Predict) step["kind"] = "predict";
    if (run.item.shape == x4::Shape::Link) step["kind"] = "link";
    step["prompt"] = x4::text(stepBuffer->text, run.item.title);
    return;
  }
  if (on == On::Thread) {
    out["piece"] = pieceId;
    return;
  }
  if (on == On::Reader) {
    out["book"] = book;
    out["page"] = atEnd ? "end" : turned ? "turned" : "first";
    if (menu) {
      out["activity"] = "EpubReaderMenu";
      out["row"] = paper::isKept(*buffer, book.c_str()) ? "Let go" : "Keep";
    }
    return;
  }
  if (on == On::Library) {
    x4::readlist::Entry kept[paper::KEPT_MAX];
    uint8_t unfinished = 0;
    const uint8_t n = paper::loadKept(*buffer, kept, paper::KEPT_MAX, unfinished);
    out["tab"] = "Kept";
    out["selected"] = keptRow;
    out["unfinished"] = unfinished;
    JsonArray list = out["kept"].to<JsonArray>();
    for (uint8_t i = 0; i < n; ++i) list.add(kept[i].title);
    return;
  }
  if (on != On::Front && on != On::Section) return;
  JsonObject view = out["paper"].to<JsonObject>();
  if (on == On::Section) {
    const paper::Section& section = front.sections[place.section];
    out["section"] = section.id;
    view["screen"] = "section";
    view["name"] = section.name;
    view["selected"] = place.piece;
    if (std::strcmp(section.id, paper::FOOTBALL) == 0) {
      // As SectionActivity names them (english.yaml), then the competitions.
      JsonArray tabs = view["tabs"].to<JsonArray>();
      tabs.add("Analysis");
      tabs.add("Matches");
      char comps[paper::COMPETITIONS_MAX][paper::COMPETITION_BYTES + 1];
      const uint8_t n = paper::loadCompetitions(comps, paper::COMPETITIONS_MAX);
      for (uint8_t i = 0; i < n; ++i) tabs.add(comps[i]);
    }
    JsonArray list = view["pieces"].to<JsonArray>();
    for (uint8_t i = 0; i < count; ++i) {
      JsonObject piece = list.add<JsonObject>();
      piece["id"] = pieces[i].id;
      piece["source"] = pieces[i].source;
      piece["title"] = pieces[i].title;
      piece["lead"] = pieces[i].lead;
      piece["read"] = pieces[i].read;
      piece["kept"] = pieces[i].kept;
    }
    if (count > 0) out["piece"] = pieces[place.piece].id;
    return;
  }
  view["screen"] = "front";
  view["selected"] = place.row;
  x4::CivilTime now{};
  if (learn::localTime(now)) {
    char date[24];
    snprintf(date, sizeof(date), "%s %u %s", WEEKDAYS[now.weekday % 7], static_cast<unsigned>(now.day),
             MONTHS[(now.month + 11) % 12]);
    view["date"] = date;
  }
  JsonArray list = view["sections"].to<JsonArray>();
  for (uint8_t i = 0; i < front.count; ++i) {
    JsonObject section = list.add<JsonObject>();
    section["id"] = front.sections[i].id;
    section["name"] = front.sections[i].name;
    section["head"] = front.sections[i].head;
  }
  out["row"] = place.row;
}
