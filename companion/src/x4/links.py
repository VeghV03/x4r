"""What a link question may point at: things the reader read on an earlier day.

A stream article's `link` offers three of them and asks which connects (docs/DATA.md). Each is
named by `ref`: a highlight's id, a finished Read article's id, or a book's key. `x4 context`
lists them (`linkable`), and a stream whose link names anything else is refused, so a link
never points at something the reader did not read.
"""

from pathlib import Path

from . import days
from .home import Home, read_json, read_jsonl
from .models import STEP_BOOK, Highlight, ReadLog, Stats
from .readlist import READ_DIR

TEXT = 160  # characters of a highlight shown to Claude
SHOWN = 80  # highlights listed, the oldest first


def _live(h: Home) -> list[Highlight]:
    live: dict[str, Highlight] = {}
    for x in map(Highlight.model_validate, read_jsonl(h.archive("highlights"))):
        if x.deleted:
            live.pop(x.id, None)
        else:
            live.setdefault(x.id, x)
    return list(live.values())


def linkable(h: Home) -> dict[str, dict]:
    """ref -> what it is: `kind` (highlight, article or book), `title`, a highlight's `text`,
    and the `day` it was read on. Only what was read before today."""
    stats = Stats.model_validate(read_json(h.stats, {}))
    articles = READ_DIR.lstrip("/") + "/"
    today = days.today()

    def earlier(record) -> bool:
        day = days.of(record)
        return day is None or day < today

    marks = _live(h)
    titles = {x.book: x.title for x in marks if x.title}
    out: dict[str, dict] = {}
    for x in marks:
        if x.kind == "highlight" and x.text and earlier(x):
            title = x.title or (x.item.title if x.item else None) or Path(x.book).stem
            out[x.id] = {"kind": "highlight", "title": title, "text": x.text[:TEXT], "day": days.of(x)}
    known = read_json(h.sync_state, {}).get("read", {})
    for r in map(ReadLog.model_validate, read_jsonl(h.archive("read-log"))):
        rid = Path(r.book).stem
        if r.finished and r.book.startswith(articles) and earlier(r) and rid in known:
            out.setdefault(rid, {"kind": "article", "title": known[rid]["title"], "day": days.of(r)})
    for key, b in stats.books.items():
        if b.read_ms > 0 and not key.startswith(articles) and not key.startswith(STEP_BOOK):
            out[key] = {"kind": "book", "title": titles.get(key) or Path(key).stem}
    return out


def listing(h: Home) -> dict:
    """`linkable` for `x4 context`: by kind, the oldest first."""
    found = linkable(h)

    def of(kind: str) -> list[dict]:
        rows = [{"ref": ref, **{k: v for k, v in x.items() if k != "kind" and v is not None}} for ref, x in found.items() if x["kind"] == kind]
        return sorted(rows, key=lambda r: r.get("day") or 0)

    marks = of("highlight")
    out = {"highlights": marks[:SHOWN], "articles": of("article"), "books": of("book")}
    if len(marks) > SHOWN:
        out["highlights_more"] = len(marks) - SHOWN
    return out


def errors(h: Home, name: str, records: list[dict]) -> list[str]:
    """Every `ref` of every link must be something read earlier, each once, and not the
    article the link stands on."""
    found = None
    out = []
    for r in records:
        link = r.get("link") if r["kind"] == "article" else None
        if not link:
            continue
        found = linkable(h) if found is None else found
        where = f"{name}: {r['id']}: link"
        refs = [o["ref"] for o in link["options"]]
        if len(set(refs)) != len(refs):
            out.append(f"{where}: two options name the same thing; give three different ones")
        for i, ref in enumerate(refs):
            if ref == Path(r["book"]).stem:
                out.append(f"{where}.options.{i}.ref: '{ref}' is this article; name something read earlier")
            elif ref not in found:
                out.append(
                    f"{where}.options.{i}.ref: no record that '{ref}' was read on an earlier day; "
                    "take a highlight id, a finished article's id or a book key from `linkable` in `x4 context`"
                )
    return out
