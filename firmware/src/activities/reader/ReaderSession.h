#pragma once

#include <cstdint>

// Fixed-scalar active-reading summary. The owner reports turn outcomes before
// render and completed pages after render; no allocation or I/O occurs here.
class ReaderSession {
 public:
  void noteTurn(bool forward, bool succeeded);
  void onRenderComplete(uint32_t monotonicMs, int64_t trustedEpoch, int progressBp);
  void reset();

  bool isEmitWorthy() const { return startEpoch > 0 && duration >= 10 && endTime() > startEpoch; }
  int64_t startTime() const { return startEpoch; }
  int64_t endTime() const { return startEpoch > 0 ? startEpoch + duration : 0; }
  uint32_t durationSeconds() const { return duration; }
  uint16_t startProgressBp() const { return startProgress; }
  uint16_t endProgressBp() const { return endProgress; }
  // Time on pages left by a page turn, each capped at five minutes, and the
  // forward turns. Unlike the session above these need no trusted clock.
  uint32_t readMs() const { return readTotalMs; }
  uint16_t pagesForward() const { return forwardPages; }
  // Pages read at a reading pace: left by a forward turn after 3 s to 5 min on them, so a flip
  // or a pause does not count. Their time is paceMs().
  uint32_t paceMs() const { return paceTotalMs; }
  uint16_t pacePages() const { return paceCount; }

 private:
  static uint16_t clampProgress(int progressBp);

  uint32_t anchorMs = 0;
  // Shown time of the current page: only page turns move it, so a menu or
  // refresh in between does not drop the time already spent on the page.
  uint32_t pageShownMs = 0;
  int64_t anchorEpoch = 0;
  int64_t startEpoch = 0;
  uint32_t duration = 0;
  uint32_t readTotalMs = 0;
  uint32_t paceTotalMs = 0;
  uint16_t paceCount = 0;
  uint16_t anchorProgress = 0;
  uint16_t startProgress = 0;
  uint16_t endProgress = 0;
  uint16_t forwardPages = 0;
  bool hasAnchor = false;
  bool hasPage = false;
  bool pendingForward = false;
  bool pendingTurn = false;
};

// Minutes to read `pagesLeft` pages at `ms` per `pages`, rounded up; -1 until `pages` is enough
// to tell a pace.
int minutesLeft(uint32_t ms, uint32_t pages, int pagesLeft);
