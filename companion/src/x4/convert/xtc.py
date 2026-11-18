"""XTCH writer: pre-rendered 2-bit pages, as read by firmware/lib/Xtc/Xtc/XtcParser.cpp.

Layout written here (all little-endian):

    0x000  header, 56 bytes (XtcHeader)
    0x038  metadata, 256 bytes: title[128] author[64] publisher[32] language[16]
           createTime u32, coverPage u16, chapterCount u16, reserved[8]
    0x138  chapters, 96 bytes each: name[80], startPage u16, endPage u16 (1-based,
           the firmware subtracts one), reserved[12]
    ...    page table, 16 bytes per page: offset u64, size u32, width u16, height u16
    ...    pages: 22-byte XTH header + two bit planes

XTH pixel values are not linear: 0 white, 1 dark gray, 2 light gray, 3 black
(XtcReaderActivity draws value 1 in both gray planes, value 2 in the MSB plane only).
"""

import hashlib
import struct
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from PIL import Image

WIDTH, HEIGHT = 480, 800
XTCH_MAGIC = b"XTCH"
XTH_MAGIC = b"XTH\0"
HEADER_SIZE = 56
METADATA_SIZE = 256
CHAPTER_SIZE = 96
PAGE_ENTRY_SIZE = 16
PAGE_HEADER_SIZE = 22

# Darkness index (0 white .. 3 black) -> XTH pixel value.
XTH_VALUE = (0, 2, 1, 3)
_PALETTE = (255, 170, 85, 0)  # gray for each darkness index


@dataclass(frozen=True)
class Chapter:
    name: str
    start: int  # 0-based page index
    end: int  # 0-based, inclusive


def _fixed(text: str, size: int) -> bytes:
    """UTF-8, cut on a character boundary, NUL-padded, always NUL-terminated."""
    data = text.encode()[: size - 1]
    data = data.decode(errors="ignore").encode()
    return data.ljust(size, b"\0")


def quantize(img: Image.Image, dither: bool) -> Image.Image:
    """Map a gray image to darkness indexes 0..3 (mode L), optionally Floyd-Steinberg dithered."""
    gray = img.convert("L")
    if not dither:
        return gray.point([min(3, (255 - v + 42) // 85) for v in range(256)])
    palette = Image.new("P", (1, 1))
    palette.putpalette([c for v in _PALETTE for c in (v, v, v)] + [0, 0, 0] * 252)
    indexed = gray.convert("RGB").quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG)
    return Image.frombytes("L", indexed.size, indexed.tobytes()).point([min(i, 3) for i in range(256)])


def pack_xth(levels: Image.Image) -> bytes:
    """Darkness indexes (mode L, 0..3) -> XTH bitmap: bit1 plane then bit2 plane.

    Each plane is column-major, columns right to left, 8 vertical pixels per byte, MSB on top.
    Rotating 90 degrees counterclockwise turns column x into row width-1-x, so a mode "1"
    image's row-packed bytes are exactly that layout.
    """
    planes = []
    for bit in (1, 0):
        lut = [255 if (XTH_VALUE[min(v, 3)] >> bit) & 1 else 0 for v in range(256)]
        plane = levels.point(lut).convert("1", dither=Image.Dither.NONE)
        planes.append(plane.transpose(Image.Transpose.ROTATE_90).tobytes())
    return b"".join(planes)


def xth_page(levels: Image.Image) -> bytes:
    bitmap = pack_xth(levels)
    md5 = hashlib.md5(bitmap).digest()[:8]
    header = XTH_MAGIC + struct.pack("<HHBBI", levels.width, levels.height, 0, 0, len(bitmap)) + md5
    return header + bitmap


def write_xtch(
    path: Path,
    pages: Iterable[Image.Image],
    *,
    title: str,
    author: str = "",
    language: str = "",
    chapters: Iterable[Chapter] = (),
) -> int:
    """Write pages (darkness-index images, see `quantize`) to an XTCH file. Returns page count."""
    encoded = [xth_page(page) for page in pages]
    if not encoded:
        raise ValueError("an XTCH book needs at least one page")
    if len(encoded) > 0xFFFF:
        raise ValueError("an XTCH book holds at most 65535 pages")
    chapter_list = [c for c in chapters if 0 <= c.start <= c.end < len(encoded)]

    metadata_offset = HEADER_SIZE
    chapter_offset = metadata_offset + METADATA_SIZE if chapter_list else 0
    table_offset = metadata_offset + METADATA_SIZE + CHAPTER_SIZE * len(chapter_list)
    data_offset = table_offset + PAGE_ENTRY_SIZE * len(encoded)

    header = struct.pack(
        "<4sBBHBBBBIQQQQII",
        XTCH_MAGIC,
        1,  # version major
        0,  # version minor
        len(encoded),
        0,  # read direction: left to right
        1,  # has metadata
        0,  # has thumbnails
        1 if chapter_list else 0,
        0,  # current page
        metadata_offset,
        table_offset,
        data_offset,
        0,  # thumbnail offset
        chapter_offset,
        0,  # padding; the firmware reads 0x30 as a u64, so this must stay 0
    )
    metadata = (
        _fixed(title, 128)
        + _fixed(author, 64)
        + _fixed("", 32)
        + _fixed(language, 16)
        + struct.pack("<IHH", 0, 0, len(chapter_list))
        + bytes(8)
    )
    chapter_bytes = b"".join(
        _fixed(c.name, 80) + struct.pack("<HH", c.start + 1, c.end + 1) + bytes(12) for c in chapter_list
    )
    table = bytearray()
    offset = data_offset
    for page in encoded:
        width, height = struct.unpack_from("<HH", page, 4)
        table += struct.pack("<QIHH", offset, len(page), width, height)
        offset += len(page)

    with path.open("wb") as f:
        f.write(header + metadata + chapter_bytes + table)
        for page in encoded:
            f.write(page)
    return len(encoded)
