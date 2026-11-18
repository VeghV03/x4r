#pragma once

#include <cstddef>
#include <cstdint>

// Threads: one screen of a running story so far, written by Claude at sync into
// /x4/threads.jsonl. A brief line that names its thread shows it before its article, once for
// each version of the text (SPEC: Feed).
namespace x4::threads {

inline constexpr size_t TITLE_MAX = 200;
inline constexpr size_t BODY_MAX = 600;  // one screen, as `x4` checks it
// Threads whose shown version is remembered; the one shown longest ago goes first.
inline constexpr uint8_t REMEMBERED = 32;

struct Thread {
  char title[TITLE_MAX + 1] = {};
  char body[BODY_MAX + 1] = {};
};

// Copies thread `id` into `out` when it is due: it is in the file, and this version of its
// text was not shown yet. Allocates a line buffer while it reads.
bool load(const char* id, Thread& out);
// This version of thread `id` is on screen: it is not due again until its text changes.
void markShown(const char* id, const Thread& thread);

}  // namespace x4::threads
