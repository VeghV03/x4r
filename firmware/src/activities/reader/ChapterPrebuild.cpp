#include "ChapterPrebuild.h"

#include <Arduino.h>
#include <Epub.h>
#include <Epub/Section.h>
#include <Epub/blocks/ImageBlock.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "EpubReaderUtils.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"

namespace prebuild {

namespace {

constexpr int MAX_BOOKS = 4;            // the most recent ones
constexpr int PAGES_PER_STEP = 2;       // as the reader's background build
constexpr int CHECKS_PER_STEP = 8;      // built chapters passed over per step
constexpr size_t MIN_FREE_HEAP = 32 * 1024;
constexpr size_t MIN_MAX_ALLOC = 16 * 1024;
constexpr int MAX_SKIPPED = 8;  // chapters of one book the builder gives up on

// One pass over the recent books; any press or USB command starts it again.
struct Pass {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section;
  int book = 0;   // index in the recent books
  int start = 0;  // the spine the reader is at
  int next = 0;   // spines looked at, from `start` around
  int spine = 0;  // the chapter `section` builds
  uint16_t skipped[MAX_SKIPPED] = {};
  int skippedCount = 0;
  bool done = false;
  unsigned long startedAt = 0;
  int built = 0;
};
Pass pass;

bool contactClosed() { return gpio.rawInputActive(); }

bool isEpub(const std::string& path) {
  return path.size() > 5 && strcasecmp(path.c_str() + path.size() - 5, ".epub") == 0;
}

// In the book's cache folder: the chapter being built, left behind only by a crash or power cut,
// and the chapters that failed, so one that crashes the build is not tried again at every rest.
std::string tryingPath() { return pass.epub->getCachePath() + "/prebuild.try"; }
std::string skippedPath() { return pass.epub->getCachePath() + "/prebuild.skip"; }

bool isSkipped(const int spine) {
  for (int i = 0; i < pass.skippedCount; ++i) {
    if (pass.skipped[i] == spine) return true;
  }
  return false;
}

void skip(const int spine) {
  if (isSkipped(spine) || pass.skippedCount >= MAX_SKIPPED) return;
  LOG_ERR("PRE", "Chapter %d: not built ahead again", spine);
  pass.skipped[pass.skippedCount++] = static_cast<uint16_t>(spine);
  HalFile f;
  if (!Storage.openFileForWrite("PRE", skippedPath(), f)) return;
  f.write(pass.skipped, pass.skippedCount * sizeof(uint16_t));
  f.close();
}

void markTrying(const int spine) {
  const uint16_t value = static_cast<uint16_t>(spine);
  HalFile f;
  if (!Storage.openFileForWrite("PRE", tryingPath(), f)) return;
  f.write(&value, sizeof(value));
  f.close();
}

void loadSkipped() {
  pass.skippedCount = 0;
  HalFile f;
  if (Storage.openFileForRead("PRE", skippedPath(), f)) {
    pass.skippedCount = f.read(pass.skipped, sizeof(pass.skipped)) / static_cast<int>(sizeof(uint16_t));
    if (pass.skippedCount < 0) pass.skippedCount = 0;
    f.close();
  }
  const std::string trying = tryingPath();
  if (Storage.openFileForRead("PRE", trying, f)) {
    uint16_t spine = 0;
    const bool read = f.read(&spine, sizeof(spine)) == sizeof(spine);
    f.close();
    if (read) skip(spine);
    Storage.remove(trying.c_str());
  }
}

void closeBook() {
  if (pass.section) {
    pass.section.reset();  // keeps a started chapter as a partial
    Storage.remove(tryingPath().c_str());  // stopped, not failed
  }
  pass.epub.reset();
}

// Opens the pass's book if the reader opened it before; false to move past it.
bool openBook() {
  const RecentBook& recent = RECENT_BOOKS.getBooks()[pass.book];
  if (!isEpub(recent.path) || RecentBooksStore::isMissing(recent)) return false;
  std::shared_ptr<Epub> epub = makeUniqueNoThrow<Epub>(recent.path, "/.crosspoint");
  if (!epub || !Storage.exists((epub->getCachePath() + "/book.bin").c_str()) ||
      !epub->load(false, SETTINGS.embeddedStyle == 0) || epub->getSpineItemsCount() <= 0) {
    return false;
  }
  epub->setupCacheDir();
  pass.start = 0;
  HalFile f;
  if (Storage.openFileForRead("PRE", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[2];
    if (f.read(data, sizeof(data)) == sizeof(data)) pass.start = data[0] + (data[1] << 8);
    f.close();
  }
  if (pass.start >= epub->getSpineItemsCount()) pass.start = 0;
  pass.next = 0;
  pass.epub = std::move(epub);
  loadSkipped();
  return true;
}

// Starts the next unbuilt chapter, from the reader's place onward. False when this step found none.
bool startNext(GfxRenderer& renderer, const ReaderRenderSpec& spec) {
  for (int checks = 0; checks < CHECKS_PER_STEP; ++checks) {
    if (!pass.epub) {
      const int count = static_cast<int>(RECENT_BOOKS.getBooks().size());
      if (pass.book >= count || pass.book >= MAX_BOOKS) {
        pass.done = true;
        LOG_INF("PRE", "Built %d chapters in %lu ms", pass.built, millis() - pass.startedAt);
        return false;
      }
      if (!openBook()) ++pass.book;
      continue;
    }
    const int count = pass.epub->getSpineItemsCount();
    if (pass.next >= count) {
      closeBook();
      ++pass.book;
      continue;
    }
    const int spine = (pass.start + pass.next++) % count;
    if (isSkipped(spine)) continue;
    auto section = makeUniqueNoThrow<Section>(pass.epub, spine, renderer);
    if (!section) return false;
    if (section->loadSectionFile(spec) && !section->isPartial()) continue;
    markTrying(spine);
    if (!section->startBuild(spec)) {
      Storage.remove(tryingPath().c_str());
      skip(spine);
      continue;
    }
    pass.section = std::move(section);
    pass.spine = spine;
    return true;
  }
  return false;
}

}  // namespace

bool tick(GfxRenderer& renderer) {
  if (pass.done) return false;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || contactClosed()) return true;
  if (ESP.getFreeHeap() < MIN_FREE_HEAP || ESP.getMaxAllocHeap() < MIN_MAX_ALLOC) return false;
  if (pass.startedAt == 0) pass.startedAt = millis();

  // The reader's fonts and orientation, so the layout is the reader's own.
  sdFontSystem.ensureLoaded(renderer);
  const GfxRenderer::Orientation shown = renderer.getOrientation();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  const ReaderRenderSpec spec = EpubReaderUtils::pageSpec(renderer);

  if (pass.section || startNext(renderer, spec)) {
    ImageBlock::setExtractor(pass.epub.get(), [](void* ctx, const char* src, const char* dest) {
      return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
    });
    const uint32_t loansBefore = renderer.frameBufferLoanCount();
    const bool ok = pass.section->buildSomeMore(PAGES_PER_STEP, &contactClosed);
    ImageBlock::setExtractor(nullptr, nullptr);
    if (!ok || pass.section->isBuildComplete()) {
      pass.built += ok ? 1 : 0;
      pass.section.reset();
      Storage.remove(tryingPath().c_str());
      if (!ok) skip(pass.spine);
    }
    // A loan hands the framebuffer back white: draw the screen again.
    if (renderer.frameBufferLoanCount() != loansBefore) activityManager.requestUpdate();
  }
  renderer.setOrientation(shown);
  return !pass.done;
}

void stop() {
  if (pass.epub || pass.section) {
    RenderLock lock;
    closeBook();
  }
  // The pass goes on with this book at the next rest; a finished one stays finished.
}

void restart() {
  stop();
  pass = Pass{};
}

}  // namespace prebuild
