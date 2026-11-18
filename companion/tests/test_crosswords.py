import gzip
import json

import pytest
from records import write_jsonl

from x4 import crosswords, ops, words
from x4.errors import InvalidData, X4Error


# Three filled 7x7 grids (tests/fixtures/words/kaikki7.jsonl): their words are the whole lexicon.
GRIDS = [
    ["you.age", "eat.wer", "sticker", "..law..", "animate", "rot.ray", "try.due"],
    ["...the.", "..sound", "riposte", "ace.hem", "chasery", "yoked..", ".row..."],
    ["...bass", "..ortho", ".sortal", "and.end", "holist.", "erect..", "mese..."],
]
WORDS = [{w for _, w in sum(words.numbered(g), [])} for g in GRIDS]


def lexicon(*extra: tuple[str, words.Word]) -> tuple[words.Lexicon, dict[str, int]]:
    entries = {w: words.Word(lemma=True, clean=True, score=3, glosses=["a thing"]) for ws in WORDS for w in ws}
    entries.update(extra)
    order = sorted(entries)
    return words.Lexicon(entries), {w: i for i, w in enumerate(order)}


def answers(grid: dict) -> set[str]:
    return {e["answer"] for e in grid["across"] + grid["down"]}


def test_grids_are_numbered_and_never_repeat_an_answer():
    lex, ranks = lexicon()
    made = words.grids(lex, ranks, "hu", 7, 3, set())
    assert made == words.grids(lex, ranks, "hu", 7, 3, set())  # deterministic
    grids = made["grids"]
    assert [g["id"] for g in grids] == ["hu-0007", "hu-0008", "hu-0009"]
    for g in grids:
        assert g["size"] == 7
        across, down = words.numbered(g["rows"])
        assert [(e["n"], e["answer"]) for e in g["across"]] == across
        assert [(e["n"], e["answer"]) for e in g["down"]] == down
        assert "clue" not in json.dumps(g) and "seed" not in json.dumps(g)
    assert len(answers(grids[0]) | answers(grids[1]) | answers(grids[2])) == 54
    assert made["placed"] == made["unplaced"] == made["skipped"] == []
    with pytest.raises(ValueError, match="too few hu words"):
        words.grids(lex, ranks, "hu", 1, 4, set())


def test_grids_leave_out_recent_answers():
    lex, ranks = lexicon()
    made = words.grids(lex, ranks, "hu", 1, 1, WORDS[0] | WORDS[2])["grids"]
    assert answers(made[0]) == WORDS[1]


def test_seed_words_go_in_first_with_their_source():
    lex, ranks = lexicon()
    seeds = {"Riposte": "Kardvívás", "holist": None, "zzz": "X", "elmélkedés": "Y", "ab-c": None, "sticker": "Z"}
    made = words.grids(lex, ranks, "hu", 1, 2, {"sticker"}, seeds)
    assert made["placed"] == ["riposte", "holist"]
    assert made["unplaced"] == ["zzz"]  # fits no grid
    assert made["skipped"] == ["elmélkedés", "ab-c", "sticker"]  # too long, not letters, a recent answer
    entries = {e["answer"]: e for g in made["grids"] for e in g["across"] + g["down"]}
    assert {k: v for k, v in entries["riposte"].items() if k != "n"} == {"answer": "riposte", "seed": True, "src": "Kardvívás"}
    assert entries["holist"]["seed"] is True and "src" not in entries["holist"]
    assert sum("seed" in e for e in entries.values()) == 2


def test_pool_is_rare_headwords_first():
    lex, ranks = lexicon(
        ("orrom", words.Word(lemma=True, clean=True, glosses=["first-person singular possessive of orr"])),
        ("lett", words.Word(lemma=True, clean=True, glosses=["third-person singular past of lesz", "Latvian"])),
        ("debby", words.Word(lemma=True, clean=True, named=True, glosses=["debutante"])),
        ("szar", words.Word(lemma=True, clean=True, bad=True, glosses=["shit"])),
        ("régi", words.Word(lemma=True, glosses=["olden"])),  # only an archaic sense
        ("almák", words.Word(inflected=True)),
        ("alma", words.Word(lemma=True, clean=True, inflected=True, glosses=["apple (fruit)"])),
        ("ritka", words.Word(lemma=True, clean=True, score=1, glosses=["rare"])),
        ("öt", words.Word(lemma=True, clean=True, glosses=["five"])),
        ("ősi", words.Word(lemma=True, clean=True, score=9, glosses=["ancient"])),
        ("elmélkedés", words.Word(lemma=True, clean=True, glosses=["meditation"])),
    )
    ranks = {"alma": 10, "lett": 5000, "age": 1, "ritka": 40_000}  # the rest has no rank
    pool = words.grid_pool(lex, ranks, "hu")
    assert pool.index("age") < pool.index("lett") < pool.index("ritka")  # rare by rank; any three-letter
    assert pool.index("ritka") < pool.index("sticker") < pool.index("alma")  # then unranked, then common
    assert not {"orrom", "debby", "szar", "régi", "almák", "öt", "ősi", "elmélkedés"} & set(pool)
    english = words.grid_pool(lex, ranks, "en")  # an unranked English word needs three senses
    assert "sticker" in english and "ritka" in english
    lex.words["sticker"].score = 2
    assert "sticker" not in words.grid_pool(lex, ranks, "en")


def clued(pid: str, rows: list[str]) -> dict:
    across, down = words.numbered(rows)
    return {
        "id": pid,
        "size": len(rows),
        "rows": rows,
        "across": [{"n": n, "answer": w, "clue": f"Rejtvény {n}"} for n, w in across],
        "down": [{"n": n, "clue": f"Függőleges {n}"} for n, w in down],
    }


ROWS = ["there", "a.a.n", "burnt", "o.t.e", "other"]


def test_valid_file_is_pushed_without_answers(tmp_path):
    path = write_jsonl(tmp_path / "hu.jsonl", [clued("hu-0001", ROWS), clued("hu-0002", ROWS)])
    c = crosswords.check_crosswords(path, "hu")
    assert c.count == 2 and b'"answer"' not in c.body
    assert json.loads(c.body.splitlines()[0])["across"][0] == {"n": 1, "clue": "Rejtvény 1"}


def test_seven_by_seven_keeps_sources_and_drops_seed_marks(tmp_path):
    p = clued("hu-0003", GRIDS[1])
    p["across"][0] |= {"seed": True, "src": "Elmélkedések"}  # decomposed é
    p["down"][0] |= {"seed": True}
    path = write_jsonl(tmp_path / "hu.jsonl", [clued("hu-0001", ROWS), p])  # a 5x5 puzzle still plays
    c = crosswords.check_crosswords(path, "hu")
    device = json.loads(c.body.splitlines()[1])
    assert device["size"] == 7 and device["rows"] == GRIDS[1]
    assert device["across"][0] == {"n": 1, "clue": "Rejtvény 1", "src": "Elmélkedések"}
    assert device["down"][0] == {"n": 1, "clue": "Függőleges 1"}


def test_source_errors_say_what_to_fix(tmp_path):
    p = clued("hu-0001", GRIDS[0])
    p["across"][0]["src"] = ""
    p["across"][1]["src"] = "A" * 49
    for i, e in enumerate(p["down"]):
        e["src"] = f"Könyv {i}"
    assert errors(tmp_path, p) == [
        "across.0.src: must be the title of the book or article the word came from; leave it out otherwise",
        "across.1.src: is 49 bytes, the device keeps 48; shorten the title",
        "src: 11 different sources, the device shows 6; leave src out on the words of the others",
    ]
    six = clued("hu-0001", GRIDS[0]) | {"size": 6}
    assert errors(tmp_path, six) == ["size: must be 7"]


def errors(tmp_path, *records: dict) -> list[str]:
    path = write_jsonl(tmp_path / "hu.jsonl", list(records))
    with pytest.raises(InvalidData) as e:
        crosswords.check_crosswords(path, "hu")
    return [d.split(":", 2)[2].strip() for d in e.value.details]


def test_grid_errors_say_what_to_fix(tmp_path):
    bad = clued("hu-0001", ROWS) | {"rows": ["thEre", "a.a.n", "burnt", "o.t.e", "othe"]}
    assert errors(tmp_path, bad) == [
        "rows.0: 'E' not in the hu alphabet; use lowercase letters, . for a black square",
        "rows.4: 'othe' has 4 characters; a row has 5",
    ]
    lonely = clued("hu-0001", ROWS) | {"rows": ["there", "a.a.n", "burnt", "o.t.e", ".t.er"]}
    assert "rows.4: cell 2 is in no word; make it . or run a word through it" in errors(tmp_path, lonely)
    assert errors(tmp_path, clued("en-0001", ROWS))[0].startswith("id: must be like hu-0001")
    assert errors(tmp_path, clued("hu-0001", ROWS), clued("hu-0001", ROWS)) == [
        "id: 'hu-0001' is already used on line 1; crossword ids must be unique"
    ]


def test_numbering_must_match_the_rows(tmp_path):
    p = clued("hu-0001", ROWS)
    p["down"] = p["down"][:2]
    assert errors(tmp_path, p) == ["down: numbers must be [1, 2, 3] (from rows), got [1, 2]"]
    p = clued("hu-0001", ROWS)
    p["across"][1]["answer"] = "burns"
    assert errors(tmp_path, p) == ["across.1.answer: is 'burns', the grid has 'burnt' at 4 across"]


def test_clues_fit_the_screen_and_keep_the_answer(tmp_path):
    p = clued("hu-0001", ROWS)
    p["across"][0]["clue"] = " "
    p["across"][1]["clue"] = "Ami égett: Burnt"  # case-insensitive
    p["across"][2]["clue"] = "Sor\nmásik"
    p["down"][0]["clue"] = " ".join(["m" * 13] * 4)  # one word per line
    p["down"][1]["clue"] = "ő" * 49
    p["down"][2]["clue"] = "Kínai 中"
    assert errors(tmp_path, p) == [
        "across.0.clue: is required; write a clue for 'there'",
        "across.1.clue: contains the answer 'burnt'; clue it without the word",
        "across.2.clue: has a line break or control character; write it on one line",
        "down.0.clue: takes 4 lines on the device, it shows 3; shorten it",
        "down.1.clue: is 98 bytes, the device shows 96; shorten it",
        "down.2.clue: the device font has no '中'; use other characters",
    ]


def test_answer_check_is_accent_aware():
    assert crosswords._gives_away("Házak sora", "ház")
    assert crosswords._check_clue("x", "Ha\u0301zak sora", "h\u00e1z") == [
        "x.clue: contains the answer 'h\u00e1z'; clue it without the word"
    ]  # decomposed accents
    assert not crosswords._gives_away("Kor, idő", "kör")  # another word
    assert not crosswords._gives_away("Szemközt", "köz")  # inside a word, not its start
    assert not crosswords._check_clue("x", "A\u0301llat a sivatagban", "teve")


def test_clue_lines_follow_the_device_wrap():
    assert crosswords.clue_lines("Teve") == 1
    assert crosswords.CLUE_WIDTH == 394
    word = "m" * 14  # 14 x 27.25 px fits, 15 does not
    assert crosswords.clue_lines(word) == 1 and crosswords.clue_lines(word + "m") == 2
    assert crosswords.clue_lines(f"{word} {word} {word}") == 3


def test_ids_and_recent_answers_come_from_the_host_file(tmp_path):
    path = tmp_path / "hu.jsonl"
    assert crosswords.next_number(path, "hu") == 1 and crosswords.recent_answers(path, "hu") == set()
    write_jsonl(path, [clued("hu-0009", ROWS), clued("hu-0012", ["teve.", "....."] + ["....."] * 3)])
    assert crosswords.next_number(path, "hu") == 13
    assert crosswords.recent_answers(path, "hu", 1) == {"teve"}
    assert "burnt" in crosswords.recent_answers(path, "hu")


def cache(tmp_path, monkeypatch) -> None:
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    lex, ranks = lexicon()
    folder = tmp_path / "cache" / "x4" / "dicts"
    data = {"words": {w: e.__dict__ for w, e in lex.words.items()}}
    (folder / "words").mkdir(parents=True)
    (folder / "words" / f"hu-1-1.lexicon{words.LEXICON_VERSION}.json.gz").write_bytes(
        gzip.compress(json.dumps(data, ensure_ascii=False).encode())
    )
    (folder / "downloads").mkdir()
    (folder / "downloads" / "hu_50k.txt").write_text("".join(f"{w} 1\n" for w in sorted(ranks)), encoding="utf-8")


def test_words_grids_command(x4, h, tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "empty"))
    result = x4("words", "grids", "hu")
    assert result.exit_code == 1 and "run x4 words install hu once" in result.output
    cache(tmp_path, monkeypatch)
    write_jsonl(h.crosswords / "hu.jsonl", [clued("hu-0004", ["teve.", "....."] + ["....."] * 3)])
    data = json.loads(x4("words", "grids", "hu", "--count", "2", "--json").output)
    assert [g["id"] for g in data["grids"]] == ["hu-0005", "hu-0006"]
    assert data["file"] == str(h.crosswords / "hu.grids.jsonl")
    assert data["append_to"] == str(h.crosswords / "hu.jsonl")
    saved = [json.loads(line) for line in (h.crosswords / "hu.grids.jsonl").read_text().splitlines()]
    assert saved == data["grids"]
    text = x4("words", "grids", "hu", "--count", "1").output.splitlines()
    assert text[0] == "hu-0005" and text[-1] == f"clue them into {h.crosswords / 'hu.jsonl'}"

    seeds = tmp_path / "seeds.txt"
    seeds.write_text("holist\tA holista\nzzz\n", encoding="utf-8")
    args = ("words", "grids", "hu", "--count", "2", "--words", "riposte, elmélkedés", "--words-file", str(seeds))
    data = json.loads(x4(*args, "--json").output)
    assert (data["placed"], data["unplaced"], data["skipped"]) == (["riposte", "holist"], ["zzz"], ["elmélkedés"])
    entries = {e["answer"]: e for g in data["grids"] for e in g["across"] + g["down"]}
    assert entries["holist"]["src"] == "A holista" and "src" not in entries["riposte"]
    text = x4(*args).output.splitlines()
    assert "seeds placed (*): 2" in text and "seeds not placed: zzz" in text and "seeds skipped: elmélkedés" in text


def test_sync_validates_and_pushes_host_crosswords(dev, sim, h):
    write_jsonl(h.crosswords / "hu.jsonl", [clued("hu-0001", ROWS)])
    write_jsonl(h.crosswords / "hu.grids.jsonl", [{"id": "hu-0002"}])  # waiting for clues: not pushed
    assert ops.sync(dev, h)["pushed"] == ["/x4/crosswords/hu.jsonl"]
    pushed = (sim.root / "x4/crosswords/hu.jsonl").read_text()
    assert '"answer"' not in pushed and json.loads(pushed)["id"] == "hu-0001"
    assert ops.sync(dev, h)["pushed"] == []

    write_jsonl(h.crosswords / "hu.jsonl", [clued("hu-0001", ROWS), clued("hu-0002", ROWS) | {"down": []}])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert e.value.details == [f"{h.crosswords / 'hu.jsonl'}:2: down: numbers must be [1, 2, 3] (from rows), got []"]
    assert (sim.root / "x4/crosswords/hu.jsonl").read_text() == pushed


def test_words_grids_needs_a_lexicon(h, tmp_path, monkeypatch):
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "none"))
    with pytest.raises(X4Error, match="no hu lexicon"):
        ops.words_grids(h, "hu")


def test_words_install_keeps_host_crosswords(x4, sim, h, tmp_path, monkeypatch):
    from test_words import FIXTURES

    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    sources = {words.SOURCES["en"].url: FIXTURES / "kaikki.jsonl"}
    sources[words.FREQ_URL.format(lang="en")] = FIXTURES / "en_50k.txt"
    monkeypatch.setattr(words, "download", lambda url, workdir: sources[url])
    (sim.root / "x4/crosswords").mkdir(parents=True)
    (sim.root / "x4/crosswords/en.jsonl").write_text("clued\n")
    write_jsonl(h.crosswords / "en.jsonl", [clued("en-0001", ROWS)])
    out = x4("words", "install", "en").output.splitlines()
    assert out[-1] == f"/x4/crosswords/en.jsonl  kept: {h.crosswords / 'en.jsonl'}"
    assert (sim.root / "x4/crosswords/en.jsonl").read_text() == "clued\n"
    assert (sim.root / "x4/words/en/answers5.txt").exists()
