#include "NextBooks.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "JsonlReader.h"
#include "LearnPaths.h"

namespace x4::nextbooks {

bool load(const char* bookPath, std::vector<Pick>& out) {
  out.clear();
  const char* key = *bookPath == '/' ? bookPath + 1 : bookPath;
  if (!Storage.exists(paths::NEXT)) return false;
  HalFile file;
  if (!Storage.openFileForRead("X4", paths::NEXT, file)) return false;
  auto buffer = makeUniqueNoThrow<LineBuffer>();
  if (!buffer) {
    LOG_ERR("X4", "OOM: next books line");
    file.close();
    return false;
  }
  JsonlReader reader(file, *buffer);
  size_t length = 0;
  uint32_t offset = 0;
  JsonlReader::Line line;
  bool found = false;
  while (!found && (line = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (line != JsonlReader::Line::Ok || !std::strstr(buffer->text, key)) continue;
    JsonDocument doc;
    if (deserializeJson(doc, buffer->text, length)) continue;
    if (std::strcmp(doc["after"] | "", key) != 0) continue;
    found = true;  // one record per book
    for (JsonObjectConst pick : doc["books"].as<JsonArrayConst>()) {
      const char* book = pick["book"] | "";
      const char* title = pick["title"] | "";
      if (out.size() >= MAX || !*book || !*title || std::strcmp(book, key) == 0) continue;
      std::string path = std::string("/") + book;
      if (!Storage.exists(path.c_str())) continue;
      out.push_back(Pick{std::move(path), title});
    }
  }
  file.close();
  return !out.empty();
}

}  // namespace x4::nextbooks
