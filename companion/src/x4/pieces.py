"""Time and pieces by section, from the pulled records (docs/DATA.md "On the Mac").

A piece's section and the issues that listed it come from X4_HOME/sent.jsonl. A piece is seen
when its section was visited while an issue listing it was on the X4, opened when the read log
has a line for it, finished when one says so, kept when its last kept.jsonl line says so.
"""

from datetime import date
from pathlib import Path

from . import days
from .home import Home, read_jsonl
from .models import Play, ReadLog, Visit
from .readlist import READ_DIR, sent

WEEK = 7  # x4 status: minutes of the last week
RECENT = 14  # x4 context: pieces of the last two weeks
ARTICLES = READ_DIR.lstrip("/") + "/"
PLAY, BOOK, KEPT, READ = "play", "book", "kept", "read"


def _listed(line: dict) -> list[str]:
    return line.get("listed") or ([line["issue"]] if line.get("issue") else [])


def _minutes(ms: int) -> int:
    return round(ms / 60000)


def _piece(book: str) -> str | None:
    """The Read id of an article's book key, or None for a book."""
    return Path(book).stem if book.startswith(ARTICLES) else None


def minutes(h: Home, order: list[str], logs: dict[str, list[dict]], now: int | None = None) -> dict[str, int]:
    """Minutes of the last WEEK days per section, in the front page's order, then Play, Book and
    the kept links that are in no section, then the old Read list's pieces, sent before
    sections. Pieces count by their section, section screens by their visits, Play by its games
    and its screen, Book by the other books. `logs` holds the records still on the X4, by log
    name."""
    now = days.today() if now is None else now
    sections = {i: line.get("section") or (KEPT if line.get("kind") == "link" else READ) for i, line in sent(h).items()}
    ms: dict[str, int] = {}

    def add(key: str, n: int | None) -> None:
        ms[key] = ms.get(key, 0) + (n or 0)

    def records(name: str, model):
        rows = read_jsonl(h.archive(name)) + logs.get(name, [])
        return [r for r in map(model.model_validate, rows) if days.within(r, WEEK, now)]

    for r in records("read-log", ReadLog):
        rid = _piece(r.book)
        add(BOOK if rid is None else sections.get(rid, READ), r.ms)
    for v in records("visits", Visit):
        add(v.section, v.ms)
    for p in records("play", Play):
        add(PLAY, p.ms)
    keys = [*order, PLAY, BOOK, KEPT, READ]
    keys += sorted(k for k in ms if k not in keys)  # a section no longer in sections.md
    return {k: _minutes(ms.get(k, 0)) for k in keys if k in ms or k in order}


def pieces(h: Home, today: date | None = None) -> list[dict]:
    """Every piece an issue listed in the last RECENT days, newest issue first: its section,
    whether it was seen, opened, finished and kept, and its minutes."""
    today = today or date.today()
    first = date.fromordinal(today.toordinal() - RECENT + 1).isoformat()
    lines = [x for x in sent(h).values() if x.get("section") and any(d >= first for d in _listed(x))]
    visited = {(v.section, v.issue) for v in map(Visit.model_validate, read_jsonl(h.archive("visits"))) if v.issue}
    logs: dict[str, list[ReadLog]] = {}
    for r in map(ReadLog.model_validate, read_jsonl(h.archive("read-log"))):
        if rid := _piece(r.book):
            logs.setdefault(rid, []).append(r)
    kept: dict[str, bool] = {}
    for k in read_jsonl(h.archive("kept")):
        if isinstance(k.get("id"), str):
            kept[k["id"]] = bool(k.get("kept", True))
    out = []
    for x in sorted(lines, key=lambda x: max(_listed(x)), reverse=True):
        rs = logs.get(x["id"], [])
        out.append({
            "id": x["id"],
            "section": x["section"],
            "title": x.get("title"),
            "issue": max(_listed(x)),
            "seen": any((x["section"], d) in visited for d in _listed(x)),
            "opened": bool(rs),
            "finished": any(r.finished for r in rs),
            "kept": kept.get(x["id"], False),
            "min": _minutes(sum(r.ms or 0 for r in rs)),
        })
    return out
