import json
import sys
import types
from pathlib import Path

import pytest
from records import DECK, highlight, write_jsonl

from x4 import ops
from x4.errors import InvalidData
from x4.home import read_json


@pytest.fixture
def stardict(monkeypatch):
    """A stand-in for x4.stardict so these tests do not depend on the converter."""
    calls = []

    def write_stardict(entries, out_dir: Path, name: str, bookname: str) -> list[Path]:
        entries = list(entries)
        calls.append((entries, name, bookname))
        out_dir.mkdir(parents=True, exist_ok=True)
        paths = []
        for ext in ("ifo", "idx", "dict"):
            p = out_dir / f"{name}.{ext}"
            p.write_text(json.dumps(entries) if ext == "dict" else ext)
            paths.append(p)
        return paths

    monkeypatch.setitem(sys.modules, "x4.stardict", types.SimpleNamespace(write_stardict=write_stardict))
    return calls


def test_push_deck_copies_into_home(dev, sim, h, tmp_path):
    src = write_jsonl(tmp_path / "elsewhere.jsonl", DECK)
    out = ops.push_deck(dev, h, src)
    assert out == {
        "deck": "meditations",
        "cards": 2,
        "remote": "/x4/decks/meditations.jsonl",
        "local": str(h.decks / "meditations.jsonl"),
    }
    assert (h.decks / "meditations.jsonl").read_bytes() == src.read_bytes()
    lines = (sim.root / "x4/decks/meditations.jsonl").read_text().splitlines()
    assert [json.loads(line) for line in lines] == DECK


def test_push_invalid_deck_uploads_nothing(dev, sim, h, tmp_path):
    src = write_jsonl(tmp_path / "d.jsonl", [DECK[0], {"id": "c1", "type": "flip"}])
    with pytest.raises(InvalidData):
        ops.push_deck(dev, h, src)
    assert not (sim.root / "x4").exists()
    assert not (h.decks / "meditations.jsonl").exists()


def test_push_deck_name_must_match_in_home(dev, h):
    src = write_jsonl(h.decks / "wrong.jsonl", DECK)
    with pytest.raises(InvalidData) as e:
        ops.push_deck(dev, h, src)
    assert "rename the file to meditations.jsonl" in e.value.details[0]


def test_push_stream_remembers_its_puzzles(dev, sim, h, tmp_path):
    src = write_jsonl(tmp_path / "s.jsonl", [{"id": "s-1", "kind": "riddle", "prompt": "P", "answer": "A", "book": "books/a.epub"}])
    ops.push_stream(dev, h, src)
    assert read_json(h.sync_state, {})["puzzles"]["s-1"] == {"kind": "riddle", "book": "books/a.epub"}
    assert (sim.root / "x4/stream.jsonl").exists() and h.stream.exists()


def test_push_glossary(dev, sim, h, tmp_path, stardict):
    src = tmp_path / "g.json"
    src.write_text(json.dumps({"logos": "Reason.", "apatheia": "Freedom from passion."}))
    (sim.root / "x4/glossary/b7561f646a8c4cfd").mkdir(parents=True)
    (sim.root / "x4/glossary/b7561f646a8c4cfd/stale.syn").write_text("old")
    out = ops.push_glossary(dev, h, "/books/meditations.epub", src)
    assert out["remote"] == "/x4/glossary/b7561f646a8c4cfd/" and out["terms"] == 2
    folder = sim.root / "x4/glossary/b7561f646a8c4cfd"
    assert sorted(p.name for p in folder.iterdir()) == [f"glossary.{e}" for e in ("dict", "idx", "ifo")]
    assert stardict[0][0] == [("apatheia", "Freedom from passion."), ("logos", "Reason.")]
    assert h.glossary_file("books/meditations.epub").exists()


def test_sync_pushes_only_changes(dev, sim, h, stardict):
    write_jsonl(h.decks / "meditations.jsonl", DECK)
    write_jsonl(h.stream, [{"id": "s1", "kind": "bite", "title": "T", "body": "B"}])
    h.glossary_file("books/meditations.epub").write_text(json.dumps({"logos": "Reason."}))

    first = ops.sync(dev, h)
    assert first["pushed"] == [
        "/x4/decks/meditations.jsonl",
        "/x4/stream.jsonl",
        "/x4/glossary/b7561f646a8c4cfd/",
    ]
    second = ops.sync(dev, h)
    assert second["pushed"] == [] and second["unchanged"] == 3

    write_jsonl(h.decks / "meditations.jsonl", DECK + [{"id": "c3", "type": "flip", "front": "f", "back": "b"}])
    assert ops.sync(dev, h)["pushed"] == ["/x4/decks/meditations.jsonl"]

    (sim.root / "x4/stream.jsonl").unlink()  # gone from the device: push again even if unchanged here
    assert ops.sync(dev, h)["pushed"] == ["/x4/stream.jsonl"]


def test_sync_deletes_decks_removed_locally(dev, sim, h):
    write_jsonl(h.decks / "meditations.jsonl", DECK)
    ops.sync(dev, h)
    (sim.root / "x4/decks/foreign.jsonl").write_text("{}")  # not ours: left alone
    (h.decks / "meditations.jsonl").unlink()
    out = ops.sync(dev, h)
    assert out["deleted"] == ["/x4/decks/meditations.jsonl"]
    assert sorted(p.name for p in (sim.root / "x4/decks").iterdir()) == ["foreign.jsonl"]
    assert read_json(h.sync_state, {})["pushed"] == {}


def test_sync_validates_everything_before_pushing(dev, sim, h):
    write_jsonl(h.decks / "meditations.jsonl", DECK)
    write_jsonl(h.decks / "bad.jsonl", [{"deck": "bad"}, {"id": "x", "type": "flip"}])
    write_jsonl(h.stream, [{"id": "s1", "kind": "bite"}])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert any("bad.jsonl:2: back: is required" in d for d in e.value.details)
    assert any("stream.jsonl:1: title: is required" in d for d in e.value.details)
    assert not (sim.root / "x4/decks").exists()


def test_push_book_rebases_remote(dev, sim, h, monkeypatch, tmp_path):
    from x4.convert import Upload

    def prepare_book(src: str, workdir: Path) -> list[Upload]:
        out = workdir / "book.epub"
        out.write_bytes(b"epub")
        return [Upload(out, "/books/book.epub")]

    monkeypatch.setitem(sys.modules, "x4.convert.books", types.SimpleNamespace(prepare_book=prepare_book))
    out = ops.push_book(dev, "x.pdf", to="/books/papers")
    assert out == {"uploaded": [{"remote": "/books/papers/book.epub", "bytes": 4}]}
    assert (sim.root / "books/papers/book.epub").read_bytes() == b"epub"
    assert not h.book("books/papers/book.epub").exists()  # no home given
    ops.push_book(dev, "x.pdf", to="/books/papers", h=h)
    assert (h.root / "books/papers/book.epub").read_bytes() == b"epub"


def test_sync_keeps_a_copy_of_each_book_read_in_for_the_writer(dev, sim, h):
    (sim.root / "books").mkdir(exist_ok=True)
    (sim.root / "books/a.epub").write_bytes(b"epub a")
    (sim.root / "x4").mkdir(exist_ok=True)
    (sim.root / "x4/stats.json").write_text(json.dumps({"books": {
        "books/a.epub": {"progress": 0.4}, "books/read/r-1.epub": {"progress": 1.0}, "books/gone.epub": {"progress": 0.1}}}))
    assert ops.sync(dev, h)["books"] == ["books/a.epub"]
    assert h.book("books/a.epub").read_bytes() == b"epub a"
    assert not (h.root / "books/read").exists()
    assert ops.sync(dev, h)["books"] == []


def test_missing_converter_is_a_clear_error(dev, monkeypatch):
    monkeypatch.setitem(sys.modules, "x4.convert.books", None)
    with pytest.raises(Exception) as e:
        ops.push_book(dev, "x.epub")
    assert "x4.convert.books.prepare_book is not available" in str(e.value)


def test_rss_needs_feeds_file(dev, h):
    with pytest.raises(InvalidData) as e:
        ops.rss_sync(dev, h)
    assert "feeds.yaml" in str(e.value)


def test_push_stream_fills_highlight_items(dev, sim, h, tmp_path):
    write_jsonl(h.archive("highlights"), [highlight("h1", text="You have power over your mind.")])
    src = write_jsonl(tmp_path / "s.jsonl", [{"id": "s1", "kind": "highlight", "highlight": "h1"}])
    ops.push_stream(dev, h, src)
    sent = json.loads((sim.root / "x4/stream.jsonl").read_text())
    assert sent == {
        "id": "s1",
        "kind": "highlight",
        "highlight": "h1",
        "text": "You have power over your mind.",
        "title": "Meditations",
    }
    assert json.loads(h.stream.read_text()) == {"id": "s1", "kind": "highlight", "highlight": "h1"}  # as written
    assert ops.sync(dev, h)["pushed"] == []  # filled body is what sync compares


def test_sync_rejects_unknown_highlight_item(dev, sim, h):
    write_jsonl(h.stream, [{"id": "s1", "kind": "highlight", "highlight": "h-missing"}])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert "'h-missing' is not in the pulled highlights" in e.value.details[0]
    assert not (sim.root / "x4/stream.jsonl").exists()


def test_sync_refuses_an_article_over_eight_pages(dev, sim, h):
    article = h.root / "articles" / "r-1.md"
    article.parent.mkdir(parents=True, exist_ok=True)
    article.write_text("---\ntitle: Long\nlang: hu\n---\n" + "szó " * 800)
    write_jsonl(h.read, [{"id": "r-1", "kind": "found", "file": "articles/r-1.md"}])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert any("800 words; cut it to 750 (8 reader pages)" in d for d in e.value.details)


@pytest.mark.parametrize(("kind", "cap", "pages"), [("translation", 450, 5), ("analysis", 450, 5)])
def test_sync_keeps_digests_and_analyses_short(dev, sim, h, kind, cap, pages):
    article = h.root / "articles" / "r-1.md"
    article.parent.mkdir(parents=True, exist_ok=True)
    article.write_text("---\ntitle: Hír\nlang: hu\n---\n" + "szó " * (cap + 1))
    write_jsonl(h.read, [{"id": "r-1", "kind": kind, "file": "articles/r-1.md"}])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert any(f"{cap + 1} words; cut it to {cap} ({pages} reader pages)" in d for d in e.value.details)
