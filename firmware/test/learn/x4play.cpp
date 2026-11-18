// x4play <folder>: the X4's paper as its Home plays, with the folder as the SD card.
// One command a line on stdin, one JSON line back:
//   press <button> [long]   back confirm left right up down power
//   screen             {"activity":…,"step":{…}} as the `screen` command (docs/PROTOCOL.md)
//   time <unix> <tz>   the host set the clock; tz in minutes east of UTC
//   changed            the host wrote files under /x4
// The folder is read before each command and written after it, so the host's files and the
// X4's records meet there as they do on the card.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "HalStorage.h"
#include "Learn.h"
#include "PaperPlayer.h"

namespace fs = std::filesystem;
namespace learn = x4::learn;

namespace {
int64_t hostNow = 0;
int32_t hostTz = 0;
uint32_t clockMs = 0;
}  // namespace

namespace x4::learn {
int64_t platformNow() { return hostNow; }
// Every look at the clock is a second later, so each step takes some time.
uint32_t platformMillis() { return clockMs += 1000; }
void addBookStats(JsonObject) {}
void addPlayStats(JsonObject) {}
bool platformHostClock(int32_t& tz) {
  if (hostNow > 0) tz = hostTz;
  return hostNow > 0;
}
}  // namespace x4::learn

namespace {

// What the folder held under /x4 when it was last read or written.
std::map<std::string, std::string> card;
// The other files it held (books, articles): only their names matter.
std::set<std::string> books;

bool isData(const std::string& path) { return path.rfind("/x4/", 0) == 0; }

std::string bytesOf(const fake::Node& node) { return {node.bytes.begin(), node.bytes.end()}; }

// Files under /x4 are read whole; anything else (a book) only has to exist.
void readCard(const fs::path& root) {
  std::set<std::string> found;
  card.clear();
  books.clear();
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    const std::string path = "/" + fs::relative(entry.path(), root).generic_string();
    found.insert(path);
    const auto known = fake::files.find(path);
    if (entry.is_directory()) {
      if (known == fake::files.end()) Storage.mkdir(path.c_str());
      continue;
    }
    if (!entry.is_regular_file()) continue;
    if (!isData(path)) {
      if (known == fake::files.end()) fake::add(path);
      books.insert(path);
      continue;
    }
    std::ifstream in(entry.path(), std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    card[path] = bytes;
    if (known != fake::files.end() && !known->second->directory && bytesOf(*known->second) == bytes) continue;
    // The folder's own time, as the card keeps it.
    const auto modified = fs::last_write_time(entry.path()).time_since_epoch();
    fake::add(path, bytes, static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(modified).count()));
  }
  for (auto it = fake::files.begin(); it != fake::files.end();) {
    it = it->first != "/" && !found.count(it->first) ? fake::files.erase(it) : std::next(it);
  }
}

void writeCard(const fs::path& root) {
  for (const auto& [path, node] : fake::files) {
    if (node->directory || !isData(path)) continue;
    const std::string bytes = bytesOf(*node);
    const auto had = card.find(path);
    if (had != card.end() && had->second == bytes) continue;
    const fs::path target = root / path.substr(1);
    fs::create_directories(target.parent_path());
    std::ofstream(target, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    card[path] = bytes;
  }
  // An article the paper removed leaves the folder too.
  for (const std::string& path : books) {
    if (!fake::files.count(path)) fs::remove(root / path.substr(1));
  }
  for (auto it = card.begin(); it != card.end();) {
    if (fake::files.count(it->first)) {
      ++it;
      continue;
    }
    fs::remove(root / it->first.substr(1));
    it = card.erase(it);
  }
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc != 2 || !fs::is_directory(argv[1])) {
    std::cerr << "usage: x4play <folder that stands for the SD card>\n";
    return 2;
  }
  const fs::path root = argv[1];
  PaperPlayer paper;
  std::string line;
  while (std::getline(std::cin, line)) {
    readCard(root);
    JsonDocument doc;
    JsonObject out = doc.to<JsonObject>();
    std::istringstream in(line);
    std::string command;
    in >> command;
    if (command == "press") {
      std::string button, how;
      in >> button >> how;
      if (!paper.press(button, how == "long")) out["error"] = "unknown button";
    } else if (command == "screen") {
      paper.screen(out);
    } else if (command == "time") {
      in >> hostNow >> hostTz;
      learn::onHostTime(hostNow, hostTz);
    } else if (command == "changed") {
      learn::onHostDataChanged();
    } else {
      out["error"] = "unknown command";
    }
    learn::flushStats();
    writeCard(root);
    serializeJson(doc, std::cout);
    std::cout << '\n' << std::flush;
  }
  return 0;
}
