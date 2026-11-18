import io
import zipfile
from pathlib import Path

import ebooklib
from ebooklib import epub as ebook
from PIL import Image

from x4.convert import Upload
from x4.convert.books import prepare_book
from x4.convert.epub import Book, Chapter, optimize_epub, write_epub

CONTAINER = b"""<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>"""

OPF = b"""<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="id">test-book</dc:identifier><dc:title>Test Book</dc:title><dc:language>en</dc:language>
<meta property="dcterms:modified">2020-01-01T00:00:00Z</meta>
</metadata>
<manifest>
<item id="nav" href="text/nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
<item id="ch1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>
<item id="css" href="style.css" media-type="text/css"/>
<item id="cover" href="images/cover.jpg" media-type="image/jpeg" properties="cover-image"/>
<item id="pic" href="images/pic.webp" media-type="image/webp"/>
</manifest>
<spine><itemref idref="ch1"/></spine>
</package>"""

NAV = b"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Nav</title></head>
<body><nav epub:type="toc"><ol><li><a href="ch1.xhtml">One</a></li></ol></nav></body></html>"""

CHAPTER = b"""<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>One</title><link rel="stylesheet" href="../style.css"/></head>
<body><h1>One</h1><p>Text stays exactly as it was.</p>
<img src="../images/cover.jpg" alt="cover"/><img src="../images/pic.webp" alt="pic"/></body></html>"""

CSS = b"body { margin: 0 } .hero { background: url('images/pic.webp') }"


def picture(width: int, height: int) -> Image.Image:
    return Image.blend(
        Image.linear_gradient("L").resize((width, height)).convert("RGB"),
        Image.effect_noise((width, height), 50).convert("RGB"),
        0.5,
    )


def encode(img: Image.Image, fmt: str) -> bytes:
    buf = io.BytesIO()
    img.save(buf, fmt)
    return buf.getvalue()


def make_epub(path: Path) -> None:
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        zf.writestr("META-INF/container.xml", CONTAINER)
        zf.writestr("OEBPS/content.opf", OPF)
        zf.writestr("OEBPS/text/nav.xhtml", NAV)
        zf.writestr("OEBPS/text/ch1.xhtml", CHAPTER)
        zf.writestr("OEBPS/style.css", CSS)
        zf.writestr("OEBPS/images/cover.jpg", encode(picture(1600, 2400), "JPEG"))
        zf.writestr("OEBPS/images/pic.webp", encode(picture(1200, 800), "WEBP"))


def test_optimize_epub_round_trip(tmp_path: Path) -> None:
    src, dst = tmp_path / "in.epub", tmp_path / "out.epub"
    make_epub(src)
    assert optimize_epub(src, dst) == 2

    with zipfile.ZipFile(dst) as zf:
        names = zf.namelist()
        assert names[0] == "mimetype" and zf.getinfo("mimetype").compress_type == zipfile.ZIP_STORED
        assert "OEBPS/images/pic.jpg" in names and "OEBPS/images/pic.webp" not in names
        assert zf.read("OEBPS/text/nav.xhtml") == NAV  # untouched files are byte-identical
        chapter = zf.read("OEBPS/text/ch1.xhtml")
        assert b'src="../images/pic.jpg"' in chapter and b'src="../images/cover.jpg"' in chapter
        assert b"Text stays exactly as it was." in chapter
        assert b"url('images/pic.jpg')" in zf.read("OEBPS/style.css")
        for name in ("OEBPS/images/cover.jpg", "OEBPS/images/pic.jpg"):
            img = Image.open(io.BytesIO(zf.read(name)))
            assert img.mode == "L" and img.width <= 480 and img.height <= 800

    book = ebook.read_epub(str(dst), options={"ignore_ncx": True})
    images = {item.file_name: item.media_type for item in book.get_items() if item.media_type.startswith("image/")}
    assert images == {"images/cover.jpg": "image/jpeg", "images/pic.jpg": "image/jpeg"}
    assert book.get_metadata("DC", "title")[0][0] == "Test Book"

    again = tmp_path / "again.epub"
    optimize_epub(src, again)
    assert again.read_bytes() == dst.read_bytes()


def test_write_epub_is_valid_and_deterministic(tmp_path: Path) -> None:
    png = encode(Image.new("L", (10, 10), 0), "PNG")
    book = Book(
        title="Árvíz & <Tűz>",
        author="Jane",
        chapters=[Chapter("One", "<h1>One</h1><p>Hello.</p>"), Chapter("", "<p>More.</p>", in_toc=False)],
        images={"a.png": (png, "image/png")},
        date="2026-11-03",
        source="https://example.com/x",
    )
    a, b = tmp_path / "a.epub", tmp_path / "b.epub"
    write_epub(book, a)
    write_epub(book, b)
    assert a.read_bytes() == b.read_bytes()
    read = ebook.read_epub(str(a))
    assert read.get_metadata("DC", "title")[0][0] == "Árvíz & <Tűz>"
    assert [i.file_name for i in read.get_items_of_type(ebooklib.ITEM_IMAGE)] == ["images/a.png"]
    assert len(list(read.get_items_of_type(ebooklib.ITEM_DOCUMENT))) == 3  # nav + 2 chapters


def test_prepare_book_epub(tmp_path: Path) -> None:
    src = tmp_path / "My Great Book.epub"
    make_epub(src)
    uploads = prepare_book(str(src), tmp_path / "work")
    assert uploads == [Upload(tmp_path / "work" / "my-great-book.epub", "/books/my-great-book.epub")]
    assert uploads[0].local.exists()
