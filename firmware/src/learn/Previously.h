#pragma once

#include <cstddef>

// "Previously": one screen of where the reader was in a book, written by Claude at sync into
// /x4/previously.jsonl, shown when the book opens after days away.
namespace x4::previously {

// Away: last read this many days ago, or this many seconds by a clock known both times.
inline constexpr unsigned AWAY_DAYS = 3;
inline constexpr long long AWAY_SECONDS = 2LL * 24 * 60 * 60;
// A recap still fits the book while its progress is this close, in percent points.
inline constexpr int PERCENT_SLACK = 1;
inline constexpr size_t TEXT_MAX = 600;  // as `x4` checks it

// Copies the recap of `bookPath` into `out` when it is due: the book was away and is still at
// `percent`, the progress the recap was written at. Allocates a line buffer and a parse document
// only once the book was away.
bool load(const char* bookPath, int percent, char (&out)[TEXT_MAX + 1]);

}  // namespace x4::previously
