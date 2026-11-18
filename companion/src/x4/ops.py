"""Operations behind the CLI and the MCP server. Each returns a JSON-ready dict."""

import io
import json
import os
import posixpath
import re
import shutil
import tempfile
import unicodedata
import zlib
from collections.abc import Callable
from dataclasses import dataclass, replace
from datetime import date
from pathlib import Path
from typing import Literal

from PIL import Image
from pydantic import ValidationError

from .device import Device
from .errors import DeviceError, InvalidData, Refused, X4Error
from .home import Home, commit_pending, glossary_folder, pending, read_json, read_jsonl, write_json
from .models import (
    DEVICE_RECORDS,
    PUZZLES,
    Checked,
    Highlight,
    ReadSource,
    Stats,
    check_deck,
    check_glossary,
    check_read,
    check_stream,
    check_threads,
    jsonl_bytes,
)
from .notes import render_all
from . import sections as paper
from .readlist import READ_DIR, READ_LIST, Ready, already_sent, too_long

DECKS = "/x4/decks"
CROSSWORDS = "/x4/crosswords"
STREAM = "/x4/stream.jsonl"
THREADS = "/x4/threads.jsonl"
GLOSSARY = "/x4/glossary"
STATS = "/x4/stats.json"
LOG_NAMES = ("highlights", "reviews", "seen", "play", "read-log", "lookups", "kept", "visits")
LOGS = {name: f"/x4/{name}.jsonl" for name in LOG_NAMES}
PULLABLE = (*LOG_NAMES, "stats")
Pullable = Literal["highlights", "reviews", "seen", "play", "read-log", "lookups", "kept", "visits", "stats", "all"]
KEEP = 300  # read and puzzle items remembered for x4 context


# device files

# What the X4 writes (docs/DATA.md). `rm` and `put` leave these and the folders holding them
# alone unless forced: pull and sync are the only host side that touches them.
DEVICE_OWNED = (*LOGS.values(), "/x4/stats.json", "/x4/agenda.json", "/x4/state", "/.crosspoint")


def _guard(remote: str, force: bool) -> None:
    if force:
        return
    path = posixpath.normpath("/" + remote.strip().lstrip("/")).lower()  # FAT ignores case
    inside = path.rstrip("/") + "/"
    for owned in DEVICE_OWNED:
        o = owned.lower()
        if path == o or o.startswith(inside) or path.startswith(o + "/"):
            raise Refused(f"{remote}: the X4 writes {owned}; pull and sync handle it")


def _lines(data: bytes) -> int:
    return sum(1 for line in data.splitlines() if line.strip())


def _device_records(dev: Device, name: str) -> list[dict]:
    """A log's records still on the X4; a line that does not parse is skipped."""
    out = []
    for line in (dev.get(LOGS[name]) if dev.stat(LOGS[name]) else b"").decode("utf-8", errors="replace").splitlines():
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if isinstance(obj, dict):
            out.append(obj)
    return out


def status(dev: Device, h: Home | None = None) -> dict:
    """Device state from hello, plus counts of records not yet pulled. With the home: `minutes`,
    the last 7 days per section, from the pulled records and those still on the X4."""
    from .pieces import minutes

    info = dev.info
    out = {k: info.get(k) for k in ("fw", "battery", "battery_mv", "charging", "sd_free", "sd_total", "day", "issue")}
    for k in ("retention", "heap_free", "heap_min", "heap_block"):
        if k in info:
            out[k] = info[k]
    if low := dev.heap().get("low"):
        out["heap_low"] = low  # the stage that had just run at each new low in free memory
    out["port"] = dev.port
    if "net" in info:
        out["net"] = info["net"]
    for name in ("highlights", "reviews"):
        out[f"new_{name}"] = _lines(dev.get(LOGS[name])) if dev.stat(LOGS[name]) else 0
    if h is not None:
        order = [s.id for s in paper.load(h) or []]
        out["minutes"] = minutes(h, order, {n: _device_records(dev, n) for n in ("read-log", "visits", "play")})
    return out


def ensure_dir(dev: Device, path: str, made: set[str] | None = None) -> None:
    if path in ("", "/") or (made is not None and path in made):
        return
    dev.mkdir(path)
    if made is not None:
        made.add(path)


def put_bytes(dev: Device, remote: str, data: bytes, made: set[str] | None = None) -> dict:
    ensure_dir(dev, posixpath.dirname(remote), made)
    dev.put(remote, data)
    return {"remote": remote, "bytes": len(data)}


def upload_all(dev: Device, uploads) -> list[dict]:
    """Upload `x4.convert.Upload` items, creating folders as needed."""
    made: set[str] = set()
    return [
        put_bytes(dev, u.remote, Path(u.local).read_bytes(), made) | ({"note": u.note} if u.note else {})
        for u in uploads
    ]


def ls(dev: Device, path: str = "/") -> dict:
    return {"path": path, "entries": dev.ls(path)}


def get(dev: Device, remote: str, local: Path | None = None) -> dict:
    local = local or Path(posixpath.basename(remote.rstrip("/")) or "x4-file")
    if local.is_dir():
        local = local / posixpath.basename(remote)
    data = dev.get(remote)
    local.write_bytes(data)
    return {"remote": remote, "local": str(local), "bytes": len(data)}


def put(dev: Device, local: Path, remote: str, force: bool = False) -> dict:
    if not local.is_file():
        raise X4Error(f"{local}: no such file")
    if remote.endswith("/"):
        remote += local.name
    _guard(remote, force)
    return put_bytes(dev, remote, local.read_bytes())


def rm(dev: Device, remote: str, recursive: bool = False, force: bool = False) -> dict:
    _guard(remote, force)
    dev.rm(remote, recursive)
    return {"removed": remote}


def press(dev: Device, button: str, long: bool = False) -> dict:
    dev.press(button, long)
    return {"button": button, "long": long, **dev.screen()}


def screen(dev: Device) -> dict:
    """The activity on screen. On a step (a question after a chapter, a piece's question, the bite,
    a book puzzle) also `step`: its `kind`, its `id` (a stream item's id, or a card key), its
    `phase` (prompt, revealed, answered, empty), `done` and `total` steps, and the `selected` row
    or the option `picked`."""
    return dev.screen()


def lag(dev: Device) -> dict:
    """The last presses, oldest first, each timed from button-down in ms, and the marks of the latest."""
    return dev.lag()


def frame_to_image(frame: bytes, width: int, height: int) -> Image.Image:
    """The framebuffer as an image.

    Assumed layout: rows of width/8 bytes, top row first, most significant bit is the
    leftmost pixel, 0 is black. If real hardware differs, fix it here only.
    """
    return Image.frombytes("1", (width, height), frame)


def screenshot(dev: Device) -> tuple[bytes, dict]:
    """PNG bytes of the screen, and the frame header."""
    head, frame = dev.screenshot()
    if head.get("bpp", 1) != 1:
        raise DeviceError("protocol", f"screenshot: unsupported bpp {head.get('bpp')}")
    image = frame_to_image(frame, int(head["width"]), int(head["height"]))
    if rotate := int(head.get("rotate", 0)) % 360:
        image = image.rotate(-rotate, expand=True)  # PIL turns counter-clockwise
    buf = io.BytesIO()
    image.save(buf, "PNG")
    return buf.getvalue(), head


# pull


def _key(name: str, r: dict):
    if name == "highlights":
        return r.get("id")
    if name == "seen":
        # A piece's guess and its link's answer are two lines under one id.
        return (r.get("id"), "predict" in r, "link" in r)
    if name == "reviews":
        return (r.get("card"), r.get("set"), r.get("day"), r.get("at"), r.get("ms"))
    if name == "play":
        return (r.get("game"), r.get("lang"), r.get("mode"), r.get("level"), r.get("set"), r.get("day"), r.get("at"), r.get("ms"))
    if name == "read-log":
        return (r.get("book"), r.get("set"), r.get("day"), r.get("at"), r.get("ms"), r.get("progress"))
    if name == "lookups":
        return (r.get("word"), r.get("book"), r.get("set"), r.get("day"), r.get("at"))
    if name == "kept":
        return (r.get("id"), r.get("kept"), r.get("set"), r.get("day"), r.get("at"))
    if name == "visits":
        return (r.get("section"), r.get("issue"), r.get("ms"), r.get("day"), r.get("at"))
    return json.dumps(r, sort_keys=True)


def _append(path: Path, lines: list[str]) -> None:
    if not lines:
        return
    with path.open("a+b") as f:
        # A crash mid-append leaves a line without its newline; end it so the next record stays whole.
        if f.tell() and (f.seek(-1, os.SEEK_END), f.read(1))[1] != b"\n":
            f.write(b"\n")
        f.write("".join(line + "\n" for line in lines).encode("utf-8"))
        f.flush()
        os.fsync(f.fileno())


def _archive(h: Home, name: str, data: bytes) -> dict:
    """Append records not seen before to pulled/<name>.jsonl. Unreadable lines go to <name>.bad.

    Highlights are deduplicated against the live ones: a deletion record is kept only for a live
    highlight, and a highlight deleted earlier can come back."""
    path = h.archive(name)
    if name == "highlights":
        live = set(fold(highlight_log(h)))
        seen = set()
    else:
        seen = {_key(name, r) for r in read_jsonl(path)}
    new, bad, dupes, deleted = [], [], 0, 0
    for raw in data.decode("utf-8", "replace").splitlines():
        if not raw.strip():
            continue
        try:
            record = json.loads(raw)
            DEVICE_RECORDS[name].model_validate(record)
        except (ValueError, ValidationError):
            bad.append(raw)
            continue
        if name == "highlights":
            hid = record["id"]
            if record.get("deleted") is True:
                if hid not in live:  # never pulled, or already gone
                    dupes += 1
                    continue
                live.discard(hid)
                deleted += 1
            elif hid in live:
                dupes += 1
                continue
            else:
                live.add(hid)
        else:
            key = _key(name, record)
            if key in seen:
                dupes += 1
                continue
            seen.add(key)
        new.append(json.dumps(record, ensure_ascii=False, separators=(",", ":")))
    _append(path, new)
    _append(h.pulled / f"{name}.bad", bad)
    out = {"new": len(new) - deleted, "duplicates": dupes, "unreadable": len(bad)}
    return out | {"deleted": deleted} if name == "highlights" else out


def _pull_log(dev: Device, h: Home, name: str) -> dict:
    """Rename the log, archive the renamed file, then remove it (DATA.md). Records the device
    writes meanwhile go to a new log. A `.pulling` file left by an interrupted pull goes first."""
    remote = LOGS[name]
    pulling = remote.removesuffix(".jsonl") + ".pulling"
    total = {"new": 0, "duplicates": 0, "unreadable": 0} | ({"deleted": 0} if name == "highlights" else {})

    def drain() -> None:
        result = _archive(h, name, dev.get(pulling))
        dev.rm(pulling)  # only after the archive is on disk
        for k in total:
            total[k] += result[k]

    if dev.stat(pulling) is not None:
        drain()
    if dev.stat(remote) is not None:
        dev.mv(remote, pulling)
        drain()
    return total


def highlight_log(h: Home) -> list[Highlight]:
    """Every pulled highlights.jsonl record in order, deletion records included."""
    return [Highlight.model_validate(r) for r in read_jsonl(h.archive("highlights"))]


def fold(log: list[Highlight]) -> dict[str, Highlight]:
    """Id -> live highlight, in the order they were made: deletion records remove theirs."""
    live: dict[str, Highlight] = {}
    for x in log:
        if x.deleted:
            live.pop(x.id, None)
        else:
            live.setdefault(x.id, x)
    return live


def highlights(h: Home) -> list[Highlight]:
    """The pulled highlights not deleted on the device."""
    return list(fold(highlight_log(h)).values())


def read_ids(h: Home) -> set[str]:
    """Ids in X4_HOME/read.jsonl, read leniently (check_read reports its errors)."""
    return _ids(h.read)


def _ids(path: Path) -> set[str]:
    ids = set()
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if isinstance(obj, dict) and isinstance(obj.get("id"), str):
                ids.add(obj["id"])
    return ids


def _same(text: str) -> str:
    """Text as a quote is compared: typographic quotes and dashes, case and spacing set aside."""
    text = unicodedata.normalize("NFKC", text).translate(_PLAIN)
    return " ".join(text.split()).casefold()


_PLAIN = str.maketrans({"\u2018": "'", "\u2019": "'", "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u2013": "-", "\u2014": "-"})


def _predict_errors(h: Home, name: str, records: list[dict]) -> list[str]:
    """A predict's answer is Claude's claim about the article. Its `quote` must be in the
    article's text, so the claim at least points at a passage that is there."""
    from .readlist import article

    errors = []
    for r in records:
        predict = r.get("predict") if r["kind"] == "article" else None
        if not predict:
            continue
        where = f"{name}: {r['id']}: predict"
        book = Path(r["book"])
        if book.parent.as_posix() != READ_DIR.lstrip("/"):
            errors.append(f"{where}: needs a Read list article (book {READ_DIR.lstrip('/')}/<read id>.epub), not {r['book']}")
            continue
        try:
            text = article(h, book.stem)["text"]
        except X4Error as e:
            errors.append(f"{where}: {e}; a predict needs an article x4 can read as text")
            continue
        if _same(predict["quote"]) not in _same(text):
            errors.append(f"{where}.quote: not in the article; copy the sentence that settles it from `x4 article {book.stem}`")
    return errors


def check_local_stream(h: Home, file: Path) -> Checked:
    """Validate a stream, highlight items filled from the pulled highlights."""
    return check_stream(file, highlights={x.id: x for x in highlights(h)})


INBOX_FILE = re.compile(r"\d{8}T\d{6}Z-(" + "|".join(LOG_NAMES) + r"|stats|taken)\.jsonl?")


def take_inbox(h: Home) -> dict:
    """Take in what the X4 sent up to the data repo's inbox/ (docs/DATA.md): its logs into
    pulled/ as a cable pull's would go, the newest stats.json, and what it took of the issue.
    In name order, which is the order they were sent. A file is removed once it is in; one
    that cannot be read stays and is named in `skipped`."""
    from .lan import taken

    out: dict = {}
    if not h.repo_inbox.is_dir():
        return out
    for f in sorted(h.repo_inbox.iterdir()):
        m = INBOX_FILE.fullmatch(f.name)
        if not m:
            continue
        kind, data = m[1], f.read_bytes()
        if kind in ("stats", "taken"):
            try:
                body = json.loads(data)
                if not isinstance(body, dict):
                    raise ValueError
            except ValueError:
                out.setdefault("skipped", []).append(f.name)
                continue
            if kind == "stats":
                h.stats.write_bytes(data)
                out["stats"] = True
            else:
                taken(h, body)
                out["taken"] = body.get("issue")
        else:
            got = _archive(h, kind, data)
            total = out.setdefault(kind, dict.fromkeys(got, 0))
            for k, v in got.items():
                total[k] += v
        f.unlink()
    if "highlights" in out:
        log = highlight_log(h)
        out["notes"] = [str(p) for p in render_all(h, list(fold(log).values()), [x for x in log if not x.deleted])]
    return out


def begin(h: Home) -> dict:
    """Before anything is read or written in a home that is a checkout of the data repo: bring
    it up to the repo, take in what the X4 sent there, and send that at once, so no other
    writer takes the same files in. Returns what the inbox held."""
    from .datarepo import fetch_first, send_after

    fetch_first(h)
    took = take_inbox(h)
    if took.keys() - {"skipped"}:
        send_after(h, "inbox taken in")
    return took


def pull(dev: Device, h: Home, what: Pullable = "all") -> dict:
    """Copy device logs into pulled/ archives, delete them on the device, render notes. A home
    that is a checkout of the data repo is brought up to the repo first, and what the X4 sent
    there over Wi-Fi is taken in before what is still on the X4."""
    names = PULLABLE if what == "all" else (what,)
    out: dict = {}
    if inbox := begin(h):
        out["inbox"] = inbox
    for name in names:
        if name == "stats":
            if dev.stat(STATS):
                data = dev.get(STATS)
                try:
                    json.loads(data)
                except ValueError:
                    raise DeviceError("io", f"get {STATS}: not valid JSON; not saved") from None
                h.stats.write_bytes(data)
                out["stats"] = True
            else:
                out["stats"] = False
        else:
            out[name] = _pull_log(dev, h, name)
    if "highlights" in names:
        log = highlight_log(h)
        made = [x for x in log if not x.deleted]
        out["notes"] = [str(p) for p in render_all(h, list(fold(log).values()), made)]
    return out


# push


def _state(h: Home) -> dict:
    state = read_json(h.sync_state, {})
    state.setdefault("pushed", {})
    return state


def _recent(old: dict, new: dict) -> dict:
    """Merge, newest last, keeping the last KEEP entries."""
    merged = {k: v for k, v in old.items() if k not in new} | new
    return dict(list(merged.items())[-KEEP:])


def _check_deck(h: Home, file: Path) -> Checked:
    c = check_deck(file)
    if file.resolve().parent == h.decks.resolve() and file.stem != c.id:
        raise InvalidData(
            f"{file}: invalid deck",
            [f"{file}:1: deck: is '{c.id}' but the file is {file.name}; rename the file to {c.id}.jsonl"],
        )
    return c


def _save(src: Path, dst: Path) -> None:
    """Keep X4_HOME the source of truth: copy files pushed from elsewhere into it."""
    if src.resolve() != dst.resolve():
        dst.write_bytes(src.read_bytes())


def push_deck(dev: Device, h: Home, file: Path, checked: Checked | None = None) -> dict:
    """Validate a deck and upload it to /x4/decks/<deck-id>.jsonl."""
    c = checked or _check_deck(h, file)
    local = h.decks / f"{c.id}.jsonl"
    _save(file, local)
    remote = f"{DECKS}/{c.id}.jsonl"
    put_bytes(dev, remote, c.body)
    state = _state(h)
    state["pushed"][remote] = zlib.crc32(c.body)
    write_json(h.sync_state, state)
    return {"deck": c.id, "cards": c.count, "remote": remote, "local": str(local)}


def push_stream(dev: Device, h: Home, file: Path, checked: Checked | None = None) -> dict:
    """Validate a stream and replace /x4/stream.jsonl with it."""
    c = checked or check_local_stream(h, file)
    _save(file, h.stream)
    put_bytes(dev, STREAM, c.body)
    state = _state(h)
    state["pushed"][STREAM] = zlib.crc32(c.body)
    _note_stream(state, c)
    write_json(h.sync_state, state)
    return {"items": c.count, "remote": STREAM, "local": str(h.stream)}


def _note_stream(state: dict, c: Checked) -> None:
    """Remember the stream's puzzles: results come back in seen.jsonl by id, after the item has
    left the stream."""
    puzzles = {r["id"]: {k: r[k] for k in ("kind", "book") if r.get(k)} for r in c.records if r["kind"] in PUZZLES}
    state["puzzles"] = _recent(state.get("puzzles", {}), puzzles)


def _glossary_crc(entries: dict[str, str]) -> int:
    return zlib.crc32(json.dumps(entries, ensure_ascii=False, sort_keys=True).encode())


def _book_titles(h: Home) -> dict[str, str]:
    return {x.book: x.title for x in highlights(h) if x.title}


def push_glossary(dev: Device, h: Home, book_key: str, file: Path) -> dict:
    """Validate a glossary JSON, build StarDict files, replace /x4/glossary/<key hash>/."""
    book_key = book_key.strip("/")
    if not book_key:
        raise InvalidData("book key is empty", ["book_key: pass the book's SD path, e.g. books/meditations.epub"])
    entries = check_glossary(file)
    local = h.glossary_file(book_key)
    _save(file, local)
    write_stardict = _converter("stardict", "write_stardict")
    remote = f"{GLOSSARY}/{glossary_folder(book_key)}"
    with tempfile.TemporaryDirectory() as tmp:
        title = _book_titles(h).get(book_key) or Path(book_key).stem
        files = write_stardict(sorted(entries.items()), Path(tmp), "glossary", title)
        if dev.stat(remote):
            dev.rm(remote, recursive=True)
        made: set[str] = set()
        for f in files:
            put_bytes(dev, f"{remote}/{Path(f).name}", Path(f).read_bytes(), made)
    state = _state(h)
    state["pushed"][remote + "/"] = _glossary_crc(entries)
    write_json(h.sync_state, state)
    return {"book": book_key, "terms": len(entries), "remote": remote + "/", "local": str(local)}


def _converter(module: str, name: str) -> Callable:
    try:
        mod = __import__(f"x4.{module}", fromlist=[name])
        return getattr(mod, name)
    except (ImportError, AttributeError) as e:
        raise X4Error(f"x4.{module}.{name} is not available yet: {e}") from e


def keeps_book(remote: str) -> bool:
    """A book the writer gets a copy of in X4_HOME/books/ (K14): an EPUB, not a Read article."""
    return remote.lower().endswith(".epub") and not remote.startswith(READ_DIR + "/")


def push_book(dev: Device, src: str, to: str = "/books/", h: Home | None = None) -> dict:
    """Convert a book (.epub, .pdf or http(s) URL) and upload it under `to`; with `h`, keep a
    copy of each EPUB in X4_HOME/books/ for the writer."""
    prepare_book = _converter("convert.books", "prepare_book")
    to = "/" + to.strip("/") + "/"
    with tempfile.TemporaryDirectory() as tmp:
        uploads = prepare_book(src, Path(tmp))
        uploads = [
            replace(u, remote=to + u.remote.removeprefix("/books/")) if u.remote.startswith("/books/") else u
            for u in uploads
        ]
        uploaded = upload_all(dev, uploads)
        if h is not None:
            for u in uploads:
                if keeps_book(u.remote):
                    h.book(u.remote).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(u.local, h.book(u.remote))
        return {"uploaded": uploaded}


def keep_books(dev: Device, h: Home) -> list[str]:
    """Copy into X4_HOME/books/ every EPUB the X4 has read in that the writer has no copy of yet,
    so the cloud run can recap it. Returns their book keys."""
    stats = Stats.model_validate(read_json(h.stats, {}))
    kept = []
    for key in sorted(stats.books):
        remote = "/" + key.lstrip("/")
        if not keeps_book(remote) or h.book(key).exists():
            continue
        try:
            data = dev.get(remote)
        except X4Error:
            continue  # no longer on the card
        h.book(key).parent.mkdir(parents=True, exist_ok=True)
        h.book(key).write_bytes(data)
        kept.append(key.lstrip("/"))
    return kept


def rss_sync(dev: Device, h: Home) -> dict:
    """Fetch the feeds in X4_HOME/feeds.yaml, bundle new articles, upload them. Entries count as
    seen only once every upload is on the device; a failed run fetches them again."""
    if not h.feeds.exists():
        raise InvalidData(f"{h.feeds}: missing", [f"{h.feeds}: create it with the feeds to follow"])
    sync_feeds = _converter("convert.rss", "sync_feeds")
    with tempfile.TemporaryDirectory() as tmp:
        uploaded = upload_all(dev, sync_feeds(h.feeds, h.rss_state, Path(tmp), pending(h.rss_state)))
    commit_pending(h.rss_state)
    return {"uploaded": uploaded}


def _cache(name: str) -> Path:
    """Downloads kept between runs."""
    return Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "x4" / name


FONT_SIZES = [12, 14, 16, 18]
_FONT_SUFFIXES = (".ttf", ".otf", ".ttc", ".woff", ".woff2")


def font_sources(sources: list[str]) -> tuple[list[Path], str | None]:
    """Font files, or else one Google Fonts family name (words joined, so `Crimson Pro` works unquoted)."""
    paths = [Path(s).expanduser() for s in sources]
    missing = [s for s, p in zip(sources, paths, strict=True) if not p.is_file()]
    if not missing:
        return paths, None
    if len(missing) < len(sources) or any("/" in s or s.lower().endswith(_FONT_SUFFIXES) for s in missing):
        raise X4Error(f"{missing[0]}: no such file")
    return [], " ".join(s.strip() for s in sources)


def font_add(dev: Device, sources: list[str], name: str | None = None, sizes: list[int] | None = None) -> dict:
    """Convert TTF/OTF files, or a Google Fonts family fetched by name, and upload the packs."""
    from .convert.fonts import assign_styles, build_font, family_name, font_family
    from .convert.gfonts import build_google_font, symbols

    sizes = sizes or FONT_SIZES
    files, google = font_sources(sources)
    with tempfile.TemporaryDirectory() as tmp:
        try:
            if google:
                family, uploads = build_google_font(google, sizes, _cache("fonts"), Path(tmp), name)
            else:
                family = family_name(name or font_family(assign_styles(files)["regular"]))
                uploads = build_font(files, family, sizes, Path(tmp), symbols(_cache("fonts")))
        except (ValueError, RuntimeError) as e:
            raise X4Error(str(e)) from e
        return {"family": family, "uploaded": upload_all(dev, uploads)}


def font_install(
    dev: Device, force: bool = False, sizes: list[int] | None = None, progress: Callable[[dict], None] | None = None
) -> dict:
    """Fetch, convert and upload the curated families; skip those on the device unless `force`."""
    from .convert import gfonts

    sizes = sizes or FONT_SIZES
    out = []
    for name in gfonts.CURATED:
        family = gfonts.family_name(name)
        remote = f"/fonts/{family}/"
        present = _remote_sizes(dev, remote.rstrip("/"))
        wanted = [f"{family}_{size}.cpfont" for size in sizes]
        if not force and all(f in present for f in wanted):
            item = {"family": family, "remote": remote, "bytes": sum(present[f] for f in wanted), "skipped": True}
        else:
            with tempfile.TemporaryDirectory() as tmp:
                try:
                    _, uploads = gfonts.build_google_font(name, sizes, _cache("fonts"), Path(tmp), family)
                except (ValueError, RuntimeError) as e:
                    raise X4Error(f"{name}: {e}") from e
                done = upload_all(dev, uploads)
            item = {"family": family, "remote": remote, "bytes": sum(u["bytes"] for u in done), "skipped": False}
        out.append(item)
        if progress:
            progress(item)
    return {"fonts": out, "bytes": sum(f["bytes"] for f in out)}


def dict_install(dev: Device, lang: Literal["en", "hu"]) -> dict:
    build_dictionary = _converter("dicts", "build_dictionary")
    # Kept between runs: the Wiktionary download is ~100 MB.
    return {"uploaded": upload_all(dev, build_dictionary(lang, _cache("dicts")))}


def words_install(dev: Device, h: Home, lang: Literal["en", "hu"]) -> dict:
    """Build the word lists and crosswords from Wiktionary (shares the dictionary download) and upload them.

    Crosswords Claude clued in X4_HOME/crosswords/<lang>.jsonl stay: x4 sync pushes those."""
    from .words import CROSSWORDS as COUNT
    from .words import build_words

    host = h.crosswords / f"{lang}.jsonl"
    made: set[str] = set()
    files = [
        put_bytes(dev, b.upload.remote, Path(b.upload.local).read_bytes(), made) | {"count": b.count, "unit": b.unit}
        for b in build_words(lang, _cache("dicts"), 0 if host.exists() else COUNT)
    ]
    if host.exists():
        files.append({"remote": f"{CROSSWORDS}/{lang}.jsonl", "kept": str(host)})
    return {"lang": lang, "files": files}


def chess_install(dev: Device) -> dict:
    """Build the chess puzzles from the Lichess database (downloaded once, ~300 MB) and upload them."""
    from .chess import build_chess

    built = build_chess(_cache("chess"))
    data = Path(built.upload.local).read_bytes()
    return {"files": [put_bytes(dev, built.upload.remote, data) | {"count": built.count, "unit": "puzzles"}]}


def seed_file(path: Path) -> dict[str, str | None]:
    """Seed words from a file, in order: one per line, `word` or `word<TAB>source title`."""
    out: dict[str, str | None] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        word, _, src = line.partition("\t")
        if word.strip():
            out.setdefault(word.strip(), src.strip() or None)
    return out


def words_grids(
    h: Home, lang: Literal["en", "hu"], count: int = 6, seeds: dict[str, str | None] | None = None
) -> dict:
    """Filled 7x7 grids for Claude to clue, numbered after X4_HOME/crosswords/<lang>.jsonl and
    without the answers of its last puzzles. `seeds` (word -> source title or None, in order) go in
    first, the rest is rare words. Saved to <lang>.grids.jsonl."""
    from .crosswords import next_number, recent_answers
    from .words import cached, grids

    host = h.crosswords / f"{lang}.jsonl"
    try:
        lex, ranks = cached(lang, _cache("dicts"))
        made = grids(lex, ranks, lang, next_number(host, lang), count, recent_answers(host, lang), seeds)
    except ValueError as e:
        raise X4Error(str(e)) from e
    out = h.crosswords / f"{lang}.grids.jsonl"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(jsonl_bytes(made["grids"]))
    return {"lang": lang, "file": str(out), "append_to": str(host), **made}


# sync


def _remote_sizes(dev: Device, path: str) -> dict[str, int]:
    try:
        return {e["name"]: e["size"] for e in dev.ls(path)}
    except DeviceError as e:
        if e.code == "not_found":
            return {}
        raise


def _push_file(dev: Device, h: Home, remote: str, data: bytes) -> None:
    put_bytes(dev, remote, data)
    state = _state(h)
    state["pushed"][remote] = zlib.crc32(data)
    write_json(h.sync_state, state)


def push_net(dev: Device, h: Home) -> dict:
    """Upload /x4/net.json from X4_HOME/net.json and the Mac's time zone."""
    from . import net

    data = net.device_body(h)
    _push_file(dev, h, net.REMOTE, data)
    return {"remote": net.REMOTE, "bytes": len(data)}


def net_command(dev: Device, args: dict) -> dict:
    """The device's `net` command: Wi-Fi networks, a fetch, and their state."""
    return dev.call("net", args)


def fetch(dev: Device, h: Home, timeout: float = 90) -> dict:
    """Push net.json, have the X4 go online now, wait for it, and return what it fetched."""
    import time

    from . import net

    if h.net.exists() or h.serve.exists() or h.repo.exists():
        push_net(dev, h)
    state = net_command(dev, {"fetch": True})
    deadline = time.monotonic() + timeout
    while state.get("net", {}).get("fetching") and time.monotonic() < deadline:
        time.sleep(1)
        state = net_command(dev, {})
    out = dict(state.get("net", {}))
    if dev.stat(net.AGENDA):
        out["agenda"] = json.loads(dev.get(net.AGENDA))
    return out


@dataclass
class Issue:
    """What the X4 takes as one issue, over USB or the LAN: the stream, the threads of its running
    stories, the Read list with its articles, and football.json. Built without a device."""

    files: list[tuple[str, bytes]]  # (device path, bytes), in push order
    stream: Checked | None
    ready: list[Ready]
    read_list: bool  # the Read list is in, so /books/read holds only its articles
    warnings: list[str]
    pieces: list[ReadSource] | None = None  # the paper's issue as it went out; None without sections.md


def _sent_today(h: Home) -> list[ReadSource] | None:
    """The pieces of the issue that already went out today, as the Read list named them."""
    issue = _state(h).get("issue") or {}
    if issue.get("date") != date.today().isoformat():
        return None
    return [ReadSource.model_validate(r) for r in issue.get("reads", [])]


def _piece_errors(h: Home, reads: list[ReadSource]) -> list[str]:
    """What a piece carries is checked as it was on the set's articles: a question's quote is
    in the article, a link names what was read earlier, a forecast settles in time, and a
    thread is in threads.jsonl."""
    from . import forecasts, links

    name = str(h.read)
    asking = [
        {"id": r.id, "kind": "article", "book": f"{READ_DIR.lstrip('/')}/{r.id}.epub"}
        | {k: v.model_dump() for k in ("predict", "link") if (v := getattr(r, k)) is not None}
        for r in reads
        if r.predict or r.link
    ]
    errors = _predict_errors(h, name, asking) + links.errors(h, name, asking)
    rows = [{"id": r.forecast.id, "kind": "forecast", "by": r.forecast.by} for r in reads if r.forecast]
    errors += forecasts.check(h, name, rows, date.today())
    threads = _ids(h.threads)
    for r in reads:
        if r.thread and r.thread not in threads:
            errors.append(f"{name}: {r.id}: thread: '{r.thread}' is not in threads.jsonl; write the story so far there, or leave the field out")
    return errors


def check_issue(h: Home) -> tuple[list[ReadSource] | None, Checked | None, list[str]]:
    """Validate the issue's files: the Read list and the stream, with their errors."""
    from . import films
    errors: list[str] = []
    reads = None
    if h.read.exists():
        try:
            reads = check_read(h.read)
            errors += too_long(h, reads)
            errors += already_sent(h, reads)
            # An issue's pieces are fixed for its day: once it went out, only saved links are read.
            if (sections := paper.load(h)) is not None and _sent_today(h) is None:
                errors += paper.check_reads(h, sections, reads)
                errors += _piece_errors(h, reads)
                errors += films.check(h, reads)
        except InvalidData as e:
            errors += e.details
    stream = None
    if h.stream.exists():
        try:
            stream = check_local_stream(h, h.stream)
        except InvalidData as e:
            errors += e.details
    if h.threads.exists():
        try:
            check_threads(h.threads)
        except InvalidData as e:
            errors += e.details
    if h.football.exists():
        try:
            json.loads(h.football.read_bytes())
        except ValueError as e:
            errors.append(f"{h.football}: not valid JSON ({e}); run x4 football")
    from . import voice

    errors += voice.check(h)
    return reads, stream, errors


def build_issue(h: Home, reads: list[ReadSource] | None, stream: Checked | None) -> Issue:
    """Fetch and convert the Read list (a failed article is left out with a warning, and so are
    the stream's lines and items pointing at it), then list the files."""
    from .football import REMOTE as FOOTBALL
    from .readlist import prepare

    files: list[tuple[str, bytes]] = []
    warnings: list[str] = []
    ready: list[Ready] = []
    sections = paper.load(h) if reads is not None else None
    if reads is not None:
        if sections is not None and (fixed := _sent_today(h)) is not None:
            had = {r.id for r in fixed}
            left = [r.id for r in reads if r.id not in had and not r.kept]
            reads = fixed + [r for r in reads if r.id not in had and r.kept]
            if left:
                warnings.append(f"{h.read}: today's issue went out already; left out {', '.join(left)}")
        ready, w = prepare(h, reads)
        warnings += w
        files += [(r.remote, r.epub.read_bytes()) for r in ready]
        head = [paper.header(sections, date.today().isoformat())] if sections is not None else []
        files.append((READ_LIST, jsonl_bytes(head + [r.record for r in ready])))
    if stream is not None:
        files.append((STREAM, stream.body))
    if h.threads.exists():
        files.append((THREADS, check_threads(h.threads).body))
    if h.football.exists():
        files.append((FOOTBALL, h.football.read_bytes()))
    return Issue(files, stream, ready, reads is not None, warnings, reads if sections is not None else None)


def _after_issue(h: Home, issue: Issue) -> list[str]:
    """Once an issue is pushed or staged: remember its Read items and puzzles, take
    its links out of the inbox, and move the feed on. Returns the links removed."""
    from .feed import remove_from_inbox

    state = _state(h)
    if issue.read_list:
        from .readlist import note_sent

        note_sent(h, issue.ready, date.today())
        state["read"] = _recent(
            state.get("read", {}),
            {r.record["id"]: {k: r.record[k] for k in ("kind", "title", "source")} for r in issue.ready},
        )
    if issue.pieces is not None:
        from . import forecasts
        from .readlist import mark_asked

        # What the pieces asked: the ledger takes their forecasts, and the context finds a
        # guess's or a link's question by the piece's id.
        went = {r.record["id"] for r in issue.ready}
        sent = [p for p in issue.pieces if p.id in went]
        forecasts.note(h, [{"kind": "forecast", "id": p.forecast.id, "text": p.forecast.text, "by": p.forecast.by} for p in sent if p.forecast], date.today())
        titles = {r.record["id"]: r.record["title"] for r in issue.ready}
        state["predicts"] = _recent(state.get("predicts", {}), {p.id: {"title": titles[p.id], "prompt": p.predict.prompt} for p in sent if p.predict})
        state["links"] = _recent(state.get("links", {}), {p.id: {"title": titles[p.id], "prompt": p.link.prompt} for p in sent if p.link})
        mark_asked(h, {p.id for p in sent if p.predict or p.link})
        state["issue"] = {
            "date": date.today().isoformat(),
            "reads": [r.model_dump(exclude_none=True) for r in issue.pieces if r.id in went],
        }
    if issue.stream is not None:
        _note_stream(state, issue.stream)
    write_json(h.sync_state, state)
    return remove_from_inbox(h, [r.url for r in issue.ready if r.record["kind"] == "link" and r.url])


def sync(dev: Device, h: Home) -> dict:
    """Pull everything, then push every deck, Read list article, stream, crossword set, recap file,
    next-book pick, glossary and football.json that changed since its last push. A picked library
    book goes as a sample, and whole once the sample was read. Empties the outbox."""
    from .football import before_issue

    out: dict = {"pulled": pull(dev, h, "all"), "pushed": [], "deleted": [], "unchanged": 0, "warnings": []}
    out["books"] = keep_books(dev, h)
    out["warnings"] += before_issue(h)

    plan: list[tuple[str, int, Callable[[], object]]] = []
    errors: list[str] = []
    for f in sorted(h.decks.glob("*.jsonl")):
        try:
            c = _check_deck(h, f)
            plan.append((f"{DECKS}/{c.id}.jsonl", zlib.crc32(c.body), lambda f=f, c=c: push_deck(dev, h, f, c)))
        except InvalidData as e:
            errors += e.details
    reads, stream, issue_errors = check_issue(h)
    errors += issue_errors
    from .crosswords import check_crosswords
    from .words import ALPHABETS

    crosswords = []
    for lang in ALPHABETS:
        f = h.crosswords / f"{lang}.jsonl"
        if f.exists():
            try:
                c = check_crosswords(f, lang)
                remote = f"{CROSSWORDS}/{lang}.jsonl"
                crosswords.append((remote, zlib.crc32(c.body), lambda r=remote, c=c: _push_file(dev, h, r, c.body)))
            except InvalidData as e:
                errors += e.details
    from .previously import REMOTE as PREVIOUSLY, check_previously

    if h.previously.exists():
        try:
            c = check_previously(h.previously)
            crosswords.append((PREVIOUSLY, zlib.crc32(c.body), lambda c=c: _push_file(dev, h, PREVIOUSLY, c.body)))
        except InvalidData as e:
            errors += e.details
    from .nextbook import REMOTE as NEXT, check_next, sync_next

    picks = None
    if h.next.exists():
        try:
            picks = check_next(h.next)
        except InvalidData as e:
            errors += e.details
    glossaries = []
    for f in sorted(h.glossary.glob("*.json")):
        try:
            crc = _glossary_crc(check_glossary(f))
            key = f.stem.replace("__", "/")
            remote = f"{GLOSSARY}/{glossary_folder(key)}/"
            glossaries.append((remote, crc, lambda f=f, key=key: push_glossary(dev, h, key, f)))
        except InvalidData as e:
            errors += e.details
    if errors:
        raise InvalidData("pulled, but pushed nothing: fix these files and run x4 sync again", errors)
    if picks is not None:
        try:
            body, out["next"] = sync_next(dev, h, picks, lambda remote, data: put_bytes(dev, remote, data))
        except InvalidData as e:
            raise InvalidData("pulled, but pushed nothing: fix these files and run x4 sync again", e.details) from None
        crosswords.append((NEXT, zlib.crc32(body), lambda: _push_file(dev, h, NEXT, body)))

    issue = build_issue(h, reads, stream)
    out["warnings"] += issue.warnings
    for remote, data in issue.files:
        if remote == STREAM:
            push = lambda c=issue.stream: push_stream(dev, h, h.stream, c)  # noqa: E731
        else:
            push = lambda r=remote, data=data: _push_file(dev, h, r, data)  # noqa: E731
        plan.append((remote, zlib.crc32(data), push))
    plan += crosswords + glossaries
    if h.net.exists() or h.serve.exists() or h.repo.exists():
        from . import net

        net_body = net.device_body(h)
        plan.append((net.REMOTE, zlib.crc32(net_body), lambda: _push_file(dev, h, net.REMOTE, net_body)))

    listings: dict[str, dict[str, int]] = {}

    def listing(folder: str) -> dict[str, int]:
        if folder not in listings:
            listings[folder] = _remote_sizes(dev, folder)
        return listings[folder]

    pushed = _state(h)["pushed"]
    for remote, crc, push in plan:
        folder, name = posixpath.split(remote.rstrip("/"))
        if pushed.get(remote) == crc and name in listing(folder):
            out["unchanged"] += 1
            continue
        push()
        out["pushed"].append(remote)

    local = {remote for remote, _, _ in plan}
    state = _state(h)
    for remote in sorted(state["pushed"]):
        if remote.startswith((DECKS + "/", READ_DIR + "/")) and remote not in local:
            del state["pushed"][remote]
            if remote.startswith(DECKS + "/"):  # stale Read articles go below, listed or not
                if posixpath.basename(remote) in listing(DECKS):
                    dev.rm(remote)
                out["deleted"].append(remote)
    # The paper's articles are the X4's to remove: it keeps the ones of kept pieces, and takes the
    # rest when its front page sees the new issue.
    if issue.read_list and issue.pieces is None:
        for name in sorted(listing(READ_DIR)):
            remote = f"{READ_DIR}/{name}"
            if name.endswith(".epub") and remote not in local:
                dev.rm(remote)
                out["deleted"].append(remote)
    write_json(h.sync_state, state)

    out["inbox_removed"] = _after_issue(h, issue)
    commit_pending(h.context_state, h.feed_state)
    from .lan import empty_outbox

    empty_outbox(h)  # everything in it is on the X4 now
    from .datarepo import send_after

    if (sent := send_after(h, "sync")) is not None:
        out["repo"] = sent
    return out


def beside_issue(h: Home) -> tuple[list[tuple[str, bytes]], list[str]]:
    """What the writer writes beside the issue and the X4 takes over Wi-Fi: every deck, the
    recaps and the crossword sets, at their device paths, and what is wrong with them."""
    from .crosswords import check_crosswords
    from .previously import REMOTE as PREVIOUSLY, check_previously
    from .words import ALPHABETS

    files: list[tuple[str, bytes]] = []
    errors: list[str] = []
    for f in sorted(h.decks.glob("*.jsonl")):
        try:
            c = _check_deck(h, f)
            files.append((f"{DECKS}/{c.id}.jsonl", c.body))
        except InvalidData as e:
            errors += e.details
    if h.previously.exists():
        try:
            files.append((PREVIOUSLY, check_previously(h.previously).body))
        except InvalidData as e:
            errors += e.details
    for lang in ALPHABETS:
        f = h.crosswords / f"{lang}.jsonl"
        if f.exists():
            try:
                files.append((f"{CROSSWORDS}/{lang}.jsonl", check_crosswords(f, lang).body))
            except InvalidData as e:
                errors += e.details
    return files, errors


def stage(h: Home) -> dict:
    """Write what the X4 takes over Wi-Fi into X4_HOME/outbox/: the issue, the decks, the recaps
    and the crosswords at their device paths, and manifest.json (docs/PROTOCOL.md, "LAN issue"
    and "Repo issue"). A deck no longer here leaves the X4. No device needed."""
    from .football import before_issue
    from .lan import write_outbox

    warnings = before_issue(h)
    reads, stream, errors = check_issue(h)
    beside, more = beside_issue(h)
    errors += more
    if errors:
        raise InvalidData("staged nothing: fix these files and run x4 stage again", errors)
    issue = build_issue(h, reads, stream)
    manifest = write_outbox(h, issue.files + beside, ([READ_DIR] if issue.read_list else []) + [DECKS])
    removed = _after_issue(h, issue)
    commit_pending(h.feed_state)
    from .datarepo import send_after

    out = {
        "outbox": str(h.outbox),
        "issue": manifest["issue"],
        "files": manifest["files"],
        "prune": manifest["prune"],
        "inbox_removed": removed,
        "warnings": warnings + issue.warnings,
    }
    if (sent := send_after(h, f"issue {manifest['issue']}")) is not None:
        out["repo"] = sent
    return out
