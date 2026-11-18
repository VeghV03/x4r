#pragma once

#include <ArduinoJson.h>

#include <cstdint>
#include <memory>

#include "Agenda.h"
#include "Football.h"

// Online on its own: on Wi-Fi the X4 sets its clock from NTP, takes the issue
// the Mac staged on the LAN, and fetches the calendars and the forecast in
// /x4/net.json into /x4/agenda.json, then turns the radio off. The fetch runs in a background task while the X4 rests on
// Today, and once more before sleep when the agenda is old.
namespace x4::net {

inline constexpr char CONFIG_PATH[] = "/x4/net.json";
inline constexpr char AGENDA_PATH[] = "/x4/agenda.json";

// At boot, before the heap fills: when a Wi-Fi network is saved, starts the network
// stack and event loop, which then live as long as the X4 is on.
void begin();
// From Today's loop. `resting`: no press for a while. Starts a fetch when one
// is due; takes in a finished one. `makeRoom(ctx)` is called when a fetch is wanted, before
// the heap is asked whether it has room: the screen frees what it can load again.
void poll(bool resting, void (*makeRoom)(void* ctx) = nullptr, void* ctx = nullptr);
// From the main loop on every screen: a fetch that ended away from Today keeps its agenda and
// clock, and leaves its reload for Today.
void pollAway();
// Stops a running fetch: closes its connection and waits for it, at most a few
// seconds; its finished parts (the clock, a calendar) are kept.
void stop();
// Before the sleep screen: fetches when the agenda is over 30 minutes old, and
// waits for that or a running fetch, at most about 25 s.
void refreshBeforeSleep();
bool fetching();
// Changes when a fetch saves a new agenda, so a screen knows to redraw.
uint32_t generation();

// The zone in /x4/net.json, else the host's last offset.
TzRule zone();
std::unique_ptr<Agenda> loadAgenda();
// /x4/football.json; with `tables` false only its matches.
std::unique_ptr<Football> loadFootball(bool tables);

// The USB `net` command (docs/PROTOCOL.md): add or forget a Wi-Fi network,
// start a fetch, and report. Returns an error message, or nullptr.
const char* command(JsonObjectConst args, JsonObject out);
void helloFields(JsonObject out);

}  // namespace x4::net
