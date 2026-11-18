"""The XTCH writer against a reader that mirrors firmware/lib/Xtc/Xtc/XtcParser.cpp line by line."""

import struct
from dataclasses import dataclass
from pathlib import Path

import pytest
from PIL import Image

from x4.convert import xtc

XTCH_MAGIC = 0x48435458
XTH_MAGIC = 0x00485458
HEADER = struct.Struct("<IBBHBBBBIQQQQII")  # XtcHeader, 56 bytes
ENTRY = struct.Struct("<QIHH")  # PageTableEntry, 16 bytes
PAGE = struct.Struct("<IHHBBIQ")  # XtgPageHeader, 22 bytes


@dataclass
class Parsed:
    page_count: int
    title: str
    author: str
    chapters: list[tuple[str, int, int]]
    pages: list[tuple[int, int, bytes]]  # width, height, bitmap


def parse(path: Path) -> Parsed:
    data = path.read_bytes()
    assert HEADER.size == 56 and ENTRY.size == 16 and PAGE.size == 22
    (magic, major, minor, count, _direction, has_meta, _thumbs, has_chapters, _current,
     _meta_off, table_off, data_off, _thumb_off, _chapter_off, _pad) = HEADER.unpack_from(data)  # fmt: skip
    assert magic == XTCH_MAGIC
    assert (major, minor) in ((1, 0), (0, 1))
    assert count > 0
    assert 0x30 <= table_off <= len(data) and count * 16 <= len(data) - table_off

    title = author = ""
    if has_meta:
        title = data[0x38 : 0x38 + 127].split(b"\0")[0].decode()
        author = data[0xB8 : 0xB8 + 63].split(b"\0")[0].decode()

    chapters = []
    if has_chapters == 1 and table_off >= 56:
        (chapter_off,) = struct.unpack_from("<Q", data, 0x30)  # read as u64, padding included
        if chapter_off and 56 <= chapter_off and chapter_off + 96 <= len(data):
            max_off = table_off if table_off > chapter_off else data_off if data_off > chapter_off else len(data)
            for i in range((max_off - chapter_off) // 96):
                raw = data[chapter_off + i * 96 : chapter_off + (i + 1) * 96]
                name = raw[:80].split(b"\0")[0].decode()
                start, end = struct.unpack_from("<HH", raw, 0x50)
                if not name and start == 0 and end == 0:
                    break
                start, end = max(start - 1, 0), max(end - 1, 0)
                if start >= count:
                    continue
                end = min(end, count - 1)
                if start <= end:
                    chapters.append((name, start, end))

    pages = []
    for i in range(count):
        offset, size, width, height = ENTRY.unpack_from(data, table_off + i * 16)
        p_magic, p_width, p_height, _mode, _comp, p_size, _md5 = PAGE.unpack_from(data, offset)
        assert p_magic == XTH_MAGIC
        bitmap_size = p_width * ((p_height + 7) // 8) * 2
        assert p_size == bitmap_size
        assert size == 22 + bitmap_size
        assert (width, height) == (p_width, p_height)
        pages.append((p_width, p_height, data[offset + 22 : offset + 22 + bitmap_size]))
    return Parsed(count, title, author, chapters, pages)


def pixel(width: int, height: int, bitmap: bytes, x: int, y: int) -> int:
    """XtcReaderActivity::renderPage getPixelValue."""
    col_bytes = (height + 7) // 8
    plane = width * col_bytes
    col = width - 1 - x
    byte = col * col_bytes + y // 8
    bit = 7 - y % 8
    return (((bitmap[byte] >> bit) & 1) << 1) | ((bitmap[plane + byte] >> bit) & 1)


def test_header_page_table_and_pixels(tmp_path: Path) -> None:
    page = Image.new("L", (xtc.WIDTH, xtc.HEIGHT), 255)
    for i, gray in enumerate((255, 170, 85, 0)):  # four horizontal bands, white to black
        page.paste(gray, (0, i * 200, xtc.WIDTH, (i + 1) * 200))
    page.putpixel((0, 0), 0)  # top-left marker
    page.putpixel((xtc.WIDTH - 1, 1), 0)  # top-right marker, second row
    second = Image.new("L", (xtc.WIDTH, xtc.HEIGHT), 255)
    chapters = [xtc.Chapter("Első fejezet", 0, 0), xtc.Chapter("Two", 1, 1)]
    out = tmp_path / "book.xtch"
    xtc.write_xtch(
        out,
        [xtc.quantize(page, dither=False), xtc.quantize(second, dither=False)],
        title="Árvíztűrő tükörfúrógép",
        author="Ada",
        chapters=chapters,
    )

    book = parse(out)
    assert book.page_count == 2
    assert book.title == "Árvíztűrő tükörfúrógép"
    assert book.author == "Ada"
    assert book.chapters == [("Első fejezet", 0, 0), ("Two", 1, 1)]
    width, height, bitmap = book.pages[0]
    assert (width, height) == (480, 800)
    # XTH values: 0 white, 2 light gray, 1 dark gray, 3 black.
    assert [pixel(width, height, bitmap, 240, y) for y in (100, 300, 500, 700)] == [0, 2, 1, 3]
    assert pixel(width, height, bitmap, 0, 0) == 3
    assert pixel(width, height, bitmap, 1, 0) == 0
    assert pixel(width, height, bitmap, 479, 1) == 3
    assert pixel(width, height, bitmap, 479, 0) == 0
    assert set(book.pages[1][2]) == {0}


def test_no_chapters_and_long_title(tmp_path: Path) -> None:
    out = tmp_path / "plain.xtch"
    xtc.write_xtch(out, [xtc.quantize(Image.new("L", (480, 800), 255), dither=True)], title="é" * 100)
    book = parse(out)
    assert book.chapters == []
    assert book.title == "é" * 63  # 127 bytes max, cut on a character boundary
    assert struct.unpack_from("<Q", out.read_bytes(), 0x30)[0] == 0


def test_dither_uses_only_four_levels_and_is_deterministic(tmp_path: Path) -> None:
    gradient = Image.linear_gradient("L").resize((480, 800))
    levels = xtc.quantize(gradient, dither=True)
    assert not any(levels.histogram()[4:])
    a, b = tmp_path / "a.xtch", tmp_path / "b.xtch"
    xtc.write_xtch(a, [levels], title="g")
    xtc.write_xtch(b, [xtc.quantize(gradient, dither=True)], title="g")
    assert a.read_bytes() == b.read_bytes()


def test_rejects_empty_book(tmp_path: Path) -> None:
    with pytest.raises(ValueError):
        xtc.write_xtch(tmp_path / "empty.xtch", [], title="none")
