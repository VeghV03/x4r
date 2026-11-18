#pragma once

// SD card paths of docs/DATA.md.
namespace x4::paths {

inline constexpr char ROOT[] = "/x4";
inline constexpr char DECKS[] = "/x4/decks";
inline constexpr char DECK_EXT[] = ".jsonl";
inline constexpr char STREAM[] = "/x4/stream.jsonl";
inline constexpr char HIGHLIGHTS[] = "/x4/highlights.jsonl";
inline constexpr char REVIEWS[] = "/x4/reviews.jsonl";
inline constexpr char SEEN_LOG[] = "/x4/seen.jsonl";
inline constexpr char STATS[] = "/x4/stats.json";
inline constexpr char STATS_TMP[] = "/x4/stats.json.tmp";
inline constexpr char READ_LIST[] = "/x4/read.jsonl";
inline constexpr char READ_LOG[] = "/x4/read-log.jsonl";
inline constexpr char LOOKUPS[] = "/x4/lookups.jsonl";
inline constexpr char PREVIOUSLY[] = "/x4/previously.jsonl";
inline constexpr char THREADS[] = "/x4/threads.jsonl";
// The version of each thread last shown.
inline constexpr char THREADS_SHOWN[] = "/x4/state/threads.bin";
inline constexpr char THREADS_SHOWN_TMP[] = "/x4/state/threads.tmp";
inline constexpr char NEXT[] = "/x4/next.jsonl";
inline constexpr char READ_BOOKS[] = "/books/read/";
// One line per hold of Confirm on a piece: kept, or let go.
inline constexpr char KEPT_LOG[] = "/x4/kept.jsonl";
inline constexpr char VISITS[] = "/x4/visits.jsonl";
// Football's results, fixtures and tables, as the Mac writes them (net/Football.h).
inline constexpr char FOOTBALL_JSON[] = "/x4/football.json";

// Device-private state.
inline constexpr char STATE[] = "/x4/state";
inline constexpr char LEARN_STATE[] = "/x4/state/learn.bin";
inline constexpr char SEEN[] = "/x4/state/seen.bin";
inline constexpr char SEEN_TMP[] = "/x4/state/seen.bin.tmp";
// The day the last bite was shown (Learn: today()), four bytes.
inline constexpr char BITE_DAY[] = "/x4/state/bite.bin";
inline constexpr char BITE_TMP[] = "/x4/state/bite.bin.tmp";
inline constexpr char BOOKS[] = "/x4/state/books.bin";
inline constexpr char BOOKS_TMP[] = "/x4/state/books.bin.tmp";
// Last reads kept beside books.bin version 1; removed when it moves to version 2.
inline constexpr char LAST_READ_V1[] = "/x4/state/lastread.bin";
// Hashes of the Read articles finished, kept in step with read-log.jsonl.
inline constexpr char READ_DONE[] = "/x4/state/read.bin";
inline constexpr char READ_DONE_TMP[] = "/x4/state/read.bin.tmp";
// The issue last settled, then the kept pieces, each with what the Library shows of it.
inline constexpr char KEPT[] = "/x4/state/kept.jsonl";
inline constexpr char KEPT_TMP[] = "/x4/state/kept.tmp";
// A kept piece whole, as its issue listed it: /x4/state/kept/<read id>.json.
inline constexpr char KEPT_DIR[] = "/x4/state/kept";
inline constexpr char CARDS[] = "/x4/state/cards";
inline constexpr char CARDS_EXT[] = ".bin";
// The guess made before the article the set stands on (predict), until that step is done.
inline constexpr char GUESS[] = "/x4/state/guess.bin";
inline constexpr char GUESS_TMP[] = "/x4/state/guess.tmp";
// Present from a host write under /x4 until the set was rebuilt from it;
// carries the change across a reboot, flash or sleep.
inline constexpr char HOST_CHANGED[] = "/x4/state/host-changed";
// The ranges of the highlights made on a step, so their steps show them shaded.
inline constexpr char MARKS[] = "/x4/state/marks.bin";

}  // namespace x4::paths
