"""`x4 dashboard`: a page on this Mac for what the paper covers. It edits the files in X4_HOME
that are yours (settings, sections, interests, feeds, saved links, fields, football, film) and,
when X4_HOME is a checkout of the data repo, sends each change there so the writer in the cloud
reads it. Only this Mac can open it (127.0.0.1).

A panel shows only when its file is in the folder. This file and dashboard.html also run on
their own, without the x4 package (`python dashboard.py --home <dir>`, needs PyYAML): then the
sections, feeds, interests and saved links can be edited, and nothing is sent anywhere."""

import json
import re
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import yaml

try:
    import httpx

    from . import datarepo, football, settings
    from . import sections as paper
    from .errors import InvalidData, X4Error
    from .home import Home, read_json, write_json
    from .pieces import minutes

    ALONE = False
except ImportError:  # run on its own, with only the folder
    ALONE = True

    class X4Error(Exception):
        def __init__(self, message: str, details: list[str] | None = None):
            super().__init__(message)
            self.message, self.details = message, details or []

    class InvalidData(X4Error):
        pass

    class Home:  # the four files a folder of interests holds
        def __init__(self, root: Path):
            self.root = Path(root)
            self.sections, self.feeds = self.root / "sections.md", self.root / "feeds.yaml"
            self.interests, self.inbox = self.root / "interests.md", self.root / "inbox.md"

    class paper:  # x4's sections.py rules: eight at most, names of 24 bytes, no name twice
        MAX, NAME_BYTES = 8, 24

        @staticmethod
        def section_id(name: str) -> str:
            import unicodedata

            plain = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
            return re.sub(r"[^a-z0-9]+", "-", plain.lower()).strip("-")[:16].strip("-")

        @classmethod
        def load(cls, h):
            if not h.sections.exists():
                return None
            out, errors = [], []
            for line in h.sections.read_text(encoding="utf-8").splitlines():
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                name, _, what = (x.strip() for x in line.partition(":"))
                if len(name.encode()) > cls.NAME_BYTES:
                    errors.append(f"'{name}' is longer than {cls.NAME_BYTES} bytes")
                if any(cls.section_id(o.name) == cls.section_id(name) for o in out):
                    errors.append(f"'{name}' is there twice")
                out.append(type("Section", (), {"name": name, "what": what})())
            if len(out) > cls.MAX:
                errors.append(f"{len(out)} sections; the front page holds {cls.MAX}")
            if errors:
                raise InvalidData("sections.md: invalid sections", errors)
            return out

PORT = 8745
TEAMS_DAYS = 30  # how long a competition's team list is kept
COMPETITIONS = {  # football-data.org's free tier
    "PL": "Premier League", "PD": "La Liga", "SA": "Serie A", "BL1": "Bundesliga", "FL1": "Ligue 1",
    "CL": "Champions League", "DED": "Eredivisie", "PPL": "Primeira Liga", "ELC": "Championship",
    "BSA": "Brasileirão",
}  # fmt: skip
SECTIONS_HEAD = "# The paper's sections, in the front page's order.\n# One line each: the name, a colon, what belongs. Edit freely; eight at most.\n\n"
FEEDS_HEAD = "# Sources for `x4 feed`, one feed a line. topic: carried into each entry's topics.\n"
FIELDS_HEAD = (
    "# Fields\n\nOne field a week, in this order, each far from the one before. This list is yours: reorder\n"
    "it, delete lines, add your own. Claude only adds the week to a line once it has been used.\n\n"
)
FIELD_LINE = re.compile(r"^- (.+?)(?: \((\d{4}-w\d{2})\))?\s*$")
TEXTS = ("interests", "taste", "seen")


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8") if path.exists() else ""


def _sections(h: Home) -> list[dict]:
    return [{"name": s.name, "what": s.what} for s in paper.load(h) or []]


def _feeds(h: Home) -> list[dict]:
    if not h.feeds.exists():
        return []
    data = yaml.safe_load(_read(h.feeds)) or {}
    return [{"name": f.get("name", ""), "url": f.get("url", ""), "topic": f.get("topic", "")} for f in data.get("feeds") or []]


def _fields(h: Home) -> list[dict]:
    out = []
    for line in _read(h.root / "fields.md").splitlines():
        if m := FIELD_LINE.match(line.strip()):
            out.append({"name": m[1].strip(), "week": m[2]})
    return out


def _football(h: Home) -> dict:
    try:
        cfg = football.load(h)
    except InvalidData as e:
        return {"error": e.message, "clubs": [], "competitions": [], "names": {}, "key": False}
    return {
        "clubs": cfg["clubs"],
        "competitions": cfg["competitions"],
        "names": {str(k): v for k, v in cfg["names"].items()},
        "key": bool(cfg["key"]),
        "all": COMPETITIONS,
    }


def state(h: Home) -> dict:
    """Everything the page shows: one key per file in the folder, none for a file that is not."""
    sections = _sections(h)
    mins = {}
    if not ALONE:
        try:
            mins = minutes(h, [paper.section_id(s["name"]) for s in sections], {})
        except Exception:  # the records are the X4's; a page should open without them
            pass
    out = {"home": str(h.root), "repo": False if ALONE else datarepo.is_checkout(h)}
    if h.sections.exists():
        out["sections"] = [{**s, "minutes": mins.get(paper.section_id(s["name"]), 0)} for s in sections]
        out["fixed"] = None if ALONE else {k: mins.get(k, 0) for k in ("play", "book")}
    if h.interests.exists():
        out["interests"] = _read(h.interests)
    if h.feeds.exists():
        out["feeds"] = _feeds(h)
    if h.inbox.exists():
        out["inbox"] = [line.strip() for line in _read(h.inbox).splitlines() if line.strip() and not line.startswith("#")]
    if ALONE:
        return out
    out["settings"] = settings.load(h)  # x4 always has settings: language defaults to English
    if (h.root / "fields.md").exists():
        out["fields"] = _fields(h)
    if h.football_cfg.exists():
        out["football"] = _football(h)
    for key, path in (("taste", h.taste), ("seen", h.seen_films)):
        if path.exists():
            out[key] = _read(path)
    return out


# Writers: each takes the page's value, writes its file, and returns the file's name.


def _write_sections(h: Home, value: list[dict]) -> str:
    named = [s for s in value if s.get("name", "").strip()]
    if bad := [s["name"].strip() for s in named if ":" in s["name"] or "\n" in s["name"]]:
        raise InvalidData("sections.md: invalid sections", [f"'{n}': a name has no colon" for n in bad])
    lines = [f"{s['name'].strip()}: {' '.join(s.get('what', '').split())}" for s in named]
    old = _read(h.sections)
    h.sections.write_text(SECTIONS_HEAD + "\n".join(lines) + "\n", encoding="utf-8")
    try:
        paper.load(h)
    except InvalidData:
        h.sections.write_text(old, encoding="utf-8") if old else h.sections.unlink()
        raise
    return h.sections.name


def _write_feeds(h: Home, value: list[dict]) -> str:
    """Keep the file's other keys and, when the list only lost or gained lines, its comments."""
    feeds = []
    for f in value:
        url = f.get("url", "").strip()
        if not url.startswith(("http://", "https://")):
            raise InvalidData("feeds.yaml: invalid", [f"'{url}' is not a link; a feed is an http(s) address"])
        item = {"name": f.get("name", "").strip() or url, "url": url}
        if f.get("topic", "").strip():
            item["topic"] = f["topic"].strip().lower()
        feeds.append(item)
    old = _read(h.feeds)
    data = yaml.safe_load(old) or {} if old else {}
    data["feeds"] = feeds
    text = _edit_lines(old, feeds) if old else None
    if text is None or (yaml.safe_load(text) or {}).get("feeds") != feeds:
        text = FEEDS_HEAD + yaml.safe_dump(data, allow_unicode=True, sort_keys=False, width=200)
    h.feeds.write_text(text, encoding="utf-8")
    return h.feeds.name


def _flow(f: dict) -> str:
    topic = f", topic: {f['topic']}" if f.get("topic") else ""
    return f"  - {{name: {json.dumps(f['name'], ensure_ascii=False)}, url: {json.dumps(f['url'])}{topic}}}"


def _edit_lines(old: str, feeds: list[dict]) -> str | None:
    """Drop the lines of removed feeds and append new ones under `feeds:`, leaving the rest of
    the file as written. None when the file is not one feed a line."""
    lines = old.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.rstrip() == "feeds:")
    except StopIteration:
        return None
    if any(re.match(r"^\S", line) and not line.startswith("#") for line in lines[start + 1 :]):
        return None  # another key after feeds:
    urls = {f["url"] for f in feeds}
    have, out = set(), []
    for line in lines:
        m = re.search(r"url:\s*\"?([^\",}]+)\"?", line) if line.lstrip().startswith("- ") else None
        if m and m[1].strip() not in urls:
            continue
        if m:
            have.add(m[1].strip())
        out.append(line)
    out += [_flow(f) for f in feeds if f["url"] not in have]
    return "\n".join(out) + "\n"


def _write_inbox(h: Home, value: list[str]) -> str:
    links = [v.strip() for v in value if v.strip()]
    bad = [v for v in links if not v.startswith(("http://", "https://"))]
    if bad:
        raise InvalidData("inbox.md: invalid", [f"'{v[:60]}' is not a link" for v in bad])
    h.inbox.write_text("".join(f"{v}\n" for v in links), encoding="utf-8")
    return h.inbox.name


def _write_fields(h: Home, value: list[dict]) -> str:
    path = h.root / "fields.md"
    old = _read(path)
    head = old[: old.find("\n- ") + 1] if "\n- " in old else FIELDS_HEAD
    lines = [f"- {f['name'].strip()}" + (f" ({f['week']})" if f.get("week") else "") for f in value if f.get("name", "").strip()]
    path.write_text(head + "\n".join(lines) + "\n", encoding="utf-8")
    return path.name


def _write_football(h: Home, value: dict) -> str:
    if key := (value.get("key") or "").strip():
        h.football_key.write_text(key + "\n", encoding="utf-8")
    cfg = {
        "clubs": [int(c) for c in value.get("clubs", [])],
        "competitions": [c for c in value.get("competitions", []) if c in COMPETITIONS],
    }
    if names := {int(k): v for k, v in (value.get("names") or {}).items() if v}:
        cfg["names"] = names
    head = "# What x4 football fetches from football-data.org. The key is in football.key, never here.\n"
    h.football_cfg.write_text(head + yaml.safe_dump(cfg, allow_unicode=True, sort_keys=False, default_flow_style=None), encoding="utf-8")
    football.load(h)
    return h.football_cfg.name


def _write_text(path: Path):
    def write(h: Home, value: str) -> str:
        path_ = h.root / path
        path_.write_text(value.rstrip() + "\n" if value.strip() else "", encoding="utf-8")
        return path_.name

    return write


def _write_settings(h: Home, value: dict) -> str:
    old = _read(h.settings)
    try:
        settings.save(h, {**settings.load(h), **value})
    except InvalidData:
        h.settings.write_text(old, encoding="utf-8") if old else h.settings.unlink(missing_ok=True)
        raise
    return h.settings.name


WRITERS = {
    "sections": _write_sections,
    "feeds": _write_feeds,
    "inbox": _write_inbox,
    "interests": _write_text(Path("interests.md")),
}
if not ALONE:
    WRITERS |= {
        "settings": _write_settings,
        "fields": _write_fields,
        "football": _write_football,
        "taste": _write_text(Path("taste.md")),
        "seen": _write_text(Path("seen.md")),
    }


def save(h: Home, what: str, value) -> dict:
    """Write one file and, in a data repo, send it. A refused value leaves the file as it was."""
    if what not in WRITERS:
        raise InvalidData(f"nothing called {what}", [f"one of {', '.join(WRITERS)}"])
    name = WRITERS[what](h, value)
    sent = None
    if not ALONE and datarepo.is_checkout(h):
        try:
            sent = bool((datarepo.send_after(h, f"dashboard: {name}") or {}).get("sent"))
        except X4Error as e:
            return {"saved": name, "sent": False, "warning": e.message}
    return {"saved": name, "sent": sent}


def teams(h: Home, code: str, client: "httpx.Client | None" = None) -> list[dict]:
    """A competition's clubs, kept for TEAMS_DAYS in X4_HOME/cache."""
    if code not in COMPETITIONS:
        raise InvalidData(f"no competition {code}", [f"one of {', '.join(COMPETITIONS)}"])
    cache = h.root / "cache" / f"teams-{code}.json"
    kept = read_json(cache, {})
    if kept and time.time() - kept.get("at", 0) < TEAMS_DAYS * 86400:
        return kept["teams"]
    cfg = football.load(h)
    if not cfg["key"]:
        raise InvalidData("no football key", ["add your football-data.org key first"])
    with client or httpx.Client(timeout=20) as c:
        data = football.Api(cfg["key"], c).get(f"/competitions/{code}/teams")
    out = sorted(({"id": t["id"], "name": t.get("shortName") or t["name"]} for t in data.get("teams", [])), key=lambda t: t["name"])
    cache.parent.mkdir(parents=True, exist_ok=True)
    write_json(cache, {"at": time.time(), "teams": out})
    return out


def _page() -> bytes:
    return Path(__file__).with_name("dashboard.html").read_bytes()


def handler(h: Home):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args) -> None:
            pass

        def _send(self, code: int, body: bytes, kind: str = "application/json") -> None:
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _json(self, code: int, data) -> None:
            self._send(code, json.dumps(data, ensure_ascii=False).encode())

        def _error(self, e: Exception) -> None:
            if isinstance(e, X4Error):
                self._json(400, {"error": e.message, "details": e.details})
            else:
                self._json(500, {"error": str(e)})

        def do_GET(self) -> None:
            try:
                if self.path in ("/", "/index.html"):
                    return self._send(200, _page(), "text/html; charset=utf-8")
                if self.path == "/api/state":
                    fetched = None
                    if not ALONE and datarepo.is_checkout(h):
                        try:
                            fetched = datarepo.fetch_first(h)
                        except X4Error as e:
                            fetched = {"warning": e.message}
                    return self._json(200, {**state(h), "fetched": fetched})
                if not ALONE and (m := re.fullmatch(r"/api/teams/([A-Z0-9]+)", self.path)):
                    return self._json(200, teams(h, m[1]))
                self._send(404, b"{}")
            except Exception as e:
                self._error(e)

        def do_PUT(self) -> None:
            # Same-origin only: a page elsewhere cannot send JSON here without a preflight.
            if self.headers.get("Content-Type") != "application/json":
                return self._json(415, {"error": "JSON only"})
            m = re.fullmatch(r"/api/([a-z]+)", self.path)
            if not m:
                return self._send(404, b"{}")
            try:
                value = json.loads(self.rfile.read(int(self.headers.get("Content-Length") or 0)))
                self._json(200, save(h, m[1], value))
            except Exception as e:
                self._error(e)

    return Handler


def serve(h: Home, port: int | None = None, open_browser: bool = True, ready=None) -> None:
    server = ThreadingHTTPServer(("127.0.0.1", port or PORT), handler(h))
    url = f"http://127.0.0.1:{server.server_address[1]}/"
    if ready:
        ready({"url": url, "home": str(h.root)})
    if open_browser:
        threading.Timer(0.3, webbrowser.open, (url,)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    import argparse
    import os

    ap = argparse.ArgumentParser(description="Edit the files of interests in a folder.")
    ap.add_argument("--home", default=os.environ.get("X4_HOME") or "~/x4")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--no-open", action="store_true")
    a = ap.parse_args()
    serve(Home(Path(a.home).expanduser()), a.port, not a.no_open, lambda d: print(d["url"], f"({d['home']})"))
