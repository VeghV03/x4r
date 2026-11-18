"""Next books: near the end of a book, Claude picks up to three unread books to follow it.

X4_HOME/library holds the user's own EPUBs. X4_HOME/next.jsonl holds the picks, one record per
book they follow: {"after": <book key>, "books": [<card book key> or "library/<file>.epub"]}.
A library pick goes to the card as a sample of its first chapters, at the path the whole book
takes at the sync after the sample was read. The reader's end-of-book menu lists the picks.
"""

import io
import posixpath
import tempfile
import zipfile
from pathlib import Path

from lxml import etree

from .convert.epub import optimize_epub, write_zip
from .convert.names import slugify
from .device import Device
from .errors import DeviceError, InvalidData
from .home import Home, read_json, write_json
from .models import Checked, Stats, _by_line, _lines, jsonl_bytes
from .previously import _text

REMOTE = "/x4/next.jsonl"
LIBRARY = "library/"
PICKS_MAX = 3
SAMPLE_WORDS = 6000  # about 25 pages: the first chapter or two
READ_DIR = "books/read/"
CACHE_ROOT = "/.crosspoint"
_BLOCK = 32 * 1024
_OPF = "{http://www.idpf.org/2007/opf}"
_DC = "{http://purl.org/dc/elements/1.1/}"
_STUB = b'<?xml version="1.0" encoding="utf-8"?>\n<html xmlns="http://www.w3.org/1999/xhtml"><head><title></title></head><body></body></html>\n'


def cache_dir(card_path: str) -> str:
    """The device's cache folder of a book: libstdc++ std::hash of its path, 32-bit MurmurHash2."""
    data = ("/" + card_path.lstrip("/")).encode()
    m, n = 0x5BD1E995, len(data)
    h = (0xC70F6907 ^ n) & 0xFFFFFFFF
    i = 0
    while n >= 4:
        k = (int.from_bytes(data[i : i + 4], "little") * m) & 0xFFFFFFFF
        k = ((k ^ (k >> 24)) * m) & 0xFFFFFFFF
        h = ((h * m) & 0xFFFFFFFF) ^ k
        i, n = i + 4, n - 4
    if n == 3:
        h ^= data[i + 2] << 16
    if n >= 2:
        h ^= data[i + 1] << 8
    if n >= 1:
        h = ((h ^ data[i]) * m) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * m) & 0xFFFFFFFF
    return f"{CACHE_ROOT}/epub_{h ^ (h >> 15)}"


class RemoteFile(io.RawIOBase):
    """A card file read in ranges, so zipfile reads a book's metadata without fetching it all."""

    def __init__(self, dev: Device, path: str, size: int):
        self.dev, self.path, self.size, self.pos = dev, path, size, 0
        self.blocks: dict[int, bytes] = {}

    def seekable(self) -> bool:
        return True

    def readable(self) -> bool:
        return True

    def tell(self) -> int:
        return self.pos

    def seek(self, offset: int, whence: int = 0) -> int:
        self.pos = [offset, self.pos + offset, self.size + offset][whence]
        return self.pos

    def readinto(self, b) -> int:
        want = min(len(b), self.size - self.pos)
        out = bytearray()
        while len(out) < want:
            at = self.pos + len(out)
            start = at - at % _BLOCK
            if start not in self.blocks:
                self.blocks[start] = self.dev.get(self.path, start, min(_BLOCK, self.size - start))
            block = self.blocks[start]
            out += block[at - start : at - start + want - len(out)]
            if at - start >= len(block):
                break
        b[: len(out)] = out
        self.pos += len(out)
        return len(out)


def _opf(zf: zipfile.ZipFile) -> tuple[str, etree._Element]:
    container = etree.fromstring(zf.read("META-INF/container.xml"))
    name = container.find(".//{*}rootfile").get("full-path")
    return name, etree.fromstring(zf.read(name))


def metadata(zf: zipfile.ZipFile) -> dict[str, str]:
    _, opf = _opf(zf)
    meta = {"title": (opf.findtext(f".//{_DC}title") or "").strip()}
    author = (opf.findtext(f".//{_DC}creator") or "").strip()
    return meta | ({"author": author} if author else {})


# Folders of the system, the firmware and the app; everything else is the user's.
_NOT_BOOKS = {"System Volume Information", "XTCache", "Pushed Images", "x4", "dictionaries", "fonts", "sleep"}


def card_books(dev: Device) -> dict[str, int]:
    """EPUBs on the card by key, wherever the Library finds them, without the Read list."""
    found: dict[str, int] = {}

    def walk(folder: str, depth: int) -> None:
        for e in dev.ls(folder):
            name = e["name"]
            path = posixpath.join(folder, name).lstrip("/")
            if e.get("dir"):
                skip = name.startswith(".") or (depth == 0 and name in _NOT_BOOKS)
                if not skip and depth < 3 and not (path + "/").startswith(READ_DIR):
                    walk("/" + path, depth + 1)
            elif path.lower().endswith(".epub"):
                found[path] = e["size"]

    walk("/", 0)
    return found


def _started(stats: Stats, key: str) -> bool:
    b = stats.books.get(key)
    return b is not None and (b.read_ms > 0 or b.pages > 0 or b.progress > 0)


def _library_meta(path: Path) -> dict[str, str]:
    with zipfile.ZipFile(path) as zf:
        return metadata(zf)


def sample_path(h: Home, file: str) -> str:
    """Where a library book goes on the card: books/<its title as a slug>.epub."""
    path = h.library / file
    return f"books/{slugify(_library_meta(path)['title'] or path.stem)}.epub"


def _card_meta(dev: Device, key: str, size: int, cached: dict) -> dict:
    """A card book's title and author, read in ranges once and kept by size."""
    entry = cached.get(key)
    if not entry or entry.get("size") != size:
        with zipfile.ZipFile(io.BufferedReader(RemoteFile(dev, "/" + key, size), _BLOCK)) as zf:
            entry = {"size": size} | metadata(zf)
        cached[key] = entry
    return {k: entry[k] for k in ("title", "author") if entry.get(k)}


def _shelf_cache(h: Home) -> Path:
    return h.cache / "shelf.json"


def _save_cache(h: Home, cached: dict) -> None:
    h.cache.mkdir(parents=True, exist_ok=True)
    write_json(_shelf_cache(h), cached)


def shelf(dev: Device, h: Home) -> list[dict]:
    """The unread books Claude can pick from: on the card, then in X4_HOME/library."""
    stats = Stats.model_validate(read_json(h.stats, {}))
    cached = read_json(_shelf_cache(h), {})
    card = card_books(dev)
    out = []
    for key, size in sorted(card.items()):
        meta = _card_meta(dev, key, size, cached)
        if not _started(stats, key):
            out.append({"book": key} | meta)
    _save_cache(h, cached)
    for f in sorted(h.library.glob("*.epub")) if h.library.exists() else []:
        if sample_path(h, f.name) not in card:
            out.append({"book": LIBRARY + f.name} | _library_meta(f))
    return out


def check_next(path: Path) -> Checked:
    """Validate X4_HOME/next.jsonl's shape. Raises InvalidData listing every problem."""
    name = str(path)
    rows, errors = _lines(name, path.read_bytes())
    records, seen = [], {}
    for n, obj in rows:
        where = f"{name}:{n}"
        found = []
        after, books = obj.get("after"), obj.get("books")
        if not isinstance(after, str) or not after or after.startswith("/"):
            found.append(f"{where}: after: must be a book key from x4 context, got {after!r}")
        elif after in seen:
            found.append(f"{where}: after: '{after}' already has picks on line {seen[after]}; one record per book")
        else:
            seen[after] = n
        if not isinstance(books, list) or not 1 <= len(books) <= PICKS_MAX:
            found.append(f"{where}: books: must list 1 to {PICKS_MAX} books from x4 shelf, got {books!r}")
        else:
            for b in books:
                if not isinstance(b, str) or not b or b.startswith("/") or b == after:
                    found.append(f"{where}: books: each must be a book key from x4 shelf, not the book itself, got {b!r}")
            if len(set(map(str, books))) < len(books):
                found.append(f"{where}: books: lists a book twice")
        errors += found
        if not found:
            records.append({"after": after, "books": books})
    if errors:
        raise InvalidData(f"{name}: invalid picks", _by_line(name, errors))
    return Checked("next", jsonl_bytes(records), len(records), records)


def cut_sample(src: Path, dst: Path, words: int = SAMPLE_WORDS) -> int:
    """Copy an EPUB keeping its first chapters up to `words`; later chapters stay in the spine
    empty, so positions match the whole book. Images only later chapters use are dropped.
    Returns the chapters kept."""
    with zipfile.ZipFile(src) as zf:
        names = zf.namelist()
        contents = {n: zf.read(n) for n in names}
        opf_name, opf = _opf(zf)
    base = posixpath.dirname(opf_name)

    def full(href: str) -> str:
        return posixpath.normpath(posixpath.join(base, href.split("#")[0]))

    items = {i.get("id"): i for i in opf.iter(f"{_OPF}item")}
    spine = [full(items[r.get("idref")].get("href")) for r in opf.iter(f"{_OPF}itemref") if r.get("idref") in items]
    kept, total = 0, 0
    for doc in spine:
        if doc not in contents:
            continue
        if total >= words:
            contents[doc] = _STUB
            continue
        total += len(_text(contents[doc]).split())
        kept += 1
    text = b"".join(contents[d] for d in spine[: max(kept, 1)] if d in contents)
    cover = {m.get("content") for m in opf.iter(f"{_OPF}meta") if m.get("name") == "cover"}
    for item in list(items.values()):
        n = full(item.get("href", ""))
        if not item.get("media-type", "").startswith("image/") or n not in contents:
            continue
        if item.get("id") in cover or "cover-image" in (item.get("properties") or ""):
            continue
        if posixpath.basename(n).encode() not in text:
            item.getparent().remove(item)
            del contents[n]
    contents[opf_name] = etree.tostring(opf, xml_declaration=True, encoding="utf-8")
    order = ["mimetype"] + [n for n in names if n != "mimetype" and n in contents]
    contents.setdefault("mimetype", b"application/epub+zip")
    with tempfile.TemporaryDirectory() as tmp:
        cut = Path(tmp) / "sample.epub"
        write_zip(cut, [(n, contents[n]) for n in order])
        optimize_epub(cut, dst)
    return kept


def _clear_cache(dev: Device, card_path: str, keep: tuple[str, ...] = ()) -> None:
    """Remove a book's device cache, all of it or all but `keep`."""
    folder = cache_dir(card_path)
    try:
        entries = dev.ls(folder)
    except DeviceError as e:
        if e.code == "not_found":
            return
        raise
    if not keep:
        dev.rm(folder, recursive=True)
        return
    for e in entries:
        if e["name"] not in keep:
            dev.rm(f"{folder}/{e['name']}", recursive=bool(e.get("dir")))


def sync_next(dev: Device, h: Home, checked: Checked, put) -> tuple[bytes, dict]:
    """Send samples for new library picks, the whole book for each sample read since, and remove
    samples no longer picked and never opened. Returns the device's next.jsonl and a report.
    `put(remote, data)` uploads a file. Checks every pick before the card changes."""
    stats = Stats.model_validate(read_json(h.stats, {}))
    card = card_books(dev)
    cached = read_json(_shelf_cache(h), {})
    errors, records, wanted = [], [], {}  # wanted: sample key -> library file
    for r in checked.records:
        books = []
        for b in r["books"]:
            if b.startswith(LIBRARY):
                file = b.removeprefix(LIBRARY)
                if not (h.library / file).is_file():
                    errors.append(f"next: {r['after']}: {b} is not in {h.library}")
                    continue
                key = sample_path(h, file)
                title = _library_meta(h.library / file)["title"] or Path(file).stem
                if key not in card:
                    wanted[key] = file
            elif b in card:
                key, title = b, _card_meta(dev, b, card[b], cached).get("title") or Path(b).stem
            else:
                errors.append(f"next: {r['after']}: {b} is not on the card; pick from x4 shelf")
                continue
            books.append({"book": key, "title": title})
        records.append({"after": r["after"], "books": books})
    _save_cache(h, cached)
    if errors:
        raise InvalidData("picks name books that are not there", errors)

    state = read_json(h.sync_state, {})
    samples: dict[str, str] = state.get("samples", {})
    report: dict[str, list[str]] = {"samples": [], "whole": [], "removed": []}
    for key, file in sorted(samples.items()):
        if _started(stats, key) and (h.library / file).is_file():
            with tempfile.TemporaryDirectory() as tmp:
                whole = Path(tmp) / "whole.epub"
                optimize_epub(h.library / file, whole)
                put("/" + key, whole.read_bytes())
            _clear_cache(dev, key, keep=("progress.bin",))  # the reader's place carries over
            del samples[key]
            report["whole"].append(key)
    for key, file in sorted(wanted.items()):
        with tempfile.TemporaryDirectory() as tmp:
            sample = Path(tmp) / "sample.epub"
            cut_sample(h.library / file, sample)
            put("/" + key, sample.read_bytes())
        samples[key] = file
        report["samples"].append(key)
    picked = {b["book"] for r in records for b in r["books"]}
    for key in sorted(set(samples) - picked):
        if not _started(stats, key):
            if key in card:
                dev.rm("/" + key)
            _clear_cache(dev, key)
            report["removed"].append(key)
            del samples[key]
    state["samples"] = samples
    write_json(h.sync_state, state)
    return jsonl_bytes(records), report
