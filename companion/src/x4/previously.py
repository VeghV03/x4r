"""'Previously': one screen of where you were in a book, written by Claude at sync.

X4_HOME/previously.jsonl holds one recap per book, each tied to the book's progress when it
was written. The device shows it before the page when the book opens after days away and is
still at that progress.
"""

import posixpath
import re
import zipfile
from pathlib import Path

from lxml import etree, html

from .device import Device
from .errors import InvalidData, X4Error
from .home import Home, read_json
from .models import Checked, Stats, _by_line, _lines, jsonl_bytes

REMOTE = "/x4/previously.jsonl"
KEY_BYTES = 512  # an SD path; the device keys recaps by hash
WORDS_MAX = 65  # one screen in the reader font, with room for long words
TEXT_BYTES = 600
READ_BOOKS = "books/read/"


def check_previously(path: Path) -> Checked:
    """Validate X4_HOME/previously.jsonl. Raises InvalidData listing every problem."""
    name = str(path)
    rows, errors = _lines(name, path.read_bytes())
    records, seen = [], {}
    for n, obj in rows:
        where = f"{name}:{n}"
        found = []
        book, progress, text = obj.get("book"), obj.get("progress"), obj.get("text")
        if not isinstance(book, str) or not book or book.startswith("/") or len(book.encode()) > KEY_BYTES:
            found.append(f"{where}: book: must be a book key from x4 context, at most {KEY_BYTES} bytes, got {book!r}")
        elif book.startswith(READ_BOOKS):
            found.append(f"{where}: book: Read list articles get no recap, got {book!r}")
        elif book in seen:
            found.append(f"{where}: book: '{book}' already has a recap on line {seen[book]}; one per book")
        else:
            seen[book] = n
        if isinstance(progress, bool) or not isinstance(progress, int | float) or not 0 <= progress <= 1:
            found.append(f"{where}: progress: must be the book's progress from x4 context, 0 to 1, got {progress!r}")
        if not isinstance(text, str) or not text.strip():
            found.append(f"{where}: text: must be the recap, plain text")
        else:
            words = len(text.split())
            if words > WORDS_MAX or len(text.encode()) > TEXT_BYTES:
                found.append(
                    f"{where}: text: is {words} words, {len(text.encode())} bytes; one screen holds "
                    f"{WORDS_MAX} words, {TEXT_BYTES} bytes"
                )
            if "\n" in text:
                found.append(f"{where}: text: must be one paragraph, without line breaks")
        errors += found
        if not found:
            records.append({"book": book, "progress": round(float(progress), 4), "text": text.strip()})
    if errors:
        raise InvalidData(f"{name}: invalid recaps", _by_line(name, errors))
    return Checked("previously", jsonl_bytes(records), len(records), records)


# --- excerpt: the text just before the reader's position -------------------------------------


def _spine(epub: zipfile.ZipFile) -> tuple[str, list[str]]:
    """The book's title and its spine's paths in the zip, in reading order."""
    container = etree.fromstring(epub.read("META-INF/container.xml"))
    opf_path = container.find(".//{*}rootfile").get("full-path")
    opf = etree.fromstring(epub.read(opf_path))
    base = posixpath.dirname(opf_path)
    manifest = {i.get("id"): i.get("href") for i in opf.iterfind(".//{*}manifest/{*}item")}
    paths = []
    for ref in opf.iterfind(".//{*}spine/{*}itemref"):
        href = manifest.get(ref.get("idref"))
        if href:
            paths.append(posixpath.normpath(posixpath.join(base, href.split("#")[0])))
    title = opf.findtext(".//{*}metadata/{*}title") or ""
    return title.strip(), paths


def _text(data: bytes) -> str:
    try:
        # EPUB text is UTF-8; without a charset tag lxml would guess Latin-1.
        root = html.fromstring(data, parser=html.HTMLParser(encoding="utf-8"))
    except (etree.ParserError, ValueError):
        return ""
    for bad in root.xpath("//script|//style"):
        bad.drop_tree()
    return re.sub(r"\s+", " ", root.text_content()).strip()


def text_before(epub_path: Path, progress: float, words: int) -> tuple[str, str]:
    """The book's title and the last `words` words before `progress`, placed as the device
    measures progress: by the inflated size of each spine item."""
    with zipfile.ZipFile(epub_path) as epub:
        title, paths = _spine(epub)
        sizes = [epub.getinfo(p).file_size if p in epub.namelist() else 0 for p in paths]
        total = sum(sizes)
        if not total:
            return title, ""
        at = min(max(progress, 0.0), 1.0) * total
        index, before = 0, 0
        while index < len(sizes) - 1 and before + sizes[index] <= at:
            before += sizes[index]
            index += 1
        fraction = (at - before) / sizes[index] if sizes[index] else 1.0
        current = _text(epub.read(paths[index])) if sizes[index] else ""
        taken = current[: round(len(current) * fraction)].split()
        while len(taken) < words and index > 0:
            index -= 1
            if sizes[index]:
                taken = _text(epub.read(paths[index])).split() + taken
    return title, " ".join(taken[-words:])


def excerpt(dev: Device | None, h: Home, book: str, words: int = 2500, progress: float | None = None) -> dict:
    """The text the reader read last in BOOK, ending where they stopped (progress from the
    last pull, unless given). Reads the copy in X4_HOME/books/ (K14), fetching it from the X4
    the first time."""
    book = book.lstrip("/")
    if progress is None:
        stats = Stats.model_validate(read_json(h.stats, {}))
        found = stats.books.get(book)
        if found is None:
            raise X4Error(f"no progress for {book}: run x4 pull, or pass --progress")
        progress = found.progress
    folder, name = posixpath.split("/" + book)
    cached = h.book(book)
    if not cached.exists():
        if dev is None:
            raise X4Error(f"/{book} is not in {h.root / 'books'}: plug in the X4 and run x4 sync")
        try:
            size = {e["name"]: e["size"] for e in dev.ls(folder)}.get(name)
        except X4Error:
            size = None
        if size is None:
            raise X4Error(f"/{book} is not on the X4")
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_bytes(dev.get("/" + book))
    if not name.lower().endswith(".epub"):
        raise X4Error(f"{book}: only EPUB books have an excerpt")
    title, text = text_before(cached, progress, words)
    return {"book": book, "title": title, "progress": round(progress, 4), "words": len(text.split()), "text": text}
