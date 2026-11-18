#include "HostGfx.h"

#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryManager.h>
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>
#include <builtinFonts/all.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <map>
#include <utility>
#include <vector>

#include "src/fontIds.h"

// ---- the platform under the drawing code -----------------------------------------

unsigned long micros() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return static_cast<unsigned long>(duration_cast<microseconds>(steady_clock::now() - start).count());
}

unsigned long millis() { return micros() / 1000; }

void hostLog(const int level, const char* tag, const char* origin, const char* format, ...) {
  static const int shown = [] {
    const char* wanted = getenv("X4_HOST_LOG");
    return wanted ? atoi(wanted) : 0;
  }();
  if (level > shown) return;
  fprintf(stderr, "[%s] [%s] ", tag, origin);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
}

// uzlib's checksummed inflate names these, and the firmware never calls it (InflateReaderTest
// does the same).
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t) { abort(); }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t) { abort(); }

// The renderer registers its caches as things to drop when the heap runs low. The Mac never
// runs low, so nothing is kept and nothing is dropped.
namespace freeink {
MemoryManager& MemoryManager::instance() {
  static MemoryManager manager;
  return manager;
}
int MemoryManager::registerSink(const CacheSink&) { return 1; }
size_t MemoryManager::freeBytes(MemPool) const { return ESP.getFreeHeap(); }
}  // namespace freeink

// ---- the panel -------------------------------------------------------------------

EInkDisplay* EInkDisplay::latest = nullptr;

EInkDisplay::EInkDisplay() {
  memset(frameBuffer, 0xFF, sizeof(frameBuffer));
  memset(lsbPlane, 0, sizeof(lsbPlane));
  memset(msbPlane, 0, sizeof(msbPlane));
  memset(glass, White, sizeof(glass));
  latest = this;
}

void EInkDisplay::show(const uint8_t* frame) {
  for (uint32_t i = 0; i < sizeof(glass); i++) {
    const bool white = ((frame[i >> 3] >> (7 - (i & 7))) & 1) != 0;
    glass[i] = white != inverted ? White : Black;
  }
}

void EInkDisplay::showGray() {
  // GrayscaleCapabilities.h: both planes set is dark gray, the MSB alone is light gray, neither
  // leaves the pixel as the black-and-white update drew it. The firmware never sets the LSB alone.
  for (uint32_t i = 0; i < sizeof(glass); i++) {
    const uint8_t mask = 0x80u >> (i & 7);
    const bool lsb = (lsbPlane[i >> 3] & mask) != 0;
    const bool msb = (msbPlane[i >> 3] & mask) != 0;
    if (msb) glass[i] = lsb ? DarkGray : LightGray;
  }
  memset(lsbPlane, 0, sizeof(lsbPlane));
  memset(msbPlane, 0, sizeof(msbPlane));
}

// ---- HalDisplay over that panel -----------------------------------------------------

HalDisplay::RefreshHook HalDisplay::refreshHook = nullptr;

HalDisplay::HalDisplay() {}
HalDisplay::~HalDisplay() {}

HalDisplay::Controller HalDisplay::getController() const { return BoardConfig::ACTIVE.displayController; }

void HalDisplay::begin(bool) {}

// The X4's SSD1677 in overlay mode. Its absolute mode is not played here.
HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(const GrayscaleMode mode) const {
  if (mode != GrayscaleMode::Overlay) return {};
  return {GrayscaleEncoding::OverlayMasks, GrayscaleBase::Separate, true, true, false};
}

void HalDisplay::clearScreen(const uint8_t color) const {
  memset(const_cast<uint8_t*>(einkDisplay.frameBuffer), color, sizeof(einkDisplay.frameBuffer));
}

namespace {
void blit(uint8_t* frame, const uint8_t* image, const uint16_t x, const uint16_t y, const uint16_t w, const uint16_t h,
          const bool transparent) {
  const uint16_t imageWidthBytes = (w + 7) / 8;
  for (uint16_t row = 0; row < h && y + row < EInkDisplay::DISPLAY_HEIGHT; row++) {
    for (uint16_t col = 0; col < w && x + col < EInkDisplay::DISPLAY_WIDTH; col++) {
      const bool white = ((image[row * imageWidthBytes + col / 8] >> (7 - (col & 7))) & 1) != 0;
      uint8_t& target = frame[(y + row) * EInkDisplay::DISPLAY_WIDTH_BYTES + (x + col) / 8];
      const uint8_t mask = 0x80u >> ((x + col) & 7);
      if (!white) {
        target &= static_cast<uint8_t>(~mask);
      } else if (!transparent) {
        target |= mask;
      }
    }
  }
}
}  // namespace

void HalDisplay::drawImage(const uint8_t* imageData, const uint16_t x, const uint16_t y, const uint16_t w,
                           const uint16_t h, bool) const {
  blit(const_cast<uint8_t*>(einkDisplay.frameBuffer), imageData, x, y, w, h, false);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, const uint16_t x, const uint16_t y, const uint16_t w,
                                      const uint16_t h, bool) const {
  blit(const_cast<uint8_t*>(einkDisplay.frameBuffer), imageData, x, y, w, h, true);
}

void HalDisplay::displayBuffer(RefreshMode, bool) { einkDisplay.show(einkDisplay.frameBuffer); }
void HalDisplay::displayBufferAsync(RefreshMode) { einkDisplay.show(einkDisplay.frameBuffer); }
void HalDisplay::refreshDisplay(RefreshMode, bool) { einkDisplay.show(einkDisplay.frameBuffer); }
void HalDisplay::waitRefreshComplete() {}
bool HalDisplay::supportsAsyncRefresh() const { return true; }
bool HalDisplay::supportsAsyncGrayscaleBase() const { return grayscaleCapabilities().asyncBase; }
void HalDisplay::powerOffWhenIdle(uint32_t) {}

void HalDisplay::setInverted(const bool inverted) { einkDisplay.inverted = inverted; }
bool HalDisplay::toggleInverted() { return einkDisplay.inverted = !einkDisplay.inverted; }
bool HalDisplay::isInverted() const { return einkDisplay.inverted; }

void HalDisplay::deepSleep() {}

uint8_t* HalDisplay::getFrameBuffer() const { return const_cast<uint8_t*>(einkDisplay.frameBuffer); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) {
  if (einkDisplay.lent) return nullptr;
  einkDisplay.lent = true;
  if (sizeOut) *sizeOut = EInkDisplay::BUFFER_SIZE;
  return einkDisplay.frameBuffer;
}

void HalDisplay::returnFrameBufferStorage() {
  memset(einkDisplay.frameBuffer, 0xFF, sizeof(einkDisplay.frameBuffer));
  einkDisplay.lent = false;
}

void HalDisplay::preconditionGrayscale() {}
void HalDisplay::preconditionGrayscale(uint16_t, uint16_t, uint16_t, uint16_t) {}

void HalDisplay::displayGrayscaleBase(RefreshMode, bool) { einkDisplay.show(einkDisplay.frameBuffer); }

bool HalDisplay::displayGrayscaleBase(const GrayscaleMode mode, RefreshMode, bool) {
  if (mode != GrayscaleMode::Overlay) return false;
  einkDisplay.show(einkDisplay.frameBuffer);
  return true;
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  copyGrayscaleLsbBuffers(lsbBuffer);
  copyGrayscaleMsbBuffers(msbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  memcpy(einkDisplay.lsbPlane, lsbBuffer, sizeof(einkDisplay.lsbPlane));
}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  memcpy(einkDisplay.msbPlane, msbBuffer, sizeof(einkDisplay.msbPlane));
}

// Only resyncs the controller's memory on the X4; the glass does not change.
void HalDisplay::cleanupGrayscaleBuffers(const uint8_t*) {}

void HalDisplay::displayGrayBuffer(bool) { einkDisplay.showGray(); }

void HalDisplay::writeGrayscalePlaneStrip(const bool lsbPlane, const uint8_t* rows, const uint16_t yStart,
                                          const uint16_t numRows) {
  uint8_t* plane = lsbPlane ? einkDisplay.lsbPlane : einkDisplay.msbPlane;
  memcpy(plane + static_cast<size_t>(yStart) * EInkDisplay::DISPLAY_WIDTH_BYTES, rows,
         static_cast<size_t>(numRows) * EInkDisplay::DISPLAY_WIDTH_BYTES);
}

bool HalDisplay::supportsStripGrayscale() const { return grayscaleCapabilities().stripUploads; }
bool HalDisplay::combinesGrayscaleBase() const { return false; }

uint16_t HalDisplay::getDisplayWidth() const { return EInkDisplay::DISPLAY_WIDTH; }
uint16_t HalDisplay::getDisplayHeight() const { return EInkDisplay::DISPLAY_HEIGHT; }
uint16_t HalDisplay::getDisplayWidthBytes() const { return EInkDisplay::DISPLAY_WIDTH_BYTES; }
uint32_t HalDisplay::getBufferSize() const { return EInkDisplay::BUFFER_SIZE; }

// ---- the built-in faces, as src/main.cpp defines them ------------------------------

namespace {

const EpdFont notosans12RegularFont(&notosans_12_regular);
const EpdFont notosans12BoldFont(&notosans_12_bold);
const EpdFont notosans12ItalicFont(&notosans_12_italic);
const EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
const EpdFont notosans14RegularFont(&notosans_14_regular);
const EpdFont notosans14BoldFont(&notosans_14_bold);
const EpdFont notosans14ItalicFont(&notosans_14_italic);
const EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
const EpdFont notosans16RegularFont(&notosans_16_regular);
const EpdFont notosans16BoldFont(&notosans_16_bold);
const EpdFont notosans16ItalicFont(&notosans_16_italic);
const EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
const EpdFont notosans18RegularFont(&notosans_18_regular);
const EpdFont notosans18BoldFont(&notosans_18_bold);
const EpdFont notosans18ItalicFont(&notosans_18_italic);
const EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
const EpdFont smallFont(&ubuntu_8_regular);
const EpdFont bigDigitsFont(&bigdigits_bold);
const EpdFont ui10RegularFont(&ubuntu_10_regular);
const EpdFont ui10BoldFont(&ubuntu_10_bold);
const EpdFont ui12RegularFont(&ubuntu_12_regular);
const EpdFont ui12BoldFont(&ubuntu_12_bold);

void insertBuiltinFonts(GfxRenderer& renderer) {
  renderer.insertFont(NOTOSANS_14_FONT_ID, EpdFontFamily(14, &notosans14RegularFont, &notosans14BoldFont,
                                                         &notosans14ItalicFont, &notosans14BoldItalicFont));
  renderer.insertFont(NOTOSANS_12_FONT_ID, EpdFontFamily(12, &notosans12RegularFont, &notosans12BoldFont,
                                                         &notosans12ItalicFont, &notosans12BoldItalicFont));
  renderer.insertFont(NOTOSANS_16_FONT_ID, EpdFontFamily(16, &notosans16RegularFont, &notosans16BoldFont,
                                                         &notosans16ItalicFont, &notosans16BoldItalicFont));
  renderer.insertFont(NOTOSANS_18_FONT_ID, EpdFontFamily(18, &notosans18RegularFont, &notosans18BoldFont,
                                                         &notosans18ItalicFont, &notosans18BoldItalicFont));
  renderer.insertFont(UI_10_FONT_ID, EpdFontFamily(10, &ui10RegularFont, &ui10BoldFont));
  renderer.insertFont(UI_12_FONT_ID, EpdFontFamily(12, &ui12RegularFont, &ui12BoldFont));
  renderer.insertFont(SMALL_FONT_ID, EpdFontFamily(8, &smallFont));
  renderer.insertFont(BIG_DIGITS_FONT_ID, EpdFontFamily(64, &bigDigitsFont));
}

std::string absolutePath(const std::string& path) {
  char resolved[PATH_MAX];
  return realpath(path.c_str(), resolved) ? resolved : "";
}

}  // namespace

// ---- HostGfx ----------------------------------------------------------------------

struct HostGfx::Impl {
  HalDisplay display;
  EInkDisplay* panel = EInkDisplay::latest;  // the one `display` just built
  GfxRenderer renderer{display};
  FontDecompressor decompressor;
  FontCacheManager fontCache{renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts()};
  SdCardFontRegistry registry;
  SdCardFontManager sdFonts;
  std::string fontsFolder;
  std::map<std::pair<std::string, uint8_t>, int> loaded;
};

HostGfx::HostGfx() : impl(std::make_unique<Impl>()) {
  impl->display.begin();
  impl->renderer.begin();
  if (!impl->decompressor.init()) LOG_ERR("HOST", "Font decompressor init failed");
  impl->fontCache.setFontDecompressor(&impl->decompressor);
  impl->renderer.setFontCacheManager(&impl->fontCache);
  insertBuiltinFonts(impl->renderer);
  impl->renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

HostGfx::~HostGfx() = default;

GfxRenderer& HostGfx::renderer() { return impl->renderer; }

int HostGfx::loadSdFamily(const std::string& fontsFolder, const std::string& family, const uint8_t pointSize) {
  const std::string folder = absolutePath(fontsFolder);
  if (folder.empty()) {
    LOG_ERR("HOST", "No fonts folder at %s", fontsFolder.c_str());
    return 0;
  }
  if (!impl->fontsFolder.empty() && impl->fontsFolder != folder) {
    LOG_ERR("HOST", "Fonts already come from %s", impl->fontsFolder.c_str());
    return 0;
  }
  const auto key = std::make_pair(family, pointSize);
  if (const auto it = impl->loaded.find(key); it != impl->loaded.end()) return it->second;

  // The folder stands for the card's /fonts, so the firmware finds the family as it does there.
  impl->fontsFolder = folder;
  Storage.mount(SdCardFontRegistry::FONTS_DIR_VISIBLE, folder);
  impl->registry.discover();
  const SdCardFontFamilyInfo* info = impl->registry.findFamily(family);
  if (!info) {
    LOG_ERR("HOST", "No font family %s in %s", family.c_str(), folder.c_str());
    return 0;
  }
  // Font id 0 lets the manager derive the id from the file's content, as it does for the reader font.
  const int fontId = impl->sdFonts.loadFamilyExtraSize(*info, impl->renderer, pointSize, 0);
  if (fontId == 0) {
    LOG_ERR("HOST", "%s has no loadable size %u", family.c_str(), pointSize);
    return 0;
  }
  impl->loaded[key] = fontId;
  return fontId;
}

void HostGfx::render(const Draw& draw, const bool antiAlias) {
  GfxRenderer& renderer = impl->renderer;
  {
    // StreamActivity::render(): scan first so an SD font loads its glyphs in one batch.
    auto scope = impl->fontCache.createPrewarmScope();
    draw(true);
    scope.endScanAndPrewarm();
  }
  renderer.clearScreen();
  draw(false);
  if (!antiAlias) {
    renderer.displayBuffer();
    return;
  }

  // x4::ui::displayWithAntiAliasing() on the X4: the base, then each gray plane sent in bands.
  renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  constexpr int STRIP_ROWS = 80;
  const int height = renderer.getDisplayHeight();
  std::vector<uint8_t> scratch(static_cast<size_t>(renderer.getDisplayWidthBytes()) * STRIP_ROWS);
  for (const bool lsb : {true, false}) {
    renderer.setRenderMode(lsb ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
    for (int y = 0; y < height; y += STRIP_ROWS) {
      const int rows = std::min(STRIP_ROWS, height - y);
      renderer.beginStripTarget(scratch.data(), y, rows);
      renderer.clearScreen(0x00);
      draw(true);
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsb, scratch.data(), y, rows);
    }
  }
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();
}

bool HostGfx::savePgm(const std::string& path) const {
  // Portrait puts logical (x, y) at panel column y, panel row 479 - x (rotateCoordinates).
  constexpr int WIDTH = EInkDisplay::DISPLAY_HEIGHT;
  constexpr int HEIGHT = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint8_t SHADES[] = {0, 85, 170, 255};
  std::vector<uint8_t> pixels(static_cast<size_t>(WIDTH) * HEIGHT);
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      pixels[static_cast<size_t>(y) * WIDTH + x] =
          SHADES[impl->panel->glass[static_cast<size_t>(WIDTH - 1 - x) * EInkDisplay::DISPLAY_WIDTH + y]];
    }
  }
  FILE* out = fopen(path.c_str(), "wb");
  if (!out) return false;
  fprintf(out, "P5\n%d %d\n255\n", WIDTH, HEIGHT);
  const bool written = fwrite(pixels.data(), 1, pixels.size(), out) == pixels.size();
  return fclose(out) == 0 && written;
}
