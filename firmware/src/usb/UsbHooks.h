#pragma once

#include <ArduinoJson.h>

#include <cstdint>

// Called once when a USB session that wrote or removed files ends. The weak default
// marks the SD font registry dirty and re-enters Home if Home is showing.
void x4OnHostDataChanged();

// Called right after each put, rm or mv that changed `path` (mv: once per side),
// before the session ends. The weak default does nothing.
void x4OnHostWrote(const char* path);

// Called before a get reads a file, so derived files the device writes lazily are current.
// The weak default does nothing.
void x4BeforeHostRead();

// Adds fields to the hello reply (the learning layer adds "day", "issue" and "retention").
// The weak default adds nothing.
void x4HelloFields(JsonObject out);

// Adds fields to the `screen` reply (the learning layer adds "step" while the daily stream is
// shown). The weak default adds nothing.
void x4ScreenFields(JsonObject out);

// The `net` command: Wi-Fi networks, an online fetch, and their state. Fills `out`
// and returns nullptr, or returns the message of a bad_request. The weak default
// answers that this build has no Wi-Fi.
const char* x4NetCommand(JsonObjectConst args, JsonObject out);

// Called after the host set the wall clock (hello or time). `tzKnown` is false
// while the host has sent no offset since power-on; `tzMinutes` is then 0 and
// the receiver keeps the offset it had. The weak default does nothing.
void x4OnHostTime(int64_t now, int16_t tzMinutes, bool tzKnown);

// True when the host has set the wall clock since power-on (hello or time);
// tzMinutes then holds the host's offset east of UTC, or is left as passed in
// when the host sent none. Survives deep sleep.
bool x4HostClock(int16_t& tzMinutes);
