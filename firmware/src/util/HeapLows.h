#pragma once

#include <ArduinoJson.h>

// Which stage ran when free memory reached a new low. The lowest free heap since power-on only
// ever falls, so a mark that finds it lower than the mark before names what just ran. The USB
// `heap` command reports the last few (docs/PROTOCOL.md).
namespace x4::heap {

// `stage` is a string literal, at most 12 characters: "net:weather", "usb:put", "today".
// Callable from any task.
void mark(const char* stage);
// {"free","min","block","low":[{"at":"net:weather","min":13788}]}, the lows oldest first.
void report(JsonObject out);

}  // namespace x4::heap
