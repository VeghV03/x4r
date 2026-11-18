"""MCP server: the `x4` commands as tools, one-to-one. No backup, flash or serve.

Each tool opens its own session (hello ... bye). The port comes from X4_PORT or autodetect,
user data from X4_HOME (default ~/x4). Paths on the Mac must be absolute.
"""

import json
import threading
from collections.abc import Callable
from pathlib import Path
from typing import Literal

from mcp.server.mcpserver import Image, MCPServer
from mcp.server.mcpserver.exceptions import ToolError

from . import ops
from .context import context as build_context
from .device import Device, connect
from .errors import X4Error
from .home import home

Button = Literal["back", "confirm", "left", "right", "up", "down", "power"]

server = MCPServer(
    "x4",
    instructions=(
        "Tools for an Xteink X4 e-reader on USB. Typical sync: pull, context, feed, write files in X4_HOME "
        "(decks/<id>.jsonl, stream.jsonl, read.jsonl, glossary/<book key with / as __>.json), then sync. "
        "Record formats: docs/DATA.md. A question is answered once a card or stream item has "
        '"highlight": "<question id>".'
    ),
)


# Tools run in worker threads; one session holds the port at a time.
_port = threading.Lock()


def _error(e: Exception) -> ToolError:
    """The CLI's --json error object, so the caller can tell busy from invalid from no_device."""
    if isinstance(e, X4Error):
        err = e.to_json()
    else:
        err = {"code": "io" if isinstance(e, OSError) else "invalid", "message": str(e)}
    return ToolError(json.dumps(err, ensure_ascii=False))


def _run[T](fn: Callable[[Device], T]) -> T:
    try:
        with _port, connect() as dev:
            return fn(dev)
    except (X4Error, OSError, ValueError) as e:
        raise _error(e) from e


def _local(path: str) -> Path:
    p = Path(path).expanduser()
    if not p.is_absolute():
        raise ToolError(json.dumps({"code": "invalid", "message": f"{path}: pass an absolute path"}))
    return p


@server.tool()
def status() -> dict:
    """Device firmware, battery, SD free/total, `day` (days since 1970 in the X4's zone; a small number while it has never seen a clock), `issue` (the date of the issue on the X4), retention (0-1, absent before any review), `minutes` (the last 7 days per section in the front page's order, then play, book, kept links in no section, and read: pieces of the old Read list, sent before sections), and how many highlights and reviews are on the device and not yet pulled."""
    return _run(lambda dev: ops.status(dev, home()))


@server.tool()
def pull(
    what: Literal["highlights", "reviews", "seen", "play", "read-log", "lookups", "kept", "visits", "stats", "all"] = "all",
) -> dict:
    """Copy device logs (highlights, reviews, seen stream items, play, read-log, dictionary lookups, kept, visits) into X4_HOME/pulled/*.jsonl (deduplicated), then remove them on the device. Saves stats.json. Applies highlights deleted on the device. Re-renders notes/<book>.md from the live highlights. Returns new/duplicate/unreadable counts (plus deleted for highlights) and changed notes files."""
    return _run(lambda dev: ops.pull(dev, home(), what))


@server.tool()
def push_deck(file: str) -> dict:
    """Validate a deck JSONL (line 1 header {"deck","title","book"}, then one card per line: flip, choice, cloze or explain) and upload it to /x4/decks/<deck-id>.jsonl. A file outside X4_HOME/decks is copied there. Validation errors name file, line and field."""
    return _run(lambda dev: ops.push_deck(dev, home(), _local(file)))


@server.tool()
def push_glossary(book_key: str, file: str) -> dict:
    """Upload a per-book glossary. book_key is the book's SD path without the leading slash (e.g. books/meditations.epub); file is a JSON object of term -> definition. Builds StarDict files and replaces /x4/glossary/<key hash>/ (FNV-1a 64 of book_key, 16 hex digits)."""
    return _run(lambda dev: ops.push_glossary(dev, home(), book_key, _local(file)))


@server.tool()
def push_book(src: str, to: str = "/books/") -> dict:
    """Convert and upload a book. src is an absolute .epub or .pdf path, or an http(s) URL of an article. to is the SD folder. A PDF is reflowed as text or kept as pages; each upload's note says which and why. Append #reflow or #pages to a PDF path to choose."""
    source = src if src.startswith(("http://", "https://")) else str(_local(src))
    return _run(lambda dev: ops.push_book(dev, source, to, home()))


@server.tool()
def sync() -> dict:
    """Pull everything, then push every deck, stream, glossary and football.json in X4_HOME whose content changed since its last push, and delete device decks whose local file was removed. X4_HOME/read.jsonl (id, kind, url or file; title, source, lang, minutes optional) is fetched, converted to EPUB, uploaded to /books/read/<id>.epub and listed in /x4/read.jsonl (the X4 removes articles no longer listed or kept), and uploaded link items leave inbox.md. EPUBs the X4 read in are copied into X4_HOME/books/ for the cloud writer. A failed fetch leaves that item out with a warning. Validates all files before pushing any. Once everything is pushed, moves the context and feed cursors on and empties the outbox."""
    return _run(lambda dev: ops.sync(dev, home()))


@server.tool()
def context() -> dict:
    """Offline summary of X4_HOME for writing a sync: highlights new since the last sync (one made on a bite or a puzzle names it in `item`; its book is the item's, or stream/<item id>), lookups (words looked up in the dictionary since the last sync, each with the book key and title it was read in), unanswered questions, deleted highlight ids (new deletions and those still named by a card or stream item), per-card reviews (reps, lapses, last grade, acc) with weak cards, books with progress and read minutes, local decks and card counts (`flip`: ids of the flip cards still to rewrite as choice or cloze), unseen stream items, day and retention, `issue` (today's date and whether its issue was sent), `sections` (id and name, in the front page's order), `pieces` (each piece an issue listed in the last 14 days, newest first: section, title, issue, seen (its section was visited while it was listed), opened, finished, kept, min). For the last 7 days: read (results: finished or left per Read id with progress and minutes; waiting: listed ids not finished), play (per game: level, played, won, and recent played/won/practice/min), puzzles (book puzzle results from seen.jsonl). Also interests (X4_HOME/interests.md) and inbox (links not yet sent). Run pull first. Calling it again before sync returns the same new items: sync moves the cursor."""
    try:
        return build_context(home())
    except (X4Error, OSError, ValueError) as e:
        raise _error(e) from e


@server.tool()
def feed(topic: str | None = None) -> dict:
    """Candidates for the Read list since the last sync, no device needed; calling it again before sync returns the same ones. news: stories from LátóTér's engine, most independent sources (k) first, each with title, day, claim, topics, sources (org, url, lang; title when it differs) and, on the top stories, primary (title, url, kind, date: the gazette acts, EU law, rulings, official statements and documents the story rests on). follow: new entries from X4_HOME/feeds.yaml (title, url, source, date, summary, topics). topics are hungary, us, eu, palestine, china, or a feed's own. inbox: links saved in X4_HOME/inbox.md. interests: X4_HOME/interests.md. warnings: what could not be gathered and the fix. topic: only that topic's stories and entries, and the call is not remembered. Write the picks to X4_HOME/read.jsonl, then sync."""
    from .feed import feed as gather

    try:
        return gather(home(), topic)
    except (X4Error, OSError, ValueError) as e:
        raise _error(e) from e


@server.tool()
def football() -> dict:
    """Fetch the followed clubs' results (past 7 days), fixtures (next 14) and competition tables from football-data.org into X4_HOME/football.json, no device needed. The key, clubs (team ids) and competitions (codes such as PL, PD, SA, CL) are in X4_HOME/football.yaml. A club or competition the key cannot see is a warning. sync and stage run it first when football.yaml has a key."""
    from .football import fetch

    try:
        return fetch(home())
    except (X4Error, OSError, ValueError) as e:
        raise _error(e) from e


@server.tool()
def stage() -> dict:
    """When the X4 is not plugged in: write the issue (stream, Read list and its articles, football.json) into X4_HOME/outbox/ with a manifest, no device needed; `x4 serve` hands it to the X4 on its next fetch at home. Fetches football first and the Read list's articles like sync, validates the same way, and refuses an issue over the LAN limits (48 files, 4 MB each, 16 MB in all). Takes uploaded link items out of inbox.md and moves the feed cursor on. The next sync empties the outbox."""
    try:
        return ops.stage(home())
    except (X4Error, OSError, ValueError) as e:
        raise _error(e) from e


@server.tool()
def ls(path: str = "/") -> dict:
    """List a folder on the SD card. Entries have name, dir and size."""
    return _run(lambda dev: ops.ls(dev, path))


@server.tool()
def get(remote: str, local: str) -> dict:
    """Copy the SD file at remote to the absolute Mac path local."""
    return _run(lambda dev: ops.get(dev, remote, _local(local)))


@server.tool()
def put(local: str, remote: str) -> dict:
    """Copy the absolute Mac path local to remote on the SD card. A remote ending in / keeps the file name. Creates folders. Refuses files the X4 writes (its logs, stats.json, /x4/state, /.crosspoint)."""
    return _run(lambda dev: ops.put(dev, _local(local), remote))


@server.tool()
def rm(remote: str, recursive: bool = False) -> dict:
    """Remove a file on the SD card, or a folder with recursive=true. Refuses files the X4 writes (its logs, stats.json, /x4/state, /.crosspoint) and folders holding them."""
    return _run(lambda dev: ops.rm(dev, remote, recursive))


@server.tool()
def screenshot() -> Image:
    """The device screen as a PNG image (800x480)."""
    return Image(data=_run(ops.screenshot)[0], format="png")


@server.tool()
def press(button: Button, long: bool = False) -> dict:
    """Press a device button (long=true for a long press). Returns what `screen` does, afterwards."""
    return _run(lambda dev: ops.press(dev, button, long))


@server.tool()
def screen() -> dict:
    """The activity on screen. On a step (a question after a chapter, a piece's question, the bite,
    a book puzzle) also `step`: its `kind`, its `id` (a stream item's id or a card key), its `phase`
    (prompt, revealed, answered, empty), `done` and `total` steps, and the `selected` row or the
    option `picked`."""
    return _run(ops.screen)


@server.tool()
def rss_sync() -> dict:
    """Fetch the feeds in X4_HOME/feeds.yaml, bundle new articles as EPUB books and upload them."""
    return _run(lambda dev: ops.rss_sync(dev, home()))


@server.tool()
def font_install(force: bool = False) -> dict:
    """Install the curated reading fonts (Literata, the device default, plus Charis SIL, Source Serif 4, EB Garamond, Bitter, Atkinson Hyperlegible) to /fonts/<Family>/ at 12, 14, 16 and 18 pt. Families already on the device are skipped unless force=true. Returns each family's size on the SD card."""
    return _run(lambda dev: ops.font_install(dev, force))


@server.tool()
def font_add(source: list[str], name: str | None = None, sizes: list[int] | None = None) -> dict:
    """Convert a font family into .cpfont packs and upload them to /fonts/<Family>/. source is either absolute TTF/OTF paths of one family, or one Google Fonts family name such as ["Lora"] or ["Crimson Pro"], fetched from github.com/google/fonts. name overrides the family name on the device. sizes are points, like the built-in fonts' 12, 14, 16 and 18 (the default)."""
    paths = [s if not ("/" in s or s.startswith("~")) else str(_local(s)) for s in source]
    return _run(lambda dev: ops.font_add(dev, paths, name, sizes))


@server.tool()
def dict_install(lang: Literal["en", "hu"]) -> dict:
    """Build and upload the English (en) or Hungarian (hu) dictionary to /dictionaries/."""
    return _run(lambda dev: ops.dict_install(dev, lang))


@server.tool()
def words_install(lang: Literal["en", "hu"]) -> dict:
    """Build and upload the word games' data for English (en) or Hungarian (hu): /x4/words/<lang>/ (answers5.txt, valid5.txt) and 200 7x7 crosswords of rare words with Wiktionary glosses as clues in /x4/crosswords/<lang>.jsonl. When X4_HOME/crosswords/<lang>.jsonl exists, the device's crosswords are kept (x4 sync pushes that file). Built from Wiktionary (downloaded once, shared with dict_install) and a word frequency list. Returns each file with its line count."""
    return _run(lambda dev: ops.words_install(dev, home(), lang))


@server.tool()
def excerpt(book_key: str, words: int = 2500, progress: float | None = None) -> dict:
    """The last `words` words read in an EPUB on the X4, ending at `progress` (0 to 1; default: the book's progress from the last pull), placed as the device places progress. Reads the copy in X4_HOME/books/, fetching it from the X4 the first time. Returns book, title, progress, words and text. Write a recap from it into X4_HOME/previously.jsonl: {"book", "progress" (this progress), "text"}."""
    from .previously import excerpt as build

    h = home()
    if h.book(book_key).exists():
        try:
            return build(None, h, book_key, words, progress)
        except (X4Error, OSError, ValueError) as e:
            raise _error(e) from e
    return _run(lambda dev: build(dev, h, book_key, words, progress))


@server.tool()
def article(read_id: str) -> dict:
    """The text of a Read list article (an id in X4_HOME/read.jsonl) as the X4 has it, fetched and converted now if it was not yet. Returns id, title, words and text. A stream article's `predict.quote` must be a sentence of this text."""
    from .readlist import article as build

    return build(home(), read_id)


@server.tool()
def forecasts() -> dict:
    """Forecasts from asked to settled: `due` (past their day: look each up and call settle), `tell` (settled yes or no: write an `outcome` stream item for each, {"id","kind":"outcome","forecast":"<id>","body":"<what happened>"}), `open`, counts, and the score once 20 are settled."""
    from datetime import date

    from .forecasts import listing

    return listing(home(), date.today())


@server.tool()
def settle(forecast_id: str, outcome: Literal["yes", "no", "void"], source: str) -> dict:
    """Record what became of a forecast: it happened (yes), did not (no), or cannot be told (void). `source` is the URL that settles it."""
    from datetime import date

    from .forecasts import settle as build

    return build(home(), forecast_id, outcome, source, date.today())


@server.tool()
def shelf() -> dict:
    """Unread books to pick from when a book nears its end: EPUBs on the card (by key) and in X4_HOME/library (as "library/<file>"), with title and author. Write up to three into X4_HOME/next.jsonl: {"after": <book key>, "books": [<keys from here>]}; x4 sync sends library books as samples, and whole once read."""
    from .nextbook import shelf as build

    return _run(lambda dev: {"books": build(dev, home())})


@server.tool()
def chess_install() -> dict:
    """Build and upload the chess tactics: /x4/chess/puzzles.txt, 4000 puzzles from the Lichess puzzle database (CC0, downloaded once, ~300 MB), 400 in each of ten rating bands from 800 to 2400, short lines (2-4 moves to find) with good votes. Returns the file with its puzzle count."""
    return _run(ops.chess_install)


@server.tool()
def words_grids(
    lang: Literal["en", "hu"], count: int = 6, words: list[str] | None = None, src: dict[str, str] | None = None
) -> dict:
    """Filled 7x7 crossword grids without clues, no device needed. `words` are seed words, best first (highlighted or looked-up words, then rare words from the books and articles being read); `src` maps a seed word to the title of the book or article it came from. Each grid takes as many seeds as fit; the rest are rare headwords, none from the file's last 30 puzzles. Each grid: id (numbered after X4_HOME/crosswords/<lang>.jsonl), size, rows (. is a black square), across and down as [{n, answer}], a seed's entries also with seed: true and its src. Also returns placed, unplaced and skipped seeds. Write a short, hard definition clue next to each answer (in the grid's language, never containing the answer, at most 3 lines on the device), keep src on words from reading, and append the lines to append_to; sync validates and pushes it. Also saved to file. Needs the lexicon words_install built."""
    seeds: dict[str, str | None] = {w: (src or {}).get(w) for w in words or []}
    try:
        return ops.words_grids(home(), lang, count, seeds)
    except (X4Error, OSError, ValueError) as e:
        raise _error(e) from e
