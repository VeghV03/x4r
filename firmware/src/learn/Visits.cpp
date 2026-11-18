#include "Visits.h"

#include <cstdio>
#include <cstring>

#include "Journal.h"
#include "Learn.h"
#include "LearnTypes.h"

namespace x4::visits {

namespace {

char issue[11] = {};
char open[ID_MAX + 1] = {};
uint32_t startedMs = 0;
uint32_t lastPressMs = 0;

}  // namespace

void noteIssue(const char* date) { snprintf(issue, sizeof(issue), "%s", date ? date : ""); }

void enter(const char* section) {
  if (!section || !*section) return;
  if (std::strcmp(open, section) == 0) return;
  leave();
  snprintf(open, sizeof(open), "%s", section);
  startedMs = lastPressMs = learn::platformMillis();
}

void press() {
  if (open[0]) lastPressMs = learn::platformMillis();
}

void leave() {
  if (!open[0]) return;
  writeVisit(open, issue, lastPressMs - startedMs, learn::today(), learn::wallClock());
  open[0] = '\0';
}

}  // namespace x4::visits
