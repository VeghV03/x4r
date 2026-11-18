import json
import zipfile
from pathlib import Path

import pytest
from PIL import Image
from records import write_jsonl

from x4 import ops
from x4.context import context
from x4.errors import InvalidData
from x4.home import write_json
from x4.nextbook import cache_dir, check_next, cut_sample, sample_path, shelf

CONTAINER = """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>"""


def png() -> bytes:
    import io

    out = io.BytesIO()
    Image.new("L", (40, 40), 128).save(out, "PNG")
    return out.getvalue()


def make_book(path: Path, title: str, chapters: int = 6, words: int = 2000, author: str = "A. Writer") -> Path:
    """`chapters` chapters of `words` words; chapter i shows image i, and there is a cover."""
    items = [f'<item id="c{i}" href="text/c{i}.xhtml" media-type="application/xhtml+xml"/>' for i in range(chapters)]
    items += [f'<item id="i{i}" href="images/fig{i}.png" media-type="image/png"/>' for i in range(chapters)]
    items.append('<item id="cover" href="images/cover.png" media-type="image/png"/>')
    opf = f"""<?xml version="1.0"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>{title}</dc:title><dc:creator>{author}</dc:creator>
  <meta name="cover" content="cover"/></metadata>
  <manifest>{"".join(items)}</manifest>
  <spine>{"".join(f'<itemref idref="c{i}"/>' for i in range(chapters))}</spine>
</package>"""
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", CONTAINER)
        zf.writestr("OEBPS/content.opf", opf)
        for i in range(chapters):
            body = " ".join(f"w{i}x{n}" for n in range(words))
            zf.writestr(
                f"OEBPS/text/c{i}.xhtml",
                f'<html xmlns="http://www.w3.org/1999/xhtml"><body><img src="../images/fig{i}.png"/><p>{body}</p></body></html>',
            )
            zf.writestr(f"OEBPS/images/fig{i}.png", png())
        zf.writestr("OEBPS/images/cover.png", png())
    return path


def test_cache_dir_is_the_devices_path_hash():
    assert cache_dir("books/read/r-2026-11-03-32.epub") == "/.crosspoint/epub_195233232"
    assert cache_dir("/books/read/r-2026-11-03-32.epub") == "/.crosspoint/epub_195233232"


def test_sample_keeps_the_first_chapters_and_every_position(tmp_path):
    src = make_book(tmp_path / "b.epub", "Whole", chapters=6, words=2000)
    out = tmp_path / "s.epub"
    assert cut_sample(src, out, words=3000) == 2
    with zipfile.ZipFile(out) as zf:
        names = set(zf.namelist())
        assert "w1x5" in zf.read("OEBPS/text/c1.xhtml").decode()
        assert b"<body></body>" in zf.read("OEBPS/text/c2.xhtml")
        opf = zf.read("OEBPS/content.opf").decode()
    assert opf.count("<itemref") == 6  # the spine is the whole book's
    assert {"OEBPS/images/fig0.png", "OEBPS/images/fig1.png", "OEBPS/images/cover.png"} <= names
    assert "OEBPS/images/fig2.png" not in names and "fig2.png" not in opf


@pytest.mark.parametrize(
    ("bad", "message"),
    [
        ({"after": "/books/x.epub", "books": ["a.epub"]}, "after: must be a book key"),
        ({"after": "x.epub", "books": []}, "books: must list 1 to 3"),
        ({"after": "x.epub", "books": ["a", "b", "c", "d"]}, "books: must list 1 to 3"),
        ({"after": "x.epub", "books": ["x.epub"]}, "not the book itself"),
        ({"after": "x.epub", "books": ["a.epub", "a.epub"]}, "lists a book twice"),
    ],
)
def test_invalid_picks_name_the_field(tmp_path, bad, message):
    with pytest.raises(InvalidData) as e:
        check_next(write_jsonl(tmp_path / "n.jsonl", [bad]))
    assert message in "\n".join(e.value.details)


def test_shelf_lists_unread_books_with_titles(dev, sim, h):
    make_book(sim.root / "Long file name -- Someone.epub", "Card Book", author="Someone")
    make_book(sim.root / "books/started.epub", "Started")
    make_book(sim.root / "books/read/r-1.epub", "An Article")
    make_book(sim.root / "Pushed Books/pushed.epub", "Pushed")
    make_book(sim.root / "x4/app.epub", "Not A Book")
    make_book(sim.root / ".crosspoint/cache.epub", "Not A Book Either")
    make_book(h.library / "mine.epub", "Mac Book")
    write_json(h.stats, {"books": {"books/started.epub": {"progress": 0.3}}})
    books = shelf(dev, h)
    assert books == [
        {"book": "Long file name -- Someone.epub", "title": "Card Book", "author": "Someone"},
        {"book": "Pushed Books/pushed.epub", "title": "Pushed", "author": "A. Writer"},
        {"book": "library/mine.epub", "title": "Mac Book", "author": "A. Writer"},
    ]


def device_picks(sim) -> list[dict]:
    return [json.loads(line) for line in (sim.root / "x4/next.jsonl").read_text().splitlines()]


def test_sync_sends_a_sample_then_the_whole_book_once_read(dev, sim, h):
    make_book(sim.root / "current.epub", "Current")
    make_book(sim.root / "unread.epub", "On The Card")
    make_book(h.library / "mine.epub", "Mac Book", chapters=6, words=4000)
    write_jsonl(h.next, [{"after": "current.epub", "books": ["library/mine.epub", "unread.epub"]}])
    key = sample_path(h, "mine.epub")
    assert key == "books/mac-book.epub"

    out = ops.sync(dev, h)
    assert out["next"]["samples"] == [key]
    assert device_picks(sim) == [
        {"after": "current.epub", "books": [{"book": key, "title": "Mac Book"}, {"book": "unread.epub", "title": "On The Card"}]}
    ]
    sample = (sim.root / key).stat().st_size
    assert sample < (h.library / "mine.epub").stat().st_size
    assert ops.sync(dev, h)["next"] == {"samples": [], "whole": [], "removed": []}

    # Read on in the sample: the next sync brings the whole book and keeps the reader's place.
    cache = sim.root / cache_dir(key).lstrip("/")
    (cache / "sections").mkdir(parents=True)
    (cache / "book.bin").write_bytes(b"stale")
    (cache / "progress.bin").write_bytes(b"\x01\x00\x05\x00")
    (sim.root / "x4").mkdir(exist_ok=True)
    (sim.root / "x4/stats.json").write_text(json.dumps({"books": {key: {"read_ms": 60000, "pages": 4}}}))
    out = ops.sync(dev, h)
    assert out["next"]["whole"] == [key]
    assert (sim.root / key).stat().st_size > sample
    assert sorted(p.name for p in cache.iterdir()) == ["progress.bin"]


def test_sync_removes_samples_no_longer_picked_and_never_opened(dev, sim, h):
    make_book(sim.root / "current.epub", "Current")
    make_book(h.library / "mine.epub", "Mac Book")
    write_jsonl(h.next, [{"after": "current.epub", "books": ["library/mine.epub"]}])
    ops.sync(dev, h)
    assert (sim.root / "books/mac-book.epub").exists()
    write_jsonl(h.next, [])
    assert ops.sync(dev, h)["next"]["removed"] == ["books/mac-book.epub"]
    assert not (sim.root / "books/mac-book.epub").exists()


def test_sync_checks_picks_before_the_card_changes(dev, sim, h):
    make_book(h.library / "mine.epub", "Mac Book")
    write_jsonl(h.next, [{"after": "current.epub", "books": ["library/mine.epub", "gone.epub"]}])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert "gone.epub is not on the card" in "\n".join(e.value.details)
    assert not (sim.root / "books/mac-book.epub").exists()


def test_context_shows_each_books_picks(h):
    write_json(h.stats, {"books": {"current.epub": {"progress": 0.8}}})
    write_jsonl(h.next, [{"after": "current.epub", "books": ["library/mine.epub"]}])
    assert context(h)["books"]["current.epub"]["next"] == ["library/mine.epub"]
