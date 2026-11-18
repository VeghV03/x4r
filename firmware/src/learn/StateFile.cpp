#include "StateFile.h"

#include <Logging.h>

namespace x4::statefile {

bool commit(HalFile& file, const bool ok, const char* tmp, const char* path) {
  // close() syncs: a failure means the bytes may not all be on the card.
  const bool closed = file.close();
  if (!ok || !closed) {
    Storage.remove(tmp);
    return false;
  }
  if (!Storage.replaceFile(tmp, path)) {
    LOG_ERR("X4", "Cannot replace %s", path);
    return false;
  }
  return true;
}

bool replace(const char* tmp, const char* path, const std::initializer_list<Part> parts) {
  HalFile file;
  if (!Storage.openFileForWrite("X4", tmp, file)) return false;
  bool ok = true;
  for (const Part& part : parts) {
    if (!ok) break;
    ok = part.length == 0 || file.write(part.data, part.length) == part.length;
  }
  return commit(file, ok, tmp, path);
}

void recover(const char* tmp, const char* path) {
  if (!Storage.exists(tmp)) return;
  if (Storage.exists(path)) {
    Storage.remove(tmp);
  } else if (Storage.rename(tmp, path)) {
    LOG_INF("X4", "Recovered %s", path);
  }
}

}  // namespace x4::statefile
