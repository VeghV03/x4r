import json

import pytest
from records import highlight, step_highlight, write_jsonl

from x4 import ops
from x4.errors import InvalidData
from x4.home import read_jsonl
from x4.models import Highlight
from x4.notes import render

REVIEW = {"card": "meditations/c2", "type": "choice", "grade": 3, "correct": True, "ms": 5400, "day": 12, "at": None}
VISIT = {"section": "world", "issue": "2026-11-12", "ms": 41000, "day": 12, "at": None}


def device_logs(sim, highlights=(), reviews=(), visits=(), seen=()):
    x4 = sim.root / "x4"
    for name, records in (("highlights", highlights), ("reviews", reviews), ("visits", visits), ("seen", seen)):
        if records:
            write_jsonl(x4 / f"{name}.jsonl", list(records))


def test_pull_archives_then_deletes(dev, sim, h):
    device_logs(
        sim, [highlight("h1"), highlight("h2")], [REVIEW], [VISIT], [{"id": "s1", "day": 12}]
    )
    write_jsonl(sim.root / "x4/stats.json", [{"day": 12, "retention": 0.9}])
    out = ops.pull(dev, h)
    assert out["highlights"] == {"new": 2, "duplicates": 0, "unreadable": 0, "deleted": 0}
    assert out["reviews"]["new"] == 1 and out["visits"]["new"] == 1 and out["seen"]["new"] == 1
    assert out["stats"] is True
    assert sorted(p.name for p in (sim.root / "x4").iterdir()) == ["stats.json"]  # logs and .pulling gone
    assert (sim.root / "x4/stats.json").exists()  # device-owned, rewritten in place: never deleted
    assert json.loads(h.stats.read_text())["day"] == 12
    assert [r["id"] for r in read_jsonl(h.archive("highlights"))] == ["h1", "h2"]


def test_pull_dedupes(dev, sim, h):
    device_logs(sim, [highlight("h1")], [REVIEW])
    ops.pull(dev, h)
    again = REVIEW | {"ms": 9000}  # same card and day, a different answer
    device_logs(sim, [highlight("h1"), highlight("h2")], [REVIEW, again])
    out = ops.pull(dev, h)
    assert out["highlights"] == {"new": 1, "duplicates": 1, "unreadable": 0, "deleted": 0}
    assert out["reviews"] == {"new": 1, "duplicates": 1, "unreadable": 0}
    assert len(read_jsonl(h.archive("reviews"))) == 2


def test_pull_renames_before_reading(dev, sim, h):
    device_logs(sim, [highlight("h1")])
    calls = []
    original = dev.call

    def spy(command, args=None, timeout=10.0):
        calls.append((command, (args or {}).get("path") or (args or {}).get("from")))
        return original(command, args, timeout)

    dev.call = spy
    ops.pull(dev, h, "highlights")
    assert ("mv", "/x4/highlights.jsonl") in calls
    assert calls.index(("mv", "/x4/highlights.jsonl")) < calls.index(("rm", "/x4/highlights.pulling"))


def test_pull_finishes_interrupted_pull_first(dev, sim, h):
    write_jsonl(sim.root / "x4/highlights.pulling", [highlight("h1"), highlight("h2")])
    ops.pull(dev, h, "highlights")
    write_jsonl(sim.root / "x4/highlights.pulling", [highlight("h2"), highlight("h3")])  # left behind
    write_jsonl(sim.root / "x4/highlights.jsonl", [highlight("h4")])  # written since
    out = ops.pull(dev, h, "highlights")
    assert out["highlights"] == {"new": 2, "duplicates": 1, "unreadable": 0, "deleted": 0}
    assert [r["id"] for r in read_jsonl(h.archive("highlights"))] == ["h1", "h2", "h3", "h4"]
    assert not list((sim.root / "x4").glob("highlights.*"))


def test_pull_keeps_unreadable_lines(dev, sim, h):
    (sim.root / "x4").mkdir(parents=True)
    (sim.root / "x4/highlights.jsonl").write_text(json.dumps(highlight("h1")) + '\n{"id":"h2","text":\n{"no":"id"}\n')
    out = ops.pull(dev, h, "highlights")
    assert out["highlights"] == {"new": 1, "duplicates": 0, "unreadable": 2, "deleted": 0}
    assert len((h.pulled / "highlights.bad").read_text().splitlines()) == 2


def test_pull_one_kind_only(dev, sim, h):
    device_logs(sim, [highlight("h1")], [REVIEW])
    out = ops.pull(dev, h, "reviews")
    assert set(out) == {"reviews"}
    assert (sim.root / "x4/highlights.jsonl").exists()


def test_pull_nothing_on_device(dev, h):
    out = ops.pull(dev, h)
    assert out["highlights"]["new"] == 0 and out["stats"] is False and out["notes"] == []


def test_notes_grouped_by_chapter_and_idempotent(dev, sim, h):
    device_logs(
        sim,
        [
            highlight("h2", chapter="Book II", text="Second.", pos={"spine": 3, "start": 50, "end": 60}),
            highlight("h1", chapter="Book I", text="First.", pos={"spine": 2, "start": 9, "end": 20}),
            highlight("q1", kind="question", chapter="Book II", text="Why?", pos={"spine": 3, "start": 70, "end": 80}),
            highlight("o1", book="books/other.epub", title="Other", author=None, chapter=None, text="Para one.\n\nPara two."),
        ],
    )
    out = ops.pull(dev, h)
    assert sorted(out["notes"]) == [str(h.notes / "meditations.md"), str(h.notes / "other.md")]
    assert (h.notes / "meditations.md").read_text() == (
        "# Meditations\n\nMarcus Aurelius\n\n## Book I\n\n> First.\n\n## Book II\n\n> Second.\n\n> **?** Why?\n"
    )
    assert (h.notes / "other.md").read_text() == "# Other\n\n> Para one.\n>\n> Para two.\n"
    device_logs(sim, [highlight("h1", chapter="Book I", text="First.", pos={"spine": 2, "start": 9, "end": 20})])
    assert ops.pull(dev, h)["notes"] == []  # nothing new, nothing rewritten


def test_daily_set_highlights_join_their_book_or_their_step(dev, sim, h):
    device_logs(
        sim,
        [
            highlight("h1", chapter="Book I", text="First.", pos={"spine": 2, "start": 9, "end": 20}),
            step_highlight("f1", "s-1", book="books/meditations.epub", text="From a bite."),
            step_highlight("f2", "s-w45", text="From the letter."),
        ],
    )
    out = ops.pull(dev, h)
    assert sorted(out["notes"]) == [str(h.notes / "meditations.md"), str(h.notes / "title-s-w45.md")]
    assert (h.notes / "meditations.md").read_text() == (
        "# Meditations\n\nMarcus Aurelius\n\n## Book I\n\n> First.\n\n## Title s-1\n\n> From a bite.\n"
    )
    assert (h.notes / "title-s-w45.md").read_text() == "# Title s-w45\n\n> From the letter.\n"
    pulled = ops.highlights(h)
    assert pulled[1].item.id == "s-1" and pulled[1].item.kind == "bite"


def test_same_title_books_get_distinct_notes(h):
    from x4.notes import render_all

    a = Highlight.model_validate(highlight("a", book="books/a/notes.epub", title="Notes"))
    b = Highlight.model_validate(highlight("b", book="books/b/notes2.epub", title="Notes"))
    paths = render_all(h, [a, b])
    assert sorted(p.name for p in paths) == ["notes-notes2.md", "notes.md"]


def test_render_without_metadata():
    text = render("books/x.epub", [Highlight(id="h", text="t")])
    assert text == "# x\n\n> t\n"


def deletion(i: str) -> dict:
    return {"id": i, "deleted": True, "set": 13, "at": None}


def test_pull_applies_deletions(dev, sim, h):
    device_logs(sim, [highlight("h1"), highlight("h2"), highlight("o1", book="books/other.epub", title="Other")])
    ops.pull(dev, h)
    assert (h.notes / "other.md").exists()
    device_logs(sim, [deletion("h1"), deletion("o1"), deletion("never-pulled")])
    out = ops.pull(dev, h)
    assert out["highlights"] == {"new": 0, "duplicates": 1, "unreadable": 0, "deleted": 2}
    assert [x.id for x in ops.highlights(h)] == ["h2"]
    assert "text h1" not in (h.notes / "meditations.md").read_text()
    assert not (h.notes / "other.md").exists()  # its only highlight is gone
    assert str(h.notes / "other.md") in out["notes"]
    # The archive stays append-only and keeps the deletion records.
    assert [(r["id"], r.get("deleted", False)) for r in read_jsonl(h.archive("highlights"))][-2:] == [
        ("h1", True),
        ("o1", True),
    ]
    device_logs(sim, [deletion("h1")])  # pulled again after an interrupted pull
    assert ops.pull(dev, h)["highlights"] == {"new": 0, "duplicates": 1, "unreadable": 0, "deleted": 0}


def test_highlight_made_again_after_its_deletion(dev, sim, h):
    device_logs(sim, [highlight("h1")])
    ops.pull(dev, h)
    device_logs(sim, [deletion("h1"), highlight("h1")])  # deleted, then the same passage highlighted again
    out = ops.pull(dev, h)
    assert out["highlights"] == {"new": 1, "duplicates": 0, "unreadable": 0, "deleted": 1}
    assert [x.id for x in ops.highlights(h)] == ["h1"]
    device_logs(sim, [highlight("h2"), deletion("h2")])  # made and deleted between two pulls
    ops.pull(dev, h)
    assert [x.id for x in ops.highlights(h)] == ["h1"]


def test_stream_cannot_name_a_deleted_highlight(dev, sim, h, tmp_path):
    device_logs(sim, [highlight("h1")])
    ops.pull(dev, h)
    device_logs(sim, [deletion("h1")])
    ops.pull(dev, h)
    draft = write_jsonl(tmp_path / "stream.jsonl", [{"id": "s1", "kind": "highlight", "highlight": "h1"}])
    with pytest.raises(InvalidData) as e:
        ops.check_local_stream(h, draft)
    assert "'h1' is not in the pulled highlights" in str(e.value)


def test_a_torn_archive_line_is_skipped_and_closed(dev, sim, h):
    device_logs(sim, [highlight("h1")])
    ops.pull(dev, h)
    path = h.archive("highlights")
    with path.open("a") as f:
        f.write('{"id":"h9","bo')  # a crash in the middle of an append
    device_logs(sim, [highlight("h2")])
    ops.pull(dev, h)
    assert [r["id"] for r in read_jsonl(path)] == ["h1", "h2"]
    assert path.read_text().splitlines()[1] == '{"id":"h9","bo'
