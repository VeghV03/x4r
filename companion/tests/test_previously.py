import json
import zipfile
from pathlib import Path

import pytest
from records import write_jsonl

from x4 import ops
from x4.context import context
from x4.errors import InvalidData, X4Error
from x4.home import write_json
from x4.previously import check_previously, excerpt, text_before

CONTAINER = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>"""

OPF = """<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Two Chapters</dc:title></metadata>
  <manifest>
    <item id="c1" href="text/one.xhtml" media-type="application/xhtml+xml"/>
    <item id="c2" href="text/two.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="c1"/><itemref idref="c2"/></spine>
</package>"""


def chapter(word: str, n: int) -> str:
    body = " ".join(f"{word}{i}" for i in range(n)) + " ‘őű’"
    return f"<html><head><style>p {{}}</style></head><body><p>{body}</p></body></html>"


def make_book(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", CONTAINER)
        zf.writestr("OEBPS/content.opf", OPF)
        zf.writestr("OEBPS/text/one.xhtml", chapter("a", 100))
        zf.writestr("OEBPS/text/two.xhtml", chapter("b", 100))
    return path


def recap(**over) -> dict:
    return {"book": "books/x.epub", "progress": 0.41, "text": "Ahol tartottál: a hős elindult."} | over


def test_valid_recaps_are_normalized(tmp_path):
    f = write_jsonl(tmp_path / "p.jsonl", [recap(text="  Ahol tartottál.  "), recap(book="books/y.epub", progress=1)])
    c = check_previously(f)
    assert c.count == 2
    assert c.records[0] == {"book": "books/x.epub", "progress": 0.41, "text": "Ahol tartottál."}
    assert [json.loads(line) for line in c.body.decode().splitlines()] == c.records


@pytest.mark.parametrize(
    ("bad", "message"),
    [
        (recap(book="/books/x.epub"), "book: must be a book key"),
        (recap(book="books/" + "x" * 510), "book: must be a book key"),
        (recap(book="books/read/a.epub"), "Read list articles get no recap"),
        (recap(progress=1.2), "progress: must be"),
        (recap(progress=True), "progress: must be"),
        (recap(text=""), "text: must be the recap"),
        (recap(text="szó " * 66), "text: is 66 words"),
        (recap(text="egy\nkettő"), "one paragraph"),
    ],
)
def test_invalid_recaps_name_the_field(tmp_path, bad, message):
    with pytest.raises(InvalidData) as e:
        check_previously(write_jsonl(tmp_path / "p.jsonl", [bad]))
    assert message in "\n".join(e.value.details)


def test_one_recap_per_book(tmp_path):
    with pytest.raises(InvalidData) as e:
        check_previously(write_jsonl(tmp_path / "p.jsonl", [recap(), recap()]))
    assert "already has a recap on line 1" in e.value.details[0]


def test_text_before_ends_at_the_progress(tmp_path):
    book = make_book(tmp_path / "x.epub")
    title, text = text_before(book, 0.75, 10)
    assert title == "Two Chapters"
    words = text.split()
    assert len(words) == 10 and all(w.startswith("b") for w in words)
    assert words[-1] in {f"b{i}" for i in range(45, 55)}  # about half way into chapter two
    _, end = text_before(book, 1.0, 1)
    assert end == "‘őű’"


def test_text_before_reaches_back_into_the_previous_chapter(tmp_path):
    _, text = text_before(make_book(tmp_path / "x.epub"), 0.52, 30)
    words = text.split()
    assert words[0].startswith("a") and words[-1].startswith("b") and len(words) == 30


def test_excerpt_keeps_the_book_for_the_writer_at_the_pulled_progress(dev, sim, h):
    make_book(sim.root / "books/x.epub")
    write_json(h.stats, {"books": {"books/x.epub": {"progress": 0.25}}})
    out = excerpt(dev, h, "/books/x.epub", words=5)
    assert out["book"] == "books/x.epub" and out["progress"] == 0.25 and out["words"] == 5
    assert out["text"].split()[-1].startswith("a")
    assert h.book("books/x.epub") == h.root / "books/x.epub" and h.book("books/x.epub").exists()
    (sim.root / "books/x.epub").unlink()
    assert excerpt(None, h, "books/x.epub", words=5) == out  # the cloud run, with no X4


def test_excerpt_without_the_book_or_the_x4(dev, sim, h):
    write_json(h.stats, {"books": {"books/y.epub": {"progress": 0.25}}})
    with pytest.raises(X4Error, match="plug in the X4"):
        excerpt(None, h, "books/y.epub")
    with pytest.raises(X4Error, match="not on the X4"):
        excerpt(dev, h, "books/y.epub")


def test_excerpt_needs_progress(dev, h):
    with pytest.raises(X4Error, match="no progress"):
        excerpt(dev, h, "books/x.epub")


def test_sync_pushes_recaps(dev, sim, h):
    write_jsonl(h.previously, [recap()])
    out = ops.sync(dev, h)
    assert "/x4/previously.jsonl" in out["pushed"]
    assert json.loads((sim.root / "x4/previously.jsonl").read_text()) == recap()
    assert "/x4/previously.jsonl" not in ops.sync(dev, h)["pushed"]


def test_context_shows_each_books_recap_progress(h):
    write_json(h.stats, {"books": {"books/x.epub": {"progress": 0.5}, "books/y.epub": {"progress": 0.1}}})
    write_jsonl(h.previously, [recap()])
    books = context(h)["books"]
    assert books["books/x.epub"]["previously"] == 0.41
    assert "previously" not in books["books/y.epub"]
