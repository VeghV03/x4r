"""Chess tactics for the Play game (docs/DATA.md, "Chess puzzles").

Source: the Lichess puzzle database, https://database.lichess.org/#puzzles (CC0). About five
million puzzles; this keeps a few thousand well-rated ones with short lines, the same number
in each of ten rating bands, so the device's level 1-10 picks a band by its slice of the file.

A line keeps Lichess's convention: the FEN is the position before the opponent's move, the
first of the moves. The device plays that move, so it can mark it on the board.
"""

import csv
import hashlib
import heapq
import io
import logging
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from pathlib import Path

from .convert import Upload
from .dicts import download

log = logging.getLogger(__name__)

URL = "https://database.lichess.org/lichess_db_puzzle.csv.zst"
REMOTE = "/x4/chess/puzzles.txt"
LICENSE = "CC0 (Lichess puzzle database)"

RATING_MIN, RATING_MAX = 800, 2400
BANDS = 10
PER_BAND = 400
PLIES = (4, 6, 8)  # the opponent's move and 2-4 of the player's, with replies
MIN_POPULARITY = 90  # Lichess's -100..100 vote score
MIN_PLAYS = 1000
MAX_DEVIATION = 90  # the rating is settled
LINE_MAX = 255  # bytes, the device's limit
ID_MAX = 15


@dataclass(frozen=True)
class Puzzle:
    id: str
    fen: str
    moves: str
    rating: int

    def line(self) -> str:
        return f"{self.id},{self.rating},{self.fen},{self.moves}"


def band_of(rating: int) -> int | None:
    """The rating band 0-9, or None outside 800-2399."""
    if not RATING_MIN <= rating < RATING_MAX:
        return None
    return (rating - RATING_MIN) * BANDS // (RATING_MAX - RATING_MIN)


def read_rows(lines: Iterable[str]) -> Iterator[Puzzle]:
    """Lichess CSV rows -> the puzzles good enough to keep."""
    for row in csv.reader(lines):
        if len(row) < 7 or row[0] == "PuzzleId":
            continue
        pid, fen, moves, rating, deviation, popularity, plays = row[:7]
        try:
            rating_, deviation_, popularity_, plays_ = int(rating), int(deviation), int(popularity), int(plays)
        except ValueError:
            continue
        if (
            band_of(rating_) is None
            or deviation_ > MAX_DEVIATION
            or popularity_ < MIN_POPULARITY
            or plays_ < MIN_PLAYS
            or len(moves.split()) not in PLIES
            or len(pid) > ID_MAX
        ):
            continue
        p = Puzzle(pid, fen, moves, rating_)
        if len(p.line().encode()) <= LINE_MAX:
            yield p


def _key(p: Puzzle) -> bytes:
    return hashlib.blake2b(p.id.encode(), digest_size=8).digest()


def select(puzzles: Iterable[Puzzle], per_band: int = PER_BAND) -> list[Puzzle]:
    """An even sample of each band, the same count in every band, sorted by rating.

    The sample is the puzzles with the smallest hash of their id: stable from run to run
    and spread over themes and openings, with memory for `per_band` puzzles a band."""
    heaps: list[list[tuple[bytes, str, Puzzle]]] = [[] for _ in range(BANDS)]
    for p in puzzles:
        heap = heaps[band_of(p.rating)]
        item = (bytes(255 - b for b in _key(p)), p.id, p)  # a max-heap of the smallest keys
        if len(heap) < per_band:
            heapq.heappush(heap, item)
        elif item[0] > heap[0][0]:
            heapq.heapreplace(heap, item)
    count = min(len(h) for h in heaps)
    if count == 0:
        raise ValueError("no puzzles in some rating band; the source has too few good ones")
    if count < per_band:
        log.warning("only %d puzzles in the smallest band; keeping %d a band", count, count)
    out = []
    for heap in heaps:
        kept = sorted(heap, reverse=True)[:count]  # smallest keys first
        out += sorted((p for _, _, p in kept), key=lambda p: (p.rating, p.id))
    return out


def write(puzzles: list[Puzzle], path: Path) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes("".join(f"{p.line()}\n" for p in puzzles).encode())
    return len(puzzles)


def _csv_lines(path: Path) -> Iterator[str]:
    """The CSV, decompressed on the fly when it is .zst."""
    if path.suffix != ".zst":
        with path.open(encoding="utf-8", newline="") as f:
            yield from f
        return
    import zstandard

    with path.open("rb") as raw, zstandard.ZstdDecompressor().stream_reader(raw) as reader:
        yield from io.TextIOWrapper(reader, encoding="utf-8", newline="")


@dataclass(frozen=True)
class Built:
    upload: Upload
    count: int


def build_from_file(source: Path, out_dir: Path, per_band: int = PER_BAND) -> Built:
    path = out_dir / "puzzles.txt"
    count = write(select(read_rows(_csv_lines(source)), per_band), path)
    return Built(Upload(path, REMOTE), count)


def build_chess(workdir: Path, per_band: int = PER_BAND) -> Built:
    """Download (cached, ~300 MB) and build the device's puzzle file."""
    return build_from_file(download(URL, workdir), workdir / "build", per_band)
