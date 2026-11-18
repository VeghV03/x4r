#include "Previously.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "BookStats.h"
#include "JsonlReader.h"
#include "Learn.h"
#include "LearnPaths.h"

namespace x4::previously {

namespace {

bool away(const char* key) {
  books::LastRead last{};
  if (!books::lastRead(key, last)) return false;
  const LearnState& g = learn::state();
  if (g.day >= last.day + AWAY_DAYS) return true;
  const int64_t clock = learn::wallClock();
  return last.at > 0 && clock > 0 && clock - last.at >= AWAY_SECONDS;
}

}  // namespace

bool load(const char* bookPath, const int percent, char (&out)[TEXT_MAX + 1]) {
  out[0] = '\0';
  const char* key = *bookPath == '/' ? bookPath + 1 : bookPath;
  if (percent < 0 || !away(key) || !Storage.exists(paths::PREVIOUSLY)) return false;
  HalFile file;
  if (!Storage.openFileForRead("X4", paths::PREVIOUSLY, file)) return false;
  auto buffer = makeUniqueNoThrow<LineBuffer>();
  if (!buffer) {
    LOG_ERR("X4", "OOM: recap line");
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
    const char* book = doc["book"] | "";
    const char* text = doc["text"] | "";
    if (std::strcmp(book, key) != 0) continue;
    found = true;  // one recap per book
    const int at = static_cast<int>(std::lround((doc["progress"] | -1.0) * 100));
    if (std::abs(at - percent) <= PERCENT_SLACK && std::strlen(text) <= TEXT_MAX) std::strcpy(out, text);
  }
  file.close();
  return out[0] != '\0';
}

}  // namespace x4::previously
