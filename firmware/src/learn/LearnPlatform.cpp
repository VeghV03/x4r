// Device glue for the learning core: clock, book stats and the USB hooks.
#include <Arduino.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <TrustedTime.h>

#include <cstring>

#include "Learn.h"
#include "LearnPaths.h"
#include "Paper.h"
#include "RecentBooksStore.h"
#include "play/Play.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "net/NetSync.h"
#include "usb/UsbHooks.h"
#include "usb/UsbProtocol.h"
#include "util/BookProgress.h"

namespace x4::learn {

int64_t platformNow() { return trustedtime::trustedNow(); }

uint32_t platformMillis() { return millis(); }

bool platformHostClock(int32_t& tzMinutes) {
#if X4R
  int16_t tz = static_cast<int16_t>(tzMinutes);
  if (!x4HostClock(tz)) return false;
  tzMinutes = tz;
  return true;
#else
  (void)tzMinutes;
  return false;  // builds without the USB link (usb/) never hear the host's clock
#endif
}

void addBookStats(JsonObject books) {
  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (RecentBooksStore::isMissing(book)) continue;
    const int percent = loadBookProgress(book.path);
    if (percent < 0) continue;
    // Keys are SD paths without the leading slash, as everywhere in /x4.
    const char* key = book.path.c_str();
    if (*key == '/') ++key;
    // Merged into the reading totals the core already wrote.
    JsonObject entry = books[key];
    if (entry.isNull()) entry = books[key].to<JsonObject>();
    entry["progress"] = static_cast<double>(percent) / 100.0;
  }
}

void addPlayStats(JsonObject play) { x4::play::addStats(play); }

}  // namespace x4::learn

namespace {
bool learnFilesChanged = false;  // since the last session end

// A visible folder may hold books (`x4 rm /books/old -r`, a moved folder).
bool mayHoldBooks(const char* path) {
  const char* name = std::strrchr(path, '/');
  name = name ? name + 1 : path;
  return *name != '\0' && *name != '.' && std::strchr(name, '.') == nullptr;
}
}  // namespace

void x4OnHostWrote(const char* path) {
  const char* root = x4::paths::ROOT;
  if (!usb::samePathIgnoringCase(path, root) && !usb::isInsideIgnoringCase(path, root)) {
    // The Library reconciles its index on next entry.
    if (library::isLibraryBookFile(path) || mayHoldBooks(path)) library::markLibraryIndexDirty();
    return;
  }
  x4::learn::onHostDataChanged();
  learnFilesChanged = true;
}

void x4OnHostDataChanged() {
  sdFontSystem.markRegistryDirty();
  // The front page shows what the host just changed: re-entering it rebuilds.
  learnFilesChanged = false;
  if (activityManager.isForegroundHomeShown()) {
    activityManager.goHome();
  } else if (std::strcmp(activityManager.currentActivityName(), "Library") == 0) {
    // An open Library shows the books and articles the host just sent; entering it rebuilds a stale index.
    activityManager.goToLibrary();
  }
}

void x4HelloFields(JsonObject out) {
  x4::learn::helloFields(out);
  x4::net::helloFields(out);
  // The issue on the card, by its date.
  auto buffer = makeUniqueNoThrow<x4::LineBuffer>();
  auto front = makeUniqueNoThrow<x4::paper::Front>();
  if (buffer && front && x4::paper::loadFront(*buffer, *front) && front->date[0]) out["issue"] = front->date;
}

const char* x4NetCommand(JsonObjectConst args, JsonObject out) { return x4::net::command(args, out); }
void x4BeforeHostRead() { x4::learn::flushStats(); }

void x4OnHostTime(const int64_t now, const int16_t tzMinutes, const bool tzKnown) {
  x4::learn::onHostTime(now, tzKnown ? tzMinutes : x4::learn::state().tzMinutes);
}
