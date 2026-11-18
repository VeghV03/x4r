"""`x4 context`: what Claude needs to write a good sync, in one compact JSON document.

Reads only X4_HOME (run `x4 pull` first). Remembers which highlights and dictionary lookups it
has already reported in .context.json, so each call shows only new ones. Read, play and puzzle results
cover the last RECENT_DAYS days.
"""

import json
import zlib
from datetime import date
from pathlib import Path

from .errors import InvalidData
from . import settings
from .feed import inbox, interests
from .home import Home, pending, read_json, read_jsonl, write_json
from .models import STEP_BOOK, Highlight, Lookup, Play, ReadLog, Review, Seen, Stats, jsonl_bytes
from .notes import note_paths
from .ops import STREAM, check_local_stream, fold, highlight_log, read_ids
from .readlist import READ_DIR
from . import days, forecasts
from . import sections as paper
from .links import listing as linkable
from .pieces import pieces

RECENT_DAYS = 7  # read, play and puzzle results cover the last week


def _records(path: Path) -> list[dict]:
    """Records of a host file, skipping lines that do not parse (validation reports those)."""
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if isinstance(obj, dict):
            out.append(obj)
    return out


def _brief(x: Highlight) -> dict:
    """A highlight in brief. One made on a bite or a puzzle names it in `item`; its `book` is
    the item's book, or `stream/<item id>` when it has none."""
    item = x.item.model_dump(exclude_none=True) if x.item else None
    d = {"id": x.id, "book": x.book, "chapter": x.chapter, "item": item, "text": x.text}
    return {k: v for k, v in d.items() if v}


def _cards(reviews: list[Review]) -> tuple[dict, list[str]]:
    """Per-card reps, lapses, last grade and accuracy, and `wrong`: the options picked when it
    was missed. Weak: 2+ lapses, or failed twice in a row."""
    results: dict[str, list[Review]] = {}
    for r in reviews:
        results.setdefault(r.card, []).append(r)
    cards, weak = {}, []
    for key, rs in results.items():
        passed = [r.passed for r in rs]
        lapses = sum(r.grade == 1 for r in rs)
        cards[key] = {"reps": len(rs), "lapses": lapses, "last": rs[-1].grade, "acc": round(sum(passed) / len(rs), 2)}
        if wrong := [r.pick for r in rs if r.pick is not None and not r.passed]:
            cards[key]["wrong"] = wrong
        if lapses >= 2 or passed[-2:] == [False, False]:
            weak.append(key)
    weak.sort(key=lambda k: (cards[k]["acc"], -cards[k]["lapses"]))
    return cards, weak


def _minutes(ms: int | None) -> int:
    return round((ms or 0) / 60000)


def _read(h: Home, info: dict, uploaded: set[str], recent) -> dict:
    """Finished or left, per Read id, in the recent days; and uploaded items not finished yet."""
    prefix = READ_DIR.lstrip("/") + "/"
    logs = [r for r in map(ReadLog.model_validate, read_jsonl(h.archive("read-log"))) if r.book.startswith(prefix)]
    finished = {Path(r.book).stem for r in logs if r.finished}
    results: dict[str, dict] = {}
    for r in logs:
        if not recent(r):
            continue
        rid = Path(r.book).stem
        e = results.setdefault(rid, {"id": rid, **info.get(rid, {}), "finished": False, "progress": 0.0, "ms": 0})
        e["finished"] = e["finished"] or r.finished
        e["progress"] = max(e["progress"], round(r.progress, 2))
        e["ms"] += r.ms or 0
    for e in results.values():
        e["min"] = _minutes(e.pop("ms"))
    waiting = sorted(uploaded - finished)
    return {"results": list(results.values()), "waiting": waiting}


def _play(h: Home, stats: Stats, recent) -> dict:
    """Per game: level and totals from stats.json, and the recent days from play.jsonl."""
    games: dict[str, dict] = {g: s.model_dump() for g, s in stats.play.items()}
    for p in map(Play.model_validate, read_jsonl(h.archive("play"))):
        if not recent(p):
            continue
        g = games.setdefault(p.game, {})
        if p.level is not None and p.game not in stats.play:
            g["level"] = p.level
        r = g.setdefault("recent", {"played": 0, "won": 0, "practice": 0, "ms": 0})
        r["played"] += 1
        r["won"] += p.won
        r["practice"] += p.mode == "practice"
        r["ms"] += p.ms or 0
    for g in games.values():
        if "recent" in g:
            g["recent"]["min"] = _minutes(g["recent"].pop("ms"))
    return games


def _puzzles(h: Home, meta: dict, recent) -> dict:
    """Book puzzle results from seen.jsonl."""
    seen = [s for s in map(Seen.model_validate, read_jsonl(h.archive("seen"))) if s.correct is not None and recent(s)]
    results = [
        {"id": s.id, **meta.get(s.id, {}), "correct": s.correct}
        | ({"pick": s.pick} if s.pick is not None and not s.correct else {})
        | ({"sec": round(s.ms / 1000)} if s.ms else {})
        for s in seen
    ]
    return {"played": len(results), "correct": sum(r["correct"] for r in results), "results": results}


def _predicts(h: Home, meta: dict, recent) -> dict:
    """Guesses made before articles, from seen.jsonl: how often the guess was right, and how
    often the article was then read (only then does the X4 show the answer)."""
    seen = [s for s in map(Seen.model_validate, read_jsonl(h.archive("seen"))) if s.predict and recent(s)]
    # A piece's guess is logged before it is read: whether it was read is in the read log.
    opened = {Path(r["book"]).stem for r in read_jsonl(h.archive("read-log")) if r.get("progress") or r.get("finished")}
    results = [
        {"id": s.id, **meta.get(s.id, {}), "pick": s.predict.get("pick"), "correct": bool(s.predict.get("correct")),
         "read": bool(s.opened) or s.id in opened}
        for s in seen
    ]
    return {"asked": len(results), "right": sum(r["correct"] for r in results), "read": sum(r["read"] for r in results), "results": results}


def _links(h: Home, meta: dict, recent) -> dict:
    """Link questions answered after articles, from seen.jsonl."""
    seen = [s for s in map(Seen.model_validate, read_jsonl(h.archive("seen"))) if s.link and recent(s)]
    results = [{"id": s.id, **meta.get(s.id, {}), "pick": s.link.get("pick"), "correct": bool(s.link.get("correct"))} for s in seen]
    return {"asked": len(results), "right": sum(r["correct"] for r in results), "results": results}


def _lookups(h: Home, seen: int, titles: dict[str, str], info: dict) -> tuple[list[dict], int]:
    """Words looked up in the dictionary since the last call, each once, with the book or Read
    article it was read in; and how many records the log has now."""
    log = [Lookup.model_validate(r) for r in read_jsonl(h.archive("lookups"))]
    out: dict[str, dict] = {}
    for x in log[seen:]:
        word = x.word.strip().lower()
        if not word or word in out:
            continue
        title = x.title or titles.get(x.book) or info.get(Path(x.book).stem, {}).get("title") or Path(x.book).stem
        out[word] = {"word": word, "book": x.book, "title": title}
    return list(out.values()), len(log)


def context(h: Home) -> dict:
    from .ops import begin

    begin(h)  # a checkout of the data repo starts from the repo's copy and the X4's inbox
    state = read_json(h.context_state, {})
    sync_state = read_json(h.sync_state, {})
    pushed = sync_state.get("pushed", {})
    log = highlight_log(h)  # append-only, so `seen` indexes it
    live = fold(log)
    marks = list(live.values())
    seen = state.get("highlights", 0)
    newer = [x for x in log[seen:] if not x.deleted and live.get(x.id) is x]
    new_ids = {x.id for x in newer}

    decks = {}
    referenced: set[str] = set(state.get("answered", []))
    for f in sorted(h.decks.glob("*.jsonl")):
        rows = _records(f)
        referenced |= {r["highlight"] for r in rows[1:] if isinstance(r.get("highlight"), str)}
        synced = pushed.get(f"/x4/decks/{f.name}") == zlib.crc32(jsonl_bytes(rows))
        decks[f.stem] = {"cards": max(len(rows) - 1, 0), "synced": synced}
        flips = [r["id"] for r in rows[1:] if r.get("type") == "flip" and isinstance(r.get("id"), str)]
        if flips:
            decks[f.stem]["flip"] = flips  # graded by the reader: to rewrite as choice or cloze
    items = _records(h.stream)
    referenced |= {r["highlight"] for r in items if isinstance(r.get("highlight"), str)}

    questions = [
        {**_brief(x), "new": x.id in new_ids} for x in marks if x.kind == "question" and x.id not in referenced
    ]
    fresh = [_brief(x) for x in newer if x.kind == "highlight"]
    # Deleted on the device: new since the last call, or still named by a card or stream item.
    newly_deleted = {x.id for x in log[seen:] if x.deleted}
    deleted = sorted({x.id for x in log if x.deleted and x.id not in live} & (newly_deleted | referenced))

    reviews = [Review.model_validate(r) for r in read_jsonl(h.archive("reviews"))]
    cards, weak = _cards(reviews)

    stats = Stats.model_validate(read_json(h.stats, {}))
    titles = {x.book: x.title for x in marks if x.title}
    counts: dict[str, int] = {}
    for x in marks:
        counts[x.book] = counts.get(x.book, 0) + 1
    articles = READ_DIR.lstrip("/") + "/"  # reported under "read"
    book_keys = sorted(
        k for k in set(stats.books) | set(counts) if not k.startswith(articles) and not k.startswith(STEP_BOOK)
    )
    notes = note_paths(h, [b for b in book_keys if b in counts], titles)
    recaps = {r["book"]: r.get("progress") for r in _records(h.previously) if isinstance(r.get("book"), str)}
    picks = {r["after"]: r.get("books") for r in _records(h.next) if isinstance(r.get("after"), str)}
    books = {}
    for key in book_keys:
        b = stats.books.get(key)
        entry = {
            "title": titles.get(key) or Path(key).stem,
            "progress": round(b.progress, 2) if b else None,
            "read_min": round(b.read_ms / 60000) if b else None,
            "highlights": counts.get(key, 0),
            "glossary": h.glossary_file(key).exists(),
            "notes": str(notes[key].relative_to(h.root)) if key in notes else None,
            "previously": recaps.get(key),
            "next": picks.get(key),
        }
        books[key] = {k: v for k, v in entry.items() if v is not None}

    listed = read_ids(h)
    uploaded = {i for i in listed if f"{READ_DIR}/{i}.epub" in pushed}
    stream_pushed = False
    if h.stream.exists():
        try:
            stream_pushed = pushed.get(STREAM) == zlib.crc32(check_local_stream(h, h.stream).body)
        except InvalidData:
            pass
    seen_ids = {r["id"] for r in read_jsonl(h.archive("seen"))}
    unseen = [{k: r[k] for k in ("id", "kind", "title") if k in r} for r in items if r.get("id") not in seen_ids]

    lookups, lookup_count = _lookups(h, state.get("lookups", 0), titles, sync_state.get("read", {}))

    def recent(record) -> bool:
        return days.within(record, RECENT_DAYS)

    from .ops import _sent_today

    sections = paper.load(h)
    out = {
        "today": date.today().isoformat(),
        "home": str(h.root),
        "day": stats.day,
        "issue": {"date": date.today().isoformat(), "sent": _sent_today(h) is not None},
        "retention": stats.retention,
        "due": stats.due,
        "sections": [{"id": x.id, "name": x.name} for x in sections or []],
        "pieces": pieces(h),
        "highlights": fresh,
        "lookups": lookups,
        "questions": questions,
        "deleted": deleted,
        "reviews": {"total": len(reviews), "cards": cards, "weak": weak},
        "books": books,
        "decks": decks,
        "stream": {"synced": stream_pushed, "unseen": unseen},
        "read": _read(h, sync_state.get("read", {}), uploaded, recent),
        "play": _play(h, stats, recent),
        "puzzles": _puzzles(h, sync_state.get("puzzles", {}), recent),
        "predict": _predicts(h, sync_state.get("predicts", {}), recent),
        "link": _links(h, sync_state.get("links", {}), recent),
        "linkable": linkable(h),
        "forecasts": forecasts.listing(h, date.today()),
        "interests": interests(h),
        "settings": settings.load(h),
        "inbox": inbox(h)[0],
    }
    write_json(
        pending(h.context_state),
        {"highlights": len(log), "lookups": lookup_count, "answered": sorted(referenced & set(live))},
    )
    return out

