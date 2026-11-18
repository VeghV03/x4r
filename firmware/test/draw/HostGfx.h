#pragma once

#include <GfxRenderer.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// The firmware's own GfxRenderer on the Mac, drawing into an X4 panel held in memory.
//
//   HostGfx gfx;                                    // upright, 480 x 800, UI fonts registered
//   const int body = gfx.loadSdFamily("build/fonts", "Literata", 12);
//   gfx.render([&](bool) { gfx.renderer().drawText(body, 20, 40, "Árvíztűrő"); });
//   gfx.savePgm("page.pgm");
class HostGfx {
 public:
  HostGfx();
  ~HostGfx();
  HostGfx(const HostGfx&) = delete;
  HostGfx& operator=(const HostGfx&) = delete;

  // In Portrait, as the UI draws. The built-in faces are registered under the ids of
  // src/fontIds.h, as setupDisplayAndFonts() in src/main.cpp does.
  GfxRenderer& renderer();

  // Loads <fontsFolder>/<family>/<family>_<pointSize>.cpfont, the card's /fonts/<family>/, through
  // the firmware's registry and manager, and returns the font id to draw with; 0 when the family
  // or that size is not there. Every family has to come from the one folder.
  int loadSdFamily(const std::string& fontsFolder, const std::string& family, uint8_t pointSize);

  // Draws one screen the way an activity's render() does: a scan pass that loads the glyphs of
  // SD fonts (with their kerning and ligatures), the black-and-white pass, the panel update, and
  // with antiAlias the two gray passes that smooth 2-bit fonts. `draw` runs once per pass and
  // gets `gray` as a screen's drawScreen(gray) does: false in the black-and-white pass, which
  // starts on a white frame; true in the scan and gray passes, where it should draw only black
  // text on white (x4::ui::GrayFn): white ink drawn there marks its pixels gray.
  using Draw = std::function<void(bool gray)>;
  void render(const Draw& draw, bool antiAlias = false);

  // What the panel shows after the last render(), upright, as a 480 x 800 binary PGM:
  // 0 black, 85 dark gray, 170 light gray, 255 white.
  bool savePgm(const std::string& path) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
