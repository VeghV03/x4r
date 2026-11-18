#pragma once

#include <cstdint>

// Visits (DATA.md "Visits"): one /x4/visits.jsonl line each time a section's screen, or Play's,
// is left. A stay counts up to the last press on it (D24). Main task only.
namespace x4::visits {

// The issue on the front page, named on the visits that follow.
void noteIssue(const char* date);
// A screen of `section` (a section's id, or "play") is on. Entering the section already open
// goes on with its visit, as Football's tables do from its pieces; another one ends it first.
void enter(const char* section);
// A press on that screen.
void press();
// The screen is left: its visit is written.
void leave();

}  // namespace x4::visits
