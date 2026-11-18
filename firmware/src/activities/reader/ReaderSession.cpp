#include "ReaderSession.h"

#include <algorithm>

namespace {
constexpr uint32_t MIN_DWELL_SECONDS = 2;
constexpr uint32_t MAX_DWELL_SECONDS = 1800;
// Longer on one page is taken as idle, not reading.
constexpr uint32_t MAX_PAGE_MS = 5UL * 60UL * 1000UL;
// Shorter on one page is a flip, not reading.
constexpr uint32_t MIN_PACE_PAGE_MS = 3000;
constexpr uint32_t MIN_PACE_PAGES = 5;
}  // namespace

uint16_t ReaderSession::clampProgress(const int progressBp) {
  return static_cast<uint16_t>(std::clamp(progressBp, 0, 10000));
}

void ReaderSession::noteTurn(const bool forward, const bool succeeded) {
  pendingForward = forward && succeeded;
  pendingTurn = succeeded;
}

void ReaderSession::onRenderComplete(const uint32_t monotonicMs, const int64_t trustedEpoch, const int progressBp) {
  const uint16_t progress = clampProgress(progressBp);
  if (pendingTurn || !hasPage) {
    if (pendingTurn && hasPage) {
      const uint32_t shownMs = monotonicMs - pageShownMs;
      readTotalMs += std::min(shownMs, MAX_PAGE_MS);
      if (pendingForward && forwardPages < UINT16_MAX) ++forwardPages;
      if (pendingForward && shownMs >= MIN_PACE_PAGE_MS && shownMs <= MAX_PAGE_MS && paceCount < UINT16_MAX) {
        paceTotalMs += shownMs;
        ++paceCount;
      }
    }
    pageShownMs = monotonicMs;
    hasPage = true;
  }
  if (pendingForward && hasAnchor && anchorEpoch > 0) {
    const uint32_t elapsedSeconds = (monotonicMs - anchorMs) / 1000;
    if (elapsedSeconds >= MIN_DWELL_SECONDS) {
      const uint32_t contribution = std::min(elapsedSeconds, MAX_DWELL_SECONDS);
      if (duration == 0) {
        startEpoch = anchorEpoch;
        startProgress = anchorProgress;
        endProgress = anchorProgress;
      }
      duration += contribution;
      endProgress = std::max(endProgress, progress);
    }
  }

  anchorMs = monotonicMs;
  anchorEpoch = trustedEpoch;
  anchorProgress = progress;
  hasAnchor = true;
  pendingForward = false;
  pendingTurn = false;
}

void ReaderSession::reset() {
  anchorMs = 0;
  pageShownMs = 0;
  readTotalMs = 0;
  forwardPages = 0;
  paceTotalMs = 0;
  paceCount = 0;
  hasPage = false;
  pendingTurn = false;
  anchorEpoch = 0;
  startEpoch = 0;
  duration = 0;
  anchorProgress = 0;
  startProgress = 0;
  endProgress = 0;
  hasAnchor = false;
  pendingForward = false;
}

int minutesLeft(const uint32_t ms, const uint32_t pages, const int pagesLeft) {
  if (pages < MIN_PACE_PAGES || pagesLeft <= 0) return -1;
  const uint64_t leftMs = static_cast<uint64_t>(ms) * static_cast<uint32_t>(pagesLeft) / pages;
  return static_cast<int>((leftMs + 59999) / 60000);
}
