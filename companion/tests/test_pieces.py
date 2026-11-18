import json
from datetime import date, timedelta

from records import TODAY, write_jsonl

from x4 import days, ops
from x4.pieces import minutes, pieces
from x4.readlist import Ready, note_sent, sent

ISSUE = days.to_date(TODAY).isoformat()
WEEK_AGO = days.to_date(TODAY - 8).isoformat()
MIN = 60000


def piece(i: str, section: str | None, *listed: str, kind: str = "found") -> dict:
    line = {"id": i, "kind": kind, "title": f"T {i}", "sent": listed[0], "url": f"https://a/{i}"}
    return line | ({"section": section, "issue": listed[0], "listed": list(listed)} if section else {})


def test_minutes_of_the_week_go_by_section_in_the_front_pages_order(h):
    write_jsonl(h.sent, [piece("r-1", "world", ISSUE), piece("r-2", "hungary", ISSUE), piece("r-k", None, ISSUE, kind="link"), piece("r-old", None, WEEK_AGO)])
    write_jsonl(h.archive("read-log"), [
        {"book": "books/read/r-old.epub", "progress": 0.5, "ms": 2 * MIN, "day": TODAY - 3},
        {"book": "books/read/r-1.epub", "progress": 1.0, "finished": True, "ms": 4 * MIN, "day": TODAY},
        {"book": "books/read/r-2.epub", "progress": 0.2, "ms": 2 * MIN, "day": TODAY - 7},  # a week and a day ago
        {"book": "books/read/r-k.epub", "progress": 0.5, "ms": 3 * MIN, "day": TODAY - 1},
        {"book": "books/meditations.epub", "progress": 0.4, "ms": 10 * MIN, "day": TODAY - 6},
        {"book": "books/meditations.epub", "progress": 0.3, "ms": 99 * MIN, "set": 4},  # before the day, no date: no day
    ])
    write_jsonl(h.archive("visits"), [{"section": "world", "issue": ISSUE, "ms": MIN, "day": TODAY}, {"section": "play", "ms": MIN, "day": TODAY}])
    on_x4 = {"play": [{"game": "sudoku", "mode": "daily", "ms": 5 * MIN, "day": TODAY}]}
    assert minutes(h, ["hungary", "world", "field"], on_x4) == {"hungary": 0, "world": 5, "field": 0, "play": 6, "book": 10, "kept": 3, "read": 2}


def test_pieces_say_whether_they_were_seen_opened_finished_and_kept(h):
    old = days.to_date(TODAY - 20).isoformat()
    write_jsonl(h.sent, [
        piece("r-1", "world", ISSUE) | {"test": "power"},
        piece("r-2", "field", WEEK_AGO, ISSUE),  # the field's primer, listed again
        piece("r-3", "world", WEEK_AGO),
        piece("r-0", "world", old),  # past the two weeks
        piece("r-k", None, ISSUE),  # a kept link, in no section
    ])
    write_jsonl(h.archive("visits"), [
        {"section": "world", "issue": ISSUE, "ms": MIN, "day": TODAY},
        {"section": "field", "issue": WEEK_AGO, "ms": MIN, "day": TODAY - 8},
    ])
    write_jsonl(h.archive("read-log"), [
        {"book": "books/read/r-2.epub", "progress": 0.4, "ms": 3 * MIN, "day": TODAY - 8},
        {"book": "books/read/r-2.epub", "progress": 1.0, "finished": True, "ms": 6 * MIN, "day": TODAY},
    ])
    write_jsonl(h.archive("kept"), [{"id": "r-3", "kept": True, "day": TODAY - 8}, {"id": "r-2", "kept": True}, {"id": "r-2", "kept": False}])
    got = {p["id"]: (p["section"], p["issue"], p["seen"], p["opened"], p["finished"], p["kept"], p["min"]) for p in pieces(h)}
    assert got == {
        "r-1": ("world", ISSUE, True, False, False, False, 0),
        "r-2": ("field", ISSUE, True, True, True, False, 9),
        "r-3": ("world", WEEK_AGO, False, False, False, True, 0),  # World was visited in another issue
    }
    assert [p["id"] for p in pieces(h)][-1] == "r-3"  # newest issue first
    assert [p.get("test") for p in pieces(h)] == ["power", None, None]  # the test a news piece's story passed


def test_every_issue_that_lists_a_piece_is_remembered(h, tmp_path):
    def ready(i: str, section: str | None = "field") -> Ready:
        record = {"id": i, "kind": "field", "title": f"T {i}"} | ({"section": section} if section else {})
        return Ready(record=record, epub=tmp_path / f"{i}.epub", url=f"https://a/{i}", file=None)

    monday = date(2026, 11, 9)
    note_sent(h, [ready("r-1"), ready("r-k", None)], monday)
    note_sent(h, [ready("r-1")], monday)
    note_sent(h, [ready("r-1")], monday + timedelta(days=1))
    lines = sent(h)
    assert lines["r-1"]["issue"] == "2026-11-09" and lines["r-1"]["listed"] == ["2026-11-09", "2026-11-10"]
    assert "listed" not in lines["r-k"]


def test_status_has_the_weeks_minutes_with_what_is_still_on_the_x4(dev, sim, h):
    h.sections.parent.mkdir(parents=True, exist_ok=True)
    h.sections.write_text("Hungary: news\nWorld: news\n")
    write_jsonl(sim.root / "x4/visits.jsonl", [{"section": "world", "issue": ISSUE, "ms": 2 * MIN, "day": TODAY}])
    write_jsonl(h.archive("visits"), [{"section": "world", "issue": ISSUE, "ms": MIN, "day": TODAY}])
    out = ops.status(dev, h)
    assert out["minutes"] == {"hungary": 0, "world": 3}
    assert out["day"] == TODAY and "set" not in out and "streak" not in out
    json.dumps(out)
