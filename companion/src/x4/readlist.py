"""The Read list: ~/x4/read.jsonl, one `url` or `file` per item, made ready for the device.

A `file` is a saved web page (.html, cleaned by trafilatura) or an article Claude wrote
(.md with front matter, converted as written: convert/markdown.py).

Each article is converted once to EPUB and cached by URL in X4_HOME/cache. `x4 sync` uploads
it as /books/read/<id>.epub and writes /x4/read.jsonl with `book` keys (docs/DATA.md).
"""

import hashlib
import shutil
import tempfile
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from urllib.parse import urlsplit

from .errors import InvalidData
from .home import Home, read_json, read_jsonl, write_json
from .models import ReadSource, check_read, jsonl_bytes

WPM = 230
MARKDOWN = {".md", ".markdown"}
# Caps for articles Claude writes, at the user's settings (about 95 words a reader page):
# a translation digest and a match analysis are short, anything else 8 pages.
WORDS_PER_PAGE = 95
ARTICLE_WORDS_MAX = 750
WORDS_MAX = {"translation": 450, "analysis": 450}  # analysis: about 400 (D51)
READ_DIR = "/books/read"
READ_LIST = "/x4/read.jsonl"


@dataclass(frozen=True)
class Ready:
    record: dict  # the device's /x4/read.jsonl line
    epub: Path
    url: str | None
    file: str | None = None  # as the Read list names it

    @property
    def remote(self) -> str:
        return f"{READ_DIR}/{self.record['id']}.epub"


def _domain(url: str) -> str:
    return urlsplit(url).hostname.removeprefix("www.") if urlsplit(url).hostname else url


def _source(h: Home, item: ReadSource) -> tuple[str, str, Path | None]:
    """Cache key, freshness stamp, and the local file if the item has one."""
    if item.url:
        return item.url, "", None
    path = Path(item.file).expanduser()
    if not path.is_absolute():
        path = h.root / path
    st = path.stat()
    return f"file:{path.resolve()}", f"{st.st_mtime_ns}:{st.st_size}", path


def _convert(h: Home, key: str, stamp: str, item: ReadSource, path: Path | None) -> dict:
    import lxml.html  # the converters load slowly; only sync needs them

    from .convert import net
    from .convert.article import extract, write_article
    from .convert.epub import guess_language

    if path is None:
        article = extract(net.fetch(item.url), item.url)
    elif path.suffix.lower() in MARKDOWN:
        from .convert.markdown import read_markdown

        article = read_markdown(path)  # Claude's own article: no extraction
    else:
        article = extract(path.read_bytes(), path.as_uri())
    if article is None:
        raise ValueError("no article text found")
    folder = h.cache / "read"
    folder.mkdir(parents=True, exist_ok=True)
    name = hashlib.sha1(key.encode()).hexdigest()[:16] + ".epub"
    with tempfile.TemporaryDirectory() as tmp:
        shutil.move(write_article(article, Path(tmp)).local, folder / name)
    text = lxml.html.fromstring(article.body).text_content() if article.body.strip() else ""
    title = article.title if article.title != article.url else (path.stem if path else item.url)
    return {
        "epub": f"read/{name}",
        "stamp": stamp,
        "title": title,
        "source": article.site or (_domain(item.url) if item.url else path.name),
        "lang": article.lang or guess_language(text),
        "minutes": max(1, round(len(text.split()) / WPM)),
        "fetched": date.today().isoformat(),
    }


def too_long(h: Home, items: list[ReadSource]) -> list[str]:
    """Errors for Claude's Markdown articles over their kind's word cap, front matter excluded."""
    errors = []
    for item in items:
        if not item.file:
            continue
        _, _, path = _source(h, item)
        if path is None or path.suffix.lower() not in MARKDOWN or not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        if text.startswith("---"):
            text = text.split("\n---", 1)[-1]
        words = len(text.split())
        cap = WORDS_MAX.get(item.kind, ARTICLE_WORDS_MAX)
        if words > cap:
            pages = -(-cap // WORDS_PER_PAGE)
            errors.append(f"{h.read}: {item.id}: {path.name} is {words} words; cut it to {cap} ({pages} reader pages)")
    return errors


def prepare(h: Home, items: list[ReadSource]) -> tuple[list[Ready], list[str]]:
    """Fetch and convert every item not cached yet. A failed item is left out with a warning."""
    index_path = h.cache / "read.json"
    index: dict = read_json(index_path, {})
    ready, warnings = [], []
    for item in items:
        try:
            key, stamp, path = _source(h, item)
            entry = index.get(key)
            if not entry or entry.get("stamp") != stamp or not (h.cache / entry["epub"]).exists():
                entry = index[key] = _convert(h, key, stamp, item, path)
        except Exception as e:  # noqa: BLE001 - one failed article never fails the sync
            what = f"{item.url} not fetched" if item.url else f"{item.file} not read"
            warnings.append(f"{h.read}: {item.id}: {what} ({e}); left out")
            continue
        record = {
            "id": item.id,
            "kind": item.kind,
            "title": item.title or entry["title"],
            "source": item.source or entry["source"],
            "lang": item.lang or entry["lang"],
            "minutes": item.minutes or entry["minutes"],
            "book": f"{READ_DIR.lstrip('/')}/{item.id}.epub",
        }
        record |= {k: v for k in ("section", "head", "lead", "kept", "thread") if (v := getattr(item, k)) is not None}
        record |= {k: v.model_dump() for k in ("predict", "link", "forecast") if (v := getattr(item, k)) is not None}
        ready.append(Ready(record, h.cache / entry["epub"], item.url, item.file))
    h.cache.mkdir(parents=True, exist_ok=True)
    write_json(index_path, index)
    return ready, warnings


def article(h: Home, read_id: str) -> dict:
    """A Read list article's text as the X4 has it, fetched and converted now if it was not yet.
    A predict question quotes from this."""
    from .previously import text_before

    items = {i.id: i for i in check_read(h.read)} if h.read.exists() else {}
    if read_id not in items and read_id in (old := sent(h)):
        # No longer listed, but sent once: a kept piece, or one a link points back to.
        items[read_id] = ReadSource.model_validate({k: old[read_id][k] for k in ("id", "kind", "url", "file") if old[read_id].get(k)})
    if read_id not in items:
        raise InvalidData(f"{read_id} is not in {h.read}", [f"{h.read}: no item with id {read_id!r}"])
    ready, warnings = prepare(h, [items[read_id]])
    if not ready:
        raise InvalidData(f"{read_id} cannot be read", warnings)
    _, text = text_before(ready[0].epub, 1.0, 1 << 30)
    return {"id": read_id, "title": ready[0].record["title"], "words": len(text.split()), "text": text}


def sent(h: Home) -> dict[str, dict]:
    """Read id -> its line in X4_HOME/sent.jsonl: `id`, `kind`, `title`, `url` or `file`, the day
    it was first `sent`, and `asked` once a set article asked a question about it. A piece of a
    section has `section`, the `issue` that first listed it and `listed`, every issue that did."""
    return {r["id"]: r for r in read_jsonl(h.sent)} if h.sent.exists() else {}


def note_sent(h: Home, ready: list[Ready], today: date) -> None:
    """An issue went out: its Read items are remembered, each once, and a piece of a section
    adds the issue to those that listed it."""
    known = sent(h)
    issue = today.isoformat()
    changed = False
    for r in ready:
        line = known.get(r.record["id"])
        if line is None:
            line = {"id": r.record["id"], "kind": r.record["kind"], "title": r.record["title"], "sent": issue}
            if "section" in r.record:
                line |= {"section": r.record["section"], "issue": issue}
            line["url" if r.url else "file"] = r.url or r.file
            known[line["id"]] = line
            changed = True
        if line.get("section") and issue not in line.get("listed", []):
            before = line.get("listed") or ([line["issue"]] if line.get("issue") else [])
            line["listed"] = list(dict.fromkeys([*before, issue]))
            changed = True
    if changed:
        h.sent.write_bytes(jsonl_bytes(list(known.values())))


def mark_asked(h: Home, read_ids: set[str]) -> None:
    known = sent(h)
    changed = [i for i in read_ids if i in known and not known[i].get("asked")]
    for i in changed:
        known[i]["asked"] = True
    if changed:
        h.sent.write_bytes(jsonl_bytes(list(known.values())))


# Query parameters that say where a link was clicked, not which page it is: these names, and
# any that starts with `utm_`.
TRACKING = {"traffic_source", "at_medium", "at_campaign", "maca", "fbclid", "gclid", "ref", "source"}


def same_page(url: str) -> str:
    """A URL as two links to one page are compared: without the scheme, `www.`, a trailing
    slash, the fragment and the parameters that only track where the link came from."""
    parts = urlsplit(url.strip())
    host = (parts.hostname or "").removeprefix("www.")
    names = [(p, p.split("=")[0].lower()) for p in parts.query.split("&") if p]
    query = "&".join(p for p, name in names if name not in TRACKING and not name.startswith("utm_"))
    return f"{host}{parts.path.rstrip('/')}" + (f"?{query}" if query else "")


def already_sent(h: Home, items: list[ReadSource]) -> list[str]:
    """Errors for pages that went out before under another id. An item kept on the list keeps
    its id, so it passes; an article the reader should get again is listed under its old id.
    Files are exempt: a dossier is one standing file, sent anew each week."""
    before = {same_page(x["url"]): x for x in sent(h).values() if x.get("url")}
    errors = []
    for item in items:
        old = before.get(same_page(item.url)) if item.url else None
        if old and old["id"] != item.id:
            errors.append(
                f"{h.read}: {item.id}: url: this page was already sent as {old['id']} on {old['sent']} "
                f"('{old['title']}'); drop it, or list it under that id to send it again"
            )
    return errors
