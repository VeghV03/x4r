"""The user's data directory, X4_HOME (default ~/x4)."""

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any


def glossary_folder(book_key: str) -> str:
    """The device folder of a book's glossary: FNV-1a 64 of the book key in 16 hex digits, short for any book."""
    h = 0xCBF29CE484222325
    for b in book_key.strip("/").encode():
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def book_folder(book_key: str) -> str:
    """`books/a/b.epub` -> `books__a__b.epub`, the folder and file name form of a book key."""
    return book_key.strip("/").replace("/", "__")


@dataclass(frozen=True)
class Home:
    root: Path

    @property
    def notes(self) -> Path:
        return self.root / "notes"

    @property
    def decks(self) -> Path:
        return self.root / "decks"

    @property
    def glossary(self) -> Path:
        return self.root / "glossary"

    @property
    def crosswords(self) -> Path:
        """Crosswords Claude clued, `<lang>.jsonl`, and the grids waiting for clues, `<lang>.grids.jsonl`."""
        return self.root / "crosswords"

    @property
    def pulled(self) -> Path:
        return self.root / "pulled"

    @property
    def stream(self) -> Path:
        return self.root / "stream.jsonl"

    @property
    def sent(self) -> Path:
        """Every Read item ever sent, written by `x4` alone (readlist.note_sent)."""
        return self.root / "sent.jsonl"

    @property
    def forecasts(self) -> Path:
        """The forecast ledger, written by `x4` alone (forecasts.py)."""
        return self.root / "forecasts.jsonl"

    @property
    def threads(self) -> Path:
        """One line per running story: the story so far, shown before its next article."""
        return self.root / "threads.jsonl"

    @property
    def previously(self) -> Path:
        """One recap per book in progress, shown when the book opens after days away."""
        return self.root / "previously.jsonl"

    @property
    def library(self) -> Path:
        """The user's own EPUBs, for next-book picks: a pick goes to the card as a sample."""
        return self.root / "library"

    @property
    def next(self) -> Path:
        """Next-book picks: up to three books to follow each book near its end."""
        return self.root / "next.jsonl"

    @property
    def feeds(self) -> Path:
        return self.root / "feeds.yaml"

    @property
    def read(self) -> Path:
        """The Read list as Claude writes it, with `url` or `file` per item."""
        return self.root / "read.jsonl"

    @property
    def inbox(self) -> Path:
        return self.root / "inbox.md"

    @property
    def sections(self) -> Path:
        """The paper's sections, in the front page's order (sections.py). Yours to edit."""
        return self.root / "sections.md"

    @property
    def settings(self) -> Path:
        """The writing language and the news engine (settings.py). Yours to edit."""
        return self.root / "settings.yaml"

    @property
    def interests(self) -> Path:
        return self.root / "interests.md"

    @property
    def taste(self) -> Path:
        """What you like in film; Claude reads it to pick Film. Yours to edit."""
        return self.root / "taste.md"

    @property
    def seen_films(self) -> Path:
        """What you have watched (films.py). Yours to edit."""
        return self.root / "seen.md"

    @property
    def cache(self) -> Path:
        """Fetched articles as EPUB, by URL. Safe to delete."""
        return self.root / "cache"

    @property
    def feed_state(self) -> Path:
        return self.root / ".feed.json"

    @property
    def stats(self) -> Path:
        return self.root / "stats.json"

    @property
    def net(self) -> Path:
        """Calendars and the weather place the X4 fetches by itself over Wi-Fi."""
        return self.root / "net.json"

    @property
    def football_cfg(self) -> Path:
        """The clubs and the competitions to follow."""
        return self.root / "football.yaml"

    def book(self, key: str) -> Path:
        """Where a book sent to the X4 is kept for the writer: books/ mirrors the card's book
        keys, without their own leading books/ (K14)."""
        return self.root / "books" / key.lstrip("/").removeprefix("books/")

    @property
    def football_key(self) -> Path:
        """The football-data.org key, one line. Never sent to the data repo."""
        return self.root / "football.key"

    @property
    def football(self) -> Path:
        """Results, fixtures and tables, as /x4/football.json."""
        return self.root / "football.json"

    @property
    def outbox(self) -> Path:
        """The staged issue, at device paths, for the X4 to take over Wi-Fi."""
        return self.root / "outbox"

    @property
    def serve(self) -> Path:
        """`x4 serve`'s port and token."""
        return self.root / "serve.json"

    @property
    def repo_inbox(self) -> Path:
        """What the X4 sent up over Wi-Fi, one file per upload, until the writer takes it in."""
        return self.root / "inbox"

    @property
    def repo(self) -> Path:
        """The data repo's name and the X4's token for it (`x4 repo`). Never in the repo itself."""
        return self.root / "repo.json"

    @property
    def sync_state(self) -> Path:
        return self.root / ".sync.json"

    @property
    def context_state(self) -> Path:
        return self.root / ".context.json"

    @property
    def rss_state(self) -> Path:
        return self.root / ".rss.json"

    def glossary_file(self, book_key: str) -> Path:
        return self.glossary / f"{book_folder(book_key)}.json"

    def archive(self, name: str) -> Path:
        """`pulled/<name>.jsonl`: highlights, reviews, seen, play, read-log, lookups, kept or visits."""
        return self.pulled / f"{name}.jsonl"


def home() -> Home:
    """The home directory, created with its subfolders on first use."""
    root = Path(os.environ.get("X4_HOME") or Path.home() / "x4").expanduser()
    h = Home(root)
    for d in (h.notes, h.decks, h.glossary, h.pulled):
        d.mkdir(parents=True, exist_ok=True)
    return h


def read_json(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, data: Any) -> None:
    """Write atomically, so a crash never leaves half a state file."""
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    tmp.replace(path)


def pending(path: Path) -> Path:
    """Where a cursor that `context` or `feed` moved waits for `x4 sync`, which commits it. A
    sync that stops early leaves the cursor as it was, so the same highlights, lookups and news
    come back next time."""
    return path.with_name(path.name + ".pending")


def commit_pending(*paths: Path) -> None:
    for path in paths:
        if pending(path).exists():
            pending(path).replace(path)


def read_jsonl(path: Path) -> list[dict]:
    """Records of a JSONL file we wrote ourselves (archives). A line cut short by a crash during an
    append is skipped."""
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(line) if line.strip() else None
        except ValueError:
            continue
        if isinstance(record, dict):
            out.append(record)
    return out
