#pragma once

#include <ArduinoJson.h>

#include <memory>
#include <string>

#include "Paper.h"
#include "StepRun.h"

// The newspaper as the X4 plays it, driven by button names: the firmware's own issue reader
// and key logic, without the drawing. The reader, Play, the Library and Football's tables are
// not played: in the reader Right turns a page, then reaches the end, and Back leaves; Confirm
// opens its menu, where Confirm takes the first row (Keep, or Let go) and Back closes it. From
// the others Back returns to the front page.
class PaperPlayer {
 public:
  // back confirm left right up down. `hold` is a long press. False for another name.
  bool press(const std::string& button, bool hold);
  // What `screen` answers: the activity, and under `paper` the view the screen is drawn from
  // (firmware/test/draw/x4paper reads it).
  void screen(JsonObject out);

 private:
  enum class On : uint8_t { Front, Section, Reader, Football, Play, Book, Library, Question, Thread };
  On on = On::Front;
  std::unique_ptr<x4::LineBuffer> buffer;
  x4::paper::Front front;
  x4::paper::Piece pieces[x4::paper::PIECES_MAX];
  uint8_t count = 0;
  x4::paper::Place place;
  // The piece open in the reader.
  std::string book;
  bool turned = false;
  bool atEnd = false;
  bool menu = false;      // the reader's menu is open
  bool fromKept = false;  // the reader was opened from the Library's Kept
  uint8_t keptRow = 0;

  // The piece on its way through: its thread, its question, the reader, what follows it.
  std::unique_ptr<x4::LineBuffer> stepBuffer;
  x4::learn::StepRun run;
  std::string pieceId;
  bool after = false;  // the question screen shows what follows the article
  bool asksFirst = false;  // the piece has a question to ask before its article

  void load();
  void go(x4::paper::Go to);
  void openPiece(const char* id, const char* bookKey);
  void openReader();
  void leavePiece();
  void pressQuestion(const std::string& button);
};
