// The paper's screens through the firmware's own drawing (src/learn/ui/PaperDraw): reads one
// JSON view on stdin and writes the panel as a PGM.
//
//   x4paper --fonts build/fonts --out front.pgm [--family Literata] [--gray] < view.json
//
//   {"screen":"front","date":"Wed 11 Nov","agenda":"6° rain","selected":0,"play":"Crossword",
//    "book":"A Course in Behavioral Economics","percent":"9%",
//    "sections":[{"name":"Hungary","head":"Orbán Áron-ügy: letartóztatás"}]}
//   {"screen":"section","name":"World","selected":0,
//    "pieces":[{"source":"Telex","title":"…","lead":"…","read":false}]}
//   Football's adds "tabs":["Analysis","Matches","PL",…], drawn in place of the name.
#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "HostGfx.h"
#include "fontIds.h"
#include "learn/ui/PaperDraw.h"

using namespace x4;

namespace {

const char* text(JsonVariantConst v) {
  const char* s = v.as<const char*>();
  return s ? s : "";
}

void copy(char* out, const size_t size, const char* s) { snprintf(out, size, "%s", s); }

}  // namespace

int main(int argc, char** argv) {
  std::string fonts = "build/fonts", out = "paper.pgm", family = "Literata";
  bool gray = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--gray") {
      gray = true;
    } else if (i + 1 < argc && arg == "--fonts") {
      fonts = argv[++i];
    } else if (i + 1 < argc && arg == "--out") {
      out = argv[++i];
    } else if (i + 1 < argc && arg == "--family") {
      family = argv[++i];
    } else {
      fprintf(stderr, "usage: x4paper --fonts <dir> --out <file.pgm> [--family <name>] [--gray] < view.json\n");
      return 2;
    }
  }
  const std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  JsonDocument doc;
  if (deserializeJson(doc, input)) {
    fprintf(stderr, "x4paper: the view is not valid JSON\n");
    return 2;
  }

  HostGfx gfx;
  paper::Faces faces{UI_10_FONT_ID, SMALL_FONT_ID, gfx.loadSdFamily(fonts, family, 12),
                     gfx.loadSdFamily(fonts, family, 14)};
  if (!faces.head || !faces.title) {
    fprintf(stderr, "x4paper: %s 12 and 14 are not in %s\n", family.c_str(), fonts.c_str());
    return 1;
  }
  const GfxRenderer& r = gfx.renderer();

  if (std::strcmp(text(doc["screen"]), "section") == 0) {
    std::vector<paper::Piece> pieces;
    for (const JsonVariantConst p : doc["pieces"].as<JsonArrayConst>()) {
      paper::Piece piece;
      copy(piece.source, sizeof(piece.source), text(p["source"]));
      copy(piece.title, sizeof(piece.title), text(p["title"]));
      copy(piece.lead, sizeof(piece.lead), text(p["lead"]));
      piece.read = p["read"] | false;
      piece.kept = p["kept"] | false;
      pieces.push_back(piece);
    }
    std::vector<const char*> tabs;
    for (const JsonVariantConst t : doc["tabs"].as<JsonArrayConst>()) tabs.push_back(text(t));
    paper::SectionView view;
    view.name = text(doc["name"]);
    view.tabs = tabs.data();
    view.tabCount = static_cast<uint8_t>(tabs.size());
    view.pieces = pieces.data();
    view.count = static_cast<uint8_t>(pieces.size());
    view.selected = doc["selected"] | 0;
    gfx.render([&](bool pass) { paper::drawSection(r, faces, view, pass); }, gray);
  } else {
    paper::Front front;
    for (const JsonVariantConst s : doc["sections"].as<JsonArrayConst>()) {
      if (front.count >= paper::SECTIONS_MAX) break;
      paper::Section& section = front.sections[front.count++];
      copy(section.name, sizeof(section.name), text(s["name"]));
      copy(section.head, sizeof(section.head), text(s["head"]));
    }
    paper::FrontView view;
    view.front = &front;
    view.date = text(doc["date"]);
    view.agenda = text(doc["agenda"]);
    view.play = text(doc["play"]);
    view.book = text(doc["book"]);
    view.percent = text(doc["percent"]);
    view.selected = doc["selected"] | 0;
    gfx.render([&](bool pass) { paper::drawFront(r, faces, view, pass); }, gray);
  }
  if (!gfx.savePgm(out)) {
    fprintf(stderr, "x4paper: cannot write %s\n", out.c_str());
    return 1;
  }
  return 0;
}
