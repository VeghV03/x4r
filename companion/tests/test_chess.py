import json
from pathlib import Path

import pytest
import zstandard

from x4 import chess

FIXTURE = Path(__file__).parent / "fixtures" / "chess" / "puzzles.csv"


def rows() -> list[chess.Puzzle]:
    return list(chess.read_rows(FIXTURE.read_text().splitlines()))


def test_bands():
    assert chess.band_of(799) is None and chess.band_of(2400) is None
    assert chess.band_of(800) == 0 and chess.band_of(959) == 0 and chess.band_of(960) == 1
    assert chess.band_of(2399) == 9


def test_read_rows_keeps_good_short_puzzles():
    kept = rows()
    assert len(kept) == 30
    assert all(p.id.startswith("g") for p in kept)  # popularity, plays, deviation, rating, length, id
    first = kept[0]
    assert first.line() == "g0001,820,q3k1nr/1pp1nQpp/3p4/1P2p3/4P3/B1PP1b2/B5PP/5K2 b k - 0 17,e8d7 a2e6 d7d8 f7f8"


def test_select_takes_the_same_count_per_band_sorted_by_rating():
    picked = chess.select(rows(), per_band=2)
    assert len(picked) == 20
    ratings = [p.rating for p in picked]
    assert ratings == sorted(ratings)
    for band in range(chess.BANDS):
        assert sum(chess.band_of(r) == band for r in ratings) == 2
    assert picked == chess.select(reversed(rows()), per_band=2)  # stable, whatever the order
    # Fewer good puzzles than asked: every band keeps the smallest band's count.
    assert len(chess.select(rows(), per_band=5)) == 30
    with pytest.raises(ValueError):
        chess.select([p for p in rows() if p.rating > 1000], per_band=2)


def test_build_from_a_compressed_file(tmp_path):
    source = tmp_path / "puzzles.csv.zst"
    source.write_bytes(zstandard.ZstdCompressor().compress(FIXTURE.read_bytes()))
    built = chess.build_from_file(source, tmp_path / "out", per_band=3)
    assert built.count == 30 and built.upload.remote == "/x4/chess/puzzles.txt"
    lines = Path(built.upload.local).read_text().splitlines()
    assert len(lines) == 30 and all(len(line.encode()) <= chess.LINE_MAX for line in lines)
    assert all(len(line.split(",")) == 4 for line in lines)


def test_chess_install_uploads_to_the_device(x4, sim, tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    monkeypatch.setattr(chess, "download", lambda url, workdir: FIXTURE)
    result = x4("chess", "install")
    assert result.exit_code == 0, result.output
    assert result.output.startswith("/x4/chess/puzzles.txt  30 puzzles")
    assert len((sim.root / "x4/chess/puzzles.txt").read_text().splitlines()) == 30
    data = json.loads(x4("chess", "install", "--json").output)
    assert data["files"][0]["count"] == 30
