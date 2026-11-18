#include "Threads.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "JsonlReader.h"
#include "LearnPaths.h"
#include "LearnTypes.h"
#include "StateFile.h"

namespace x4::threads {

namespace {

// /x4/state/threads.bin: the version last shown of each thread, oldest first.
struct Shown {
  uint64_t id;    // fnv1a64 of the thread's id
  uint64_t text;  // fnv1a64 of its title and body
};

uint64_t versionOf(const Thread& thread) {
  const uint64_t title = fnv1a64(thread.title, std::strlen(thread.title));
  return fnv1a64(thread.body, std::strlen(thread.body), title);
}

uint8_t loadShown(Shown* out) {
  statefile::recover(paths::THREADS_SHOWN_TMP, paths::THREADS_SHOWN);
  HalFile file;
  if (!Storage.exists(paths::THREADS_SHOWN) || !Storage.openFileForRead("X4", paths::THREADS_SHOWN, file)) return 0;
  const int got = file.read(out, REMEMBERED * sizeof(Shown));
  file.close();
  return got > 0 ? static_cast<uint8_t>(static_cast<size_t>(got) / sizeof(Shown)) : 0;
}

}  // namespace

bool load(const char* id, Thread& out) {
  out = Thread{};
  if (!id || !*id || !Storage.exists(paths::THREADS)) return false;
  HalFile file;
  if (!Storage.openFileForRead("X4", paths::THREADS, file)) return false;
  auto buffer = makeUniqueNoThrow<LineBuffer>();
  if (!buffer) {
    LOG_ERR("X4", "OOM: thread line");
    file.close();
    return false;
  }
  JsonlReader reader(file, *buffer);
  size_t length = 0;
  uint32_t offset = 0;
  JsonlReader::Line line;
  bool found = false;
  while (!found && (line = reader.next(length, offset)) != JsonlReader::Line::End) {
    if (line != JsonlReader::Line::Ok || !std::strstr(buffer->text, id)) continue;
    JsonDocument doc;
    if (deserializeJson(doc, buffer->text, length) || std::strcmp(doc["id"] | "", id) != 0) continue;
    const char* title = doc["title"] | "";
    const char* body = doc["body"] | "";
    if (!*title || !*body || std::strlen(title) > TITLE_MAX || std::strlen(body) > BODY_MAX) break;
    std::strcpy(out.title, title);
    std::strcpy(out.body, body);
    found = true;
  }
  file.close();
  if (!found) return false;

  Shown shown[REMEMBERED];
  const uint8_t count = loadShown(shown);
  const uint64_t key = fnv1a64(id, std::strlen(id));
  const uint64_t version = versionOf(out);
  for (uint8_t i = 0; i < count; ++i) {
    if (shown[i].id == key && shown[i].text == version) return false;
  }
  return true;
}

void markShown(const char* id, const Thread& thread) {
  Shown shown[REMEMBERED + 1];
  uint8_t count = loadShown(shown);
  const uint64_t key = fnv1a64(id, std::strlen(id));
  // Its old version goes; the new one is the newest.
  uint8_t kept = 0;
  for (uint8_t i = 0; i < count; ++i) {
    if (shown[i].id != key) shown[kept++] = shown[i];
  }
  shown[kept++] = {key, versionOf(thread)};
  const Shown* first = kept > REMEMBERED ? shown + 1 : shown;
  count = kept > REMEMBERED ? REMEMBERED : kept;
  Storage.mkdir(paths::STATE);
  if (!statefile::replace(paths::THREADS_SHOWN_TMP, paths::THREADS_SHOWN, {{first, count * sizeof(Shown)}})) {
    LOG_ERR("X4", "Cannot remember the thread shown");
  }
}

}  // namespace x4::threads
