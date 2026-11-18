#pragma once

#include <GfxRenderer.h>

#include <cstdint>

#include "net/Agenda.h"

namespace x4::ui {

// One day of the agenda in the UI face, from `top`: a line with `heading` (may
// be null) and the weather, then up to `maxEvents` events, "09:30  Title".
// With `after` set, events already over are left out. Returns the bottom, or
// `top` when there was nothing to draw.
int drawAgenda(const GfxRenderer& renderer, int left, int width, int top, const net::Agenda& agenda,
               const net::TzRule& zone, int32_t day, int64_t after, uint8_t maxEvents, const char* heading);

}  // namespace x4::ui
