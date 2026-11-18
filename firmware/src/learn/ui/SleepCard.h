#pragma once

#include <GfxRenderer.h>
#include <HalDisplay.h>

namespace x4::ui {

// The "Today" sleep screen: under the day ahead, the question of the next article that asks
// one, alone, to sleep on; else one card front from the next set, else a highlight. Draws and displays it with `refresh`; false when there is nothing
// to show (the caller then shows its default screen).
bool renderSleepCard(GfxRenderer& renderer, HalDisplay::RefreshMode refresh);

}  // namespace x4::ui
