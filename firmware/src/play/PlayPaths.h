#pragma once

#include "learn/LearnPaths.h"

// SD card paths of the Play part of docs/DATA.md.
namespace x4::paths {

inline constexpr char PLAY_LOG[] = "/x4/play.jsonl";
// Device-private: one record per game; play-<game>.bin holds a game in progress.
inline constexpr char PLAY_STATE[] = "/x4/state/play.bin";
// Host-written: chess tactics, one line per puzzle, sorted by rating.
inline constexpr char CHESS_PUZZLES[] = "/x4/chess/puzzles.txt";

}  // namespace x4::paths
