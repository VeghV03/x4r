import json
from pathlib import Path

import pytest

from x4 import words
from x4.dicts import _lines

FIXTURES = Path(__file__).parent / "fixtures" / "words"


def lexicon(lang: str) -> words.Lexicon:
    return words.read_lexicon(_lines(FIXTURES / "kaikki.jsonl"), lang)


def ranks(lang: str) -> dict[str, int]:
    return words.read_ranks((FIXTURES / f"{lang}_50k.txt").read_text(encoding="utf-8").splitlines(), lang)


@pytest.fixture
def en():
    return lexicon("en"), ranks("en")


def merged(tmp_path: Path) -> tuple[Path, Path]:
    """The English fixture plus kaikki7.jsonl: words that fill three 7x7 grids (three-letter
    ones ranked in en7_50k.txt, the rest unranked with three senses)."""
    dump, freq = tmp_path / "kaikki-all.jsonl", tmp_path / "en-all.txt"
    dump.write_bytes((FIXTURES / "kaikki.jsonl").read_bytes() + (FIXTURES / "kaikki7.jsonl").read_bytes())
    freq.write_bytes((FIXTURES / "en_50k.txt").read_bytes() + (FIXTURES / "en7_50k.txt").read_bytes())
    return dump, freq


@pytest.fixture
def en7(tmp_path):
    dump, freq = merged(tmp_path)
    lex = words.read_lexicon(_lines(dump), "en")
    return lex, words.read_ranks(freq.read_text(encoding="utf-8").splitlines(), "en")


def test_lexicon_flags(en):
    lex, _ = en
    w = lex.words
    assert "london" not in w  # proper nouns are not words
    assert w["debby"].named  # a weak entry shadowed by a name
    assert w["shite"].bad
    assert "scuba" not in w  # only an acronym
    assert w["heard"].inflected and not w["heard"].lemma
    assert w["colour"].alt and not w["colour"].lemma
    assert w["olden"].lemma and not w["olden"].clean
    assert w["houses"].inflected  # from the forms of "house"


def test_answers_are_common_clean_headwords_by_frequency(en):
    lex, r = en
    assert words.answers5(lex, r, "en") == ["there", "other", "house", "earth", "enter", "plane", "burnt", "taboo",
                                            "color"]  # fmt: skip


def test_valid5_is_sorted_by_bytes_and_accepts_forms(en):
    lex, r = en
    valid = words.valid5(lex, words.answers5(lex, r, "en"))
    assert valid == sorted(valid, key=str.encode)
    assert {"heard", "shite", "olden", "there"} <= set(valid)
    assert all(len(v) == 5 for v in valid)


def test_hungarian_letters_are_single_characters():
    lex, r = lexicon("hu"), ranks("hu")
    assert words.answers5(lex, r, "hu") == ["felhő", "almás"]  # by frequency
    assert words.valid5(lex, []) == ["almák", "almás", "erdők", "felhő"]  # ő is one letter


def test_clue_never_gives_the_word_away():
    assert words.clue("plan", ["A plan for the day", "A set of intended actions; a scheme."]) == "A set of intended actions"
    assert words.clue("élem", ["First-person singular possessive of él"]) is None
    assert words.clue("cee", ["The name of the Latin-script letter C."]) is None
    assert words.clue("ten", ["Ten million: 10⁷", "(informal) A number"]) == "A number"


def test_crosswords_fill_and_number_the_grid(en7):
    lex, r = en7
    puzzles = words.crosswords(lex, r, "en", count=4)
    assert puzzles == words.crosswords(lex, r, "en", count=4)  # deterministic
    pool = set(words.grid_pool(lex, r, "en"))
    for p in puzzles:
        rows = p["rows"]
        assert p["size"] == 7 and len(rows) == 7 and all(len(row) == 7 for row in rows)
        across, down = words.numbered(rows)
        assert [e["n"] for e in p["across"]] == [n for n, _ in across]
        assert [e["n"] for e in p["down"]] == [n for n, _ in down]
        entries = [w for _, w in across + down]
        assert set(entries) <= pool and len(set(entries)) == len(entries)
        for (_, w), e in zip(across + down, p["across"] + p["down"], strict=True):
            assert w not in e["clue"].lower()
    assert puzzles[0]["id"] == "en-0001"


def test_numbering_follows_reading_order():
    across, down = words.numbered(["there", "a.a.n", "burnt", "o.t.e", "other"])
    assert across == [(1, "there"), (4, "burnt"), (5, "other")]
    assert down == [(1, "taboo"), (2, "earth"), (3, "enter")]


def test_build_writes_every_file(en7, tmp_path):
    lex, r = en7
    built = words.build_from(lex, r, "en", tmp_path, count=2)
    assert [b.upload.remote for b in built] == [
        "/x4/words/en/answers5.txt",
        "/x4/words/en/valid5.txt",
        "/x4/crosswords/en.jsonl",
    ]
    lines = (tmp_path / "en.jsonl").read_text().splitlines()
    assert len(lines) == built[-1].count == 2
    assert json.loads(lines[1])["id"] == "en-0002"


def test_words_install_uploads_to_the_device(x4, sim, tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    sources = {words.SOURCES["hu"].url: FIXTURES / "kaikki.jsonl", words.FREQ_URL.format(lang="hu"): FIXTURES / "hu_50k.txt"}
    monkeypatch.setattr(words, "download", lambda url, workdir: sources[url])
    with pytest.raises(ValueError, match="cannot fill"):
        words.build_words("hu", tmp_path / "work")  # two Hungarian words fill no grid
    sources[words.SOURCES["en"].url], sources[words.FREQ_URL.format(lang="en")] = merged(tmp_path)
    result = x4("words", "install", "en")
    assert result.exit_code == 0, result.output
    out = result.output.splitlines()
    assert len(out) == 3 and out[0].startswith("/x4/words/en/answers5.txt  9 words")
    assert (sim.root / "x4/words/en/valid5.txt").exists()
    assert len((sim.root / "x4/crosswords/en.jsonl").read_text().splitlines()) == words.CROSSWORDS
    data = json.loads(x4("words", "install", "en", "--json").output)
    assert data["lang"] == "en" and data["files"][2]["remote"] == "/x4/crosswords/en.jsonl"


def test_unknown_language(tmp_path):
    with pytest.raises(ValueError):
        words.build_words("xx", tmp_path)
