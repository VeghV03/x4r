import json

from records import DECK, TODAY as T, highlight, step_highlight, write_jsonl

from x4 import ops
from x4.context import context as build_context
from x4.home import commit_pending, pending, write_json


def context(h):
    """`context`, then the `x4 sync` that commits its cursor."""
    out = build_context(h)
    commit_pending(h.context_state)
    return out


def test_cursor_waits_for_sync(dev, sim, h):
    write_jsonl(sim.root / "x4" / "highlights.jsonl", [highlight("h1")])
    ops.pull(dev, h)
    assert [x["id"] for x in build_context(h)["highlights"]] == ["h1"]
    assert [x["id"] for x in build_context(h)["highlights"]] == ["h1"]  # no sync yet: reported again
    assert pending(h.context_state).exists() and not h.context_state.exists()
    ops.sync(dev, h)
    assert not pending(h.context_state).exists()
    assert build_context(h)["highlights"] == []


def review(card: str, grade: int, s: int = 12, **kw) -> dict:
    return {"card": card, "type": "flip", "grade": grade, "ms": 1000 * grade, "day": s, "at": None} | kw


def test_context(dev, sim, h):
    x4 = sim.root / "x4"
    write_jsonl(x4 / "highlights.jsonl", [highlight("h1"), highlight("q1", kind="question", text="Why?")])
    write_jsonl(
        x4 / "reviews.jsonl",
        [
            review("meditations/c1", 3),
            review("meditations/c1", 3, 13),
            review("meditations/c2", 1, correct=False, pick=2),
            review("meditations/c2", 3, 13, correct=True, pick=1),
            review("meditations/c2", 1, 14, correct=False, pick=0),
            review("meditations/c3", 3),
            review("meditations/c3", 1, 13),
            review("meditations/c3", 1, 14),
        ],
    )
    write_jsonl(x4 / "seen.jsonl", [{"id": "s0", "day": 13, "at": None}, {"id": "s2", "day": 14, "at": None}])
    (x4 / "stats.json").write_text(
        json.dumps(
            {
                "day": 14,
                "retention": 0.81,
                "due": 4,
                "books": {"books/meditations.epub": {"read_ms": 3_600_000, "pages": 10, "progress": 0.414}},
            }
        )
    )
    ops.pull(dev, h)
    write_jsonl(h.decks / "meditations.jsonl", DECK)
    ops.push_deck(dev, h, h.decks / "meditations.jsonl")
    stream = [{"id": f"s{i}", "kind": "bite", "title": f"T{i}", "body": "B"} for i in range(4)]
    ops.push_stream(dev, h, write_jsonl(h.root / "draft.jsonl", stream))

    c = context(h)
    assert (c["day"], c["retention"], c["due"]) == (14, 0.81, 4)
    assert c["highlights"] == [{"id": "h1", "book": "books/meditations.epub", "chapter": "Book II", "text": "text h1"}]
    assert [(q["id"], q["new"]) for q in c["questions"]] == [("q1", True)]
    assert c["reviews"]["cards"]["meditations/c2"] == {"reps": 3, "lapses": 2, "last": 1, "acc": 0.33, "wrong": [2, 0]}
    assert c["reviews"]["weak"] == ["meditations/c2", "meditations/c3"]
    assert c["books"]["books/meditations.epub"] == {
        "title": "Meditations",
        "progress": 0.41,
        "read_min": 60,
        "highlights": 2,
        "glossary": False,
        "notes": "notes/meditations.md",
    }
    assert c["decks"] == {"meditations": {"cards": 2, "synced": True, "flip": ["c1"]}}  # c1 is to rewrite
    assert c["stream"] == {"synced": True, "unseen": [{"id": f"s{i}", "kind": "bite", "title": f"T{i}"} for i in (1, 3)]}
    assert "last_letter" not in c

    again = context(h)
    assert again["highlights"] == []
    assert [(q["id"], q["new"]) for q in again["questions"]] == [("q1", False)]


def test_question_answered_by_a_reference(h):
    write_jsonl(h.archive("highlights"), [highlight("q1", kind="question"), highlight("q2", kind="question")])
    write_jsonl(h.stream, [{"id": "s1", "kind": "bite", "title": "T", "body": "B", "highlight": "q1"}])
    assert [q["id"] for q in context(h)["questions"]] == ["q2"]
    h.stream.unlink()  # the answering bite left the stream; the question stays answered
    assert [q["id"] for q in context(h)["questions"]] == ["q2"]


def test_context_lists_daily_set_highlights_with_their_step(h):
    write_jsonl(
        h.archive("highlights"),
        [
            step_highlight("f1", "s-1", book="books/meditations.epub"),
            step_highlight("f2", "s-w45", kind="question"),
        ],
    )
    c = context(h)
    assert c["highlights"] == [
        {
            "id": "f1",
            "book": "books/meditations.epub",
            "item": {"id": "s-1", "kind": "bite", "title": "Title s-1"},
            "text": "text f1",
        }
    ]
    assert c["questions"][0]["book"] == "stream/s-w45"
    assert c["questions"][0]["item"]["id"] == "s-w45"
    assert list(c["books"]) == ["books/meditations.epub"]  # a step without a book is not a book
    assert c["books"]["books/meditations.epub"]["highlights"] == 1


def test_context_lists_deleted_highlights(dev, sim, h):
    x4 = sim.root / "x4"
    write_jsonl(x4 / "highlights.jsonl", [highlight("h1"), highlight("h2"), highlight("q1", kind="question")])
    ops.pull(dev, h)
    assert context(h)["deleted"] == []
    write_jsonl(h.decks / "meditations.jsonl", [DECK[0], DECK[1] | {"highlight": "h1"}])
    write_jsonl(x4 / "highlights.jsonl", [{"id": "h1", "deleted": True}, {"id": "q1", "deleted": True}, highlight("h3")])
    ops.pull(dev, h)
    c = context(h)
    assert c["deleted"] == ["h1", "q1"]
    assert [x["id"] for x in c["highlights"]] == ["h3"]
    assert c["questions"] == []
    assert c["books"]["books/meditations.epub"]["highlights"] == 2
    # Next call: only the deletion a card still names.
    assert context(h)["deleted"] == ["h1"]
    write_jsonl(h.decks / "meditations.jsonl", [DECK[0], DECK[1]])  # Claude retired the card
    assert context(h)["deleted"] == []


def test_context_on_empty_home(h):
    c = context(h)
    assert c["highlights"] == [] and c["books"] == {} and c["stream"] == {"synced": False, "unseen": []}


def test_read_play_and_puzzles(dev, sim, h):
    x4 = sim.root / "x4"
    write_jsonl(
        x4 / "read-log.jsonl",
        [
            {"book": "books/read/r-1.epub", "progress": 0.3, "finished": False, "ms": 120000, "day": T - 1, "at": None},
            {"book": "books/read/r-1.epub", "progress": 1.0, "finished": True, "ms": 300000, "day": T, "at": None},
            {"book": "books/read/r-2.epub", "progress": 0.1, "finished": False, "ms": 30000, "day": T, "at": None},
            {"book": "books/read/r-0.epub", "progress": 1.0, "finished": True, "ms": 60000, "day": T - 11, "at": None},
            {"book": "books/other.epub", "progress": 0.5, "finished": False, "ms": 1, "day": T, "at": None},
        ],
    )
    write_jsonl(
        x4 / "play.jsonl",
        [
            {"game": "sudoku", "mode": "daily", "level": 4, "won": True, "ms": 400000, "day": T - 1, "at": None},
            {"game": "sudoku", "mode": "practice", "level": 4, "won": False, "ms": 200000, "day": T, "at": None},
            {"game": "words", "lang": "hu", "mode": "set", "level": 3, "won": True, "tries": 4, "ms": 90000, "day": T, "at": None},
            {"game": "arith", "mode": "daily", "level": 2, "won": True, "ms": 60000, "day": T - 16, "at": None},
        ],
    )
    write_jsonl(
        x4 / "seen.jsonl",
        [
            {"id": "s-p1", "correct": True, "ms": 41000, "day": T, "at": None},
            {"id": "s-p2", "correct": False, "ms": 9000, "day": T, "at": None},
            {"id": "s-b1", "day": T, "at": None},
        ],
    )
    (x4 / "stats.json").write_text(
        json.dumps(
            {
                "day": T,
                "books": {"books/read/r-1.epub": {"read_ms": 1, "pages": 3, "progress": 1.0}},
                "play": {"sudoku": {"level": 5, "played": 31, "won": 24}, "arith": {"level": 2, "played": 9, "won": 6}},
            }
        )
    )
    ops.pull(dev, h)
    assert ops.pull(dev, h)["play"] == {"new": 0, "duplicates": 0, "unreadable": 0}
    write_json(
        h.sync_state,
        {
            "pushed": {"/books/read/r-1.epub": 1, "/books/read/r-2.epub": 2},  # r-3 failed to fetch
            "read": {"r-1": {"kind": "found", "title": "The Extended Mind", "source": "Aeon"}},
            "puzzles": {"s-p1": {"kind": "riddle", "book": "books/meditations.epub"}},
        },
    )
    write_jsonl(
        h.read,
        [
            {"id": "r-1", "kind": "found", "url": "https://a/1"},
            {"id": "r-2", "kind": "news", "url": "https://a/2"},
            {"id": "r-3", "kind": "news", "url": "https://a/3"},
        ],
    )
    h.interests.write_text("Stoicism\n")
    h.inbox.write_text("https://a.example/x\n")

    c = context(h)
    assert c["read"] == {
        "results": [
            {"id": "r-1", "kind": "found", "title": "The Extended Mind", "source": "Aeon", "finished": True, "progress": 1.0, "min": 7},
            {"id": "r-2", "finished": False, "progress": 0.1, "min": 0},
        ],
        "waiting": ["r-2"],
    }  # fmt: skip
    assert c["play"] == {
        "sudoku": {"level": 5, "played": 31, "won": 24, "recent": {"played": 2, "won": 1, "practice": 1, "min": 10}},
        "arith": {"level": 2, "played": 9, "won": 6},
        "words": {"level": 3, "recent": {"played": 1, "won": 1, "practice": 0, "min": 2}},
    }
    assert c["puzzles"] == {
        "played": 2,
        "correct": 1,
        "results": [
            {"id": "s-p1", "kind": "riddle", "book": "books/meditations.epub", "correct": True, "sec": 41},
            {"id": "s-p2", "correct": False, "sec": 9},
        ],
    }
    assert c["interests"] == "Stoicism" and c["inbox"] == ["https://a.example/x"]
    assert c["books"] == {}  # Read articles are reported under read, not books


def test_push_stream_remembers_puzzles(dev, h, tmp_path):
    from x4.home import read_json

    src = write_jsonl(tmp_path / "s.jsonl", [{"id": "s-p1", "kind": "riddle", "prompt": "p", "answer": "a", "book": "books/a.epub"}])
    ops.push_stream(dev, h, src)
    assert read_json(h.sync_state, {})["puzzles"] == {"s-p1": {"kind": "riddle", "book": "books/a.epub"}}


def test_lookups_are_pulled_and_reported_once(dev, sim, h):
    write_jsonl(h.archive("highlights"), [highlight("h1")])  # titles the book
    write_json(h.sync_state, {"read": {"r-2026-11-03-7": {"kind": "link", "title": "On rivers", "source": "x"}}})
    write_jsonl(
        sim.root / "x4" / "lookups.jsonl",
        [
            {"word": "Ataraxia", "book": "books/meditations.epub", "set": 12, "at": None},
            {"word": "ataraxia", "book": "books/meditations.epub", "set": 13, "at": None},
            {"word": "oxbow", "book": "books/read/r-2026-11-03-7.epub", "set": 13, "at": None},
            {"word": "lacuna", "book": "books/other.epub", "title": "Other Book", "set": 13, "at": None},
            {"word": "lacuna", "book": "books/other.epub", "title": "Other Book", "set": 13, "at": None},
        ],
    )
    assert ops.pull(dev, h, "lookups")["lookups"] == {"new": 4, "duplicates": 1, "unreadable": 0}
    assert not (sim.root / "x4" / "lookups.jsonl").exists()
    assert context(h)["lookups"] == [
        {"word": "ataraxia", "book": "books/meditations.epub", "title": "Meditations"},
        {"word": "oxbow", "book": "books/read/r-2026-11-03-7.epub", "title": "On rivers"},
        {"word": "lacuna", "book": "books/other.epub", "title": "Other Book"},
    ]
    assert context(h)["lookups"] == []
    write_jsonl(sim.root / "x4" / "lookups.jsonl", [{"word": "liminal", "book": "books/other.epub", "set": 14, "at": None}])
    ops.pull(dev, h)
    assert [x["word"] for x in context(h)["lookups"]] == ["liminal"]
