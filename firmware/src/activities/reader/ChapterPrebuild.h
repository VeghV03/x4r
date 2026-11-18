#pragma once

class GfxRenderer;

// Lays out the unbuilt chapters of the recent books while the X4 rests on Home on USB power, with
// the reader's own code and layout, so a chapter opens from its cache.
namespace prebuild {

// One small step, under the render lock. True while there is work, so the loop stays fast.
bool tick(GfxRenderer& renderer);
// Ends the current step's chapter (kept as a partial) and closes the book. Before input or USB
// file work acts, so nothing else opens a chapter this is writing. A finished pass stays finished.
void stop();
// stop(), and the next rest starts a new pass: after reading or a USB session, which may leave
// new chapters to build.
void restart();

}  // namespace prebuild
