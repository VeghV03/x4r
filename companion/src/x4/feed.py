"""`x4 feed`: every candidate for the next sync in one object. It fetches; Claude chooses.

- news: an optional news engine (settings.yaml news_engine), run in place (snapshot, then discover for each day since the
  last run). Each source URL is reported once, so a story seen before comes back only with
  new coverage, and ranks by that new coverage. A story is used up only when it was put in
  front of a writer who could print it: a run after the day's issue went out holds what it
  found for the next issue, and so does a story below the first MAX_NEWS. The top stories
  carry `primary`: the acts, rulings and documents they rest on (primary.py).
- follow: entries from ~/x4/feeds.yaml not reported before, a few per feed and per topic.
- inbox: links saved in ~/x4/inbox.md.
- interests: ~/x4/interests.md as written.

Stories and entries carry `topics` (topics.py). With a topic, only its items come back and
nothing is marked as reported, so a narrow look leaves the full run intact.

Nothing here fails the command: a missing engine, Node or feed becomes a `warnings` line.
"""

import html
import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from datetime import date, datetime, timedelta
from pathlib import Path
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

import feedparser
import lxml.html
import yaml
from lxml import etree

from . import primary
from .convert import net
from .convert.rss import _entry_id, _entry_time
from .home import Home, pending, read_json, write_json
from . import settings
from .topics import tag

NODE_MAJOR = 22
TIMEOUT = 600  # seconds per engine script; the snapshot fetches ~15 feeds one by one
MAX_DAYS = 14  # discover at most this many past days
MAX_NEWS = 60  # the engine's own cap for a day: the writer chooses, so it sees them all
KEEP_DAYS = 30  # how long a reported news URL is remembered
HELD_DAYS = 2  # how long a story nobody could print waits for an issue
SUMMARY_CHARS = 200
PER_FEED = 3  # follow entries per feed and per topic; feeds.yaml max_per_feed, max_per_topic
PER_TOPIC = 10

# From the engine's scripts/snapshot-feeds.mjs and discover.mjs.
FOREIGN = {
    "aljazeera", "reuters", "ap", "politico-eu", "euractiv", "bbc", "dw", "guardian", "spiegel", "nyt", "lemonde",
    "france24", "ft", "wsj", "economist", "timesofisrael", "haaretz", "kyivindependent", "pravda-ua",
}  # fmt: skip
FOREIGN_LANG = {"lefigaro": "fr", "faz": "de"}  # the rest of FOREIGN reads in English
WIRES = {"reuters", "ap"}  # channels, not sources: they relay what someone else found out
NO_FETCH = {"24", "24.hu"}  # publishers that refuse article downloads
# Counted in k, never the piece (K22, K23): the government-aligned press, and the engine's `paywalled` feeds.
ALIGNED = {"index", "magyarnemzet", "mandiner", "origo"}
PAYWALLED = {"nyt", "lemonde", "ft", "wsj", "economist", "lefigaro", "faz", "haaretz"}
HU_LETTERS = set("áéíóöőúüű")


class EngineError(Exception):
    """An engine script failed; the message becomes a warning."""




def check_node() -> str | None:
    """A warning if Node 22+ is not available, else None."""
    node = shutil.which("node")
    if not node:
        return f"news: Node.js not found; install Node {NODE_MAJOR}+ (brew install node)"
    try:
        version = subprocess.run([node, "--version"], capture_output=True, text=True, timeout=30).stdout.strip()
        major = int(version.lstrip("v").split(".")[0])
    except (OSError, ValueError, subprocess.SubprocessError):
        return f"news: {node} --version failed; reinstall Node {NODE_MAJOR}+"
    if major < NODE_MAJOR:
        return f"news: Node {version} is too old; install Node {NODE_MAJOR}+ (brew upgrade node)"
    return None


# What the engine may see: the shell basics, locale, network and TLS settings, and its own
# LATOTER_* switches. API keys and other secrets in the user's environment stay out.
ENGINE_ENV = {"PATH", "HOME", "USER", "LOGNAME", "SHELL", "TMPDIR", "LANG", "TZ", "SSL_CERT_FILE", "SSL_CERT_DIR"}
ENGINE_ENV_PREFIXES = ("LC_", "LATOTER_", "NODE_EXTRA_CA_CERTS")
PROXY_ENV = {"http_proxy", "https_proxy", "no_proxy", "all_proxy"}


def engine_env() -> dict[str, str]:
    """A minimal environment without this tool's virtualenv: the engine's scripts call the
    user's python3 (kozlony.mjs reads PDFs with pdfplumber when pdftotext is missing).

    Node's fetch ignores proxy variables unless told otherwise; where the network has a proxy
    (the cloud run), the engine must use it like every other tool, or it reaches only the
    hosts the direct route allows."""
    env = {
        k: v
        for k, v in os.environ.items()
        if k in ENGINE_ENV or k.startswith(ENGINE_ENV_PREFIXES) or k.lower() in PROXY_ENV
    }
    if any(k.lower() in ("https_proxy", "all_proxy") for k in env):
        env["NODE_USE_ENV_PROXY"] = "1"
    if sys.prefix != sys.base_prefix:
        own = str(Path(sys.prefix) / "bin")
        env["PATH"] = os.pathsep.join(p for p in env.get("PATH", "").split(os.pathsep) if p.rstrip("/") != own)
    return env


def run_node(root: Path, script: str, args: list[str], timeout: int = TIMEOUT) -> str:
    """Run one engine script with the engine folder as working directory; returns its output."""
    try:
        r = subprocess.run(
            ["node", f"scripts/{script}", *args], cwd=root, env=engine_env(), capture_output=True, text=True, timeout=timeout
        )
    except subprocess.TimeoutExpired:
        raise EngineError(f"{script} timed out after {timeout} s") from None
    except OSError as e:
        raise EngineError(f"{script} did not start ({e})") from None
    if r.returncode:
        last = ((r.stderr or "").strip() or (r.stdout or "").strip()).splitlines()[-1:] or [""]
        raise EngineError(f"{script} failed (exit {r.returncode}): {last[0][:200]}")
    return r.stdout or ""


# news


def _unreached(report: str | None, orgs: dict[str, str]) -> list[str]:
    """A warning naming the sources the snapshot could not fetch, from its report's
    'Nem élő: a, b' line, so a run's report says which outlets it did not read."""
    m = re.search(r"Nem élő: (.+?) —", report or "")
    if not m:
        return []
    names = [orgs.get(i.strip(), i.strip()) for i in m.group(1).split(",") if i.strip()]
    return [f"news: not reached: {', '.join(names)}"] if names else []


def _clean_url(url: str) -> str:
    """Unescape and drop tracking parameters, so one article has one URL."""
    url = html.unescape(url.strip())
    parts = urlsplit(url)
    query = [(k, v) for k, v in parse_qsl(parts.query, keep_blank_values=True) if not k.startswith("utm_")]
    return urlunsplit(parts._replace(query=urlencode(query), fragment=""))


def _lang(brand: str, title: str) -> str:
    if brand in FOREIGN_LANG:
        return FOREIGN_LANG[brand]
    if brand in FOREIGN:
        return "en"
    return "hu" if HU_LETTERS & set(title.lower()) else "en"


def _orgs(root: Path) -> dict[str, str]:
    """Feed id -> organisation name, from the engine's snapshot manifest."""
    out: dict[str, str] = {}
    manifest = root / "out" / "feed-snapshots" / "manifest.jsonl"
    if manifest.exists():
        for line in manifest.read_text(encoding="utf-8").splitlines():
            try:
                rec = json.loads(line)
                out[str(rec["id"])] = str(rec["org"])
            except (ValueError, KeyError, TypeError):
                continue
    return out


def _candidates(data: dict, orgs: dict[str, str], day: str) -> list[dict]:
    """The engine's ranked clusters as stories with deduplicated sources."""
    stories = []
    for c in data.get("candidates") or []:
        sources: dict[str, dict] = {}
        official: dict[str, dict] = {}  # the engine's constitutive members: the record itself, not coverage
        for m in c.get("members") or []:
            url = _clean_url(str(m.get("link") or ""))
            if not url.startswith(("http://", "https://")) or url in sources or url in official:
                continue
            brand = str(m.get("brand") or "")
            title = str(m.get("title") or "").strip()
            if brand in primary.CONSTITUTIVE:
                official[url] = {"title": title, "url": url, "kind": primary.CONSTITUTIVE[brand], "date": day}
                continue
            sources[url] = {"org": orgs.get(brand, brand), "url": url, "lang": _lang(brand, title), "title": title, "brand": brand}
            if brand in PAYWALLED:
                sources[url]["paywalled"] = True
            if brand in ALIGNED or brand in PAYWALLED:
                sources[url]["piece"] = False
        if sources:
            stories.append({
                "day": day, "claim": c.get("claimKey"), "score": c.get("pont") or 0,
                "sources": list(sources.values()), "primary": list(official.values()),
            })  # fmt: skip
    return stories


def _k(sources: list[dict]) -> int:
    """Independent sources: distinct organisations, wires not counted unless alone."""
    return len({s["brand"] for s in sources if s["brand"] not in WIRES}) or 1


def _story(raw: dict) -> dict:
    # Sources that can be the piece first, then the ones that refuse downloads, then the rest.
    sources = sorted(raw["sources"], key=lambda s: ("piece" in s, s["brand"] in NO_FETCH))
    title = sources[0]["title"]
    out = {"title": title, "k": _k(sources), "day": raw["day"]}
    out["sources"] = [
        {k: v for k, v in s.items() if k != "brand" and not (k == "title" and v == title)} for s in sources
    ]
    if raw.get("claim"):
        out["claim"] = raw["claim"]
    text = " ".join([title] + [s["title"] for s in sources])
    lang = "hu" if any(s["lang"] == "hu" for s in sources) else "en"
    out["topics"] = tag(text, lang, [s["url"] for s in sources] + [p["url"] for p in raw.get("primary", [])])
    if raw.get("primary"):
        out["primary"] = raw["primary"]
    return out


def _days(state: dict, today: date) -> list[str]:
    try:
        first = date.fromisoformat(state["news_day"])
    except (KeyError, TypeError, ValueError):
        first = today
    first = max(first, today - timedelta(days=MAX_DAYS - 1))
    return [(first + timedelta(days=i)).isoformat() for i in range((today - first).days + 1)]


def _discover(state: dict, today: date, root: Path | None) -> tuple[list[dict], list[str]]:
    """The engine's stories for each day since the last issue; a failure is a warning. None without an engine."""
    if root is None:
        return [], []
    if not (root / "scripts" / "discover.mjs").is_file():
        return [], [f"news: no news engine at {root}; set news_engine in settings.yaml or X4_NEWS_ENGINE"]
    if warning := check_node():
        return [], [warning]
    warnings = []
    report = None
    try:
        report = run_node(root, "snapshot-feeds.mjs", [])
    except EngineError as e:
        warnings.append(f"news: {e}")
    orgs = _orgs(root)
    warnings += _unreached(report, orgs)
    raw: list[dict] = []
    discovered = False
    for day in _days(state, today):
        if not (root / "out" / "feed-snapshots" / day).is_dir():
            continue
        try:
            run_node(root, "discover.mjs", [day])
            data = json.loads((root / "out" / "discovery" / f"{day}.json").read_text(encoding="utf-8"))
        except EngineError as e:
            warnings.append(f"news: {e}")
            continue
        except (OSError, ValueError) as e:
            warnings.append(f"news: out/discovery/{day}.json not readable ({e})")
            continue
        discovered = True
        raw += _candidates(data, orgs, day)
    if not discovered:
        return [], warnings or [f"news: no snapshot for {today}; run the engine's snapshot-feeds.mjs to see why"]
    state["news_day"] = today.isoformat()
    return raw, warnings


def _merged(stories: list[dict]) -> list[dict]:
    """One story for clusters that share an article or a claim: the same story found again by
    a later run, or on another day."""
    out: list[dict] = []
    by_key: dict[str, dict] = {}
    for s in stories:
        keys = [x["url"] for x in s["sources"]] + ([f"claim:{s['claim']}"] if s.get("claim") else [])
        into = next((by_key[k] for k in keys if k in by_key), None)
        if into is None:
            into = {**s, "sources": [], "primary": []}
            out.append(into)
        else:
            into["day"] = max(into["day"], s["day"])
            into["score"] = max(into["score"], s["score"])
        for field in ("sources", "primary"):
            have = {x["url"] for x in into[field]}
            into[field] += [x for x in s.get(field) or [] if x["url"] not in have]
        for k in keys:
            by_key[k] = into
    return out


def news(state: dict, today: date, root: Path | None) -> tuple[list[dict], list[str]]:
    """Stories nobody was shown yet, most independent sources first: the held ones and what
    the engine finds now. Nothing is marked here; `feed` decides what was used up."""
    cutoff = (today - timedelta(days=HELD_DAYS)).isoformat()
    held = [s for s in state.get("news_held") or [] if s.get("day", "") >= cutoff]
    found, warnings = _discover(state, today, root)
    seen: dict[str, str] = state.get("news_seen", {})
    stories = []
    for s in _merged(held + found):
        s["sources"] = [x for x in s["sources"] if x["url"] not in seen]
        if s["sources"]:
            stories.append(s)
    stories.sort(key=lambda s: (-_k(s["sources"]), -s["score"], s["day"]))
    return stories, warnings


def _issue_out(h: Home) -> bool:
    """True when today's issue went out already: no piece can be printed until tomorrow."""
    from .ops import _sent_today

    return _sent_today(h) is not None


# follow


def _text(fragment: str) -> str:
    if not fragment.strip():
        return ""
    try:
        text = lxml.html.fromstring(fragment).text_content()
    except (ValueError, etree.ParserError):
        text = re.sub(r"<[^>]+>", " ", fragment)
    return " ".join(text.split())


def _summary(entry) -> str:
    content = entry.get("content") or []
    text = _text(entry.get("summary") or (content[0].get("value", "") if content else ""))
    if len(text) <= SUMMARY_CHARS:
        return text
    return text[:SUMMARY_CHARS].rsplit(" ", 1)[0] + "…"


def _topics_of(feed: dict) -> list[str]:
    value = feed.get("topic") or []
    return [str(t) for t in ([value] if isinstance(value, str) else value)]


def _fetch_feed(url: str):
    return feedparser.parse(net.fetch(url))


def follow(h: Home, state: dict) -> tuple[list[dict], list[str]]:
    """Entries of the feeds in feeds.yaml not reported before: newest first, `max_per_feed` per
    feed and `max_per_topic` per topic, taking each feed's newest entry before anyone's second."""
    if not h.feeds.exists():
        return [], []
    try:
        config = yaml.safe_load(h.feeds.read_text(encoding="utf-8")) or {}
        per_feed = int(config.get("max_per_feed", PER_FEED))
        per_topic = int(config.get("max_per_topic", PER_TOPIC))
        feeds = [f if isinstance(f, dict) else {"url": f} for f in config.get("feeds") or []]
    except (yaml.YAMLError, AttributeError, TypeError, ValueError) as e:
        return [], [f"follow: {h.feeds}: not readable ({e}); write feeds: [{{url, name, topic}}]"]
    seen_by: dict[str, list[str]] = state.get("follow_seen", {})
    warnings, kept, ranked = [], {}, []
    with ThreadPoolExecutor(max_workers=8) as pool:
        parsed_all = list(pool.map(lambda f: _try(_fetch_feed, str(f.get("url") or "")), feeds))
    for feed, (parsed, error) in zip(feeds, parsed_all):
        url = str(feed.get("url") or "")
        name = str(feed.get("name") or url)
        if url in seen_by:
            kept[url] = seen_by[url]
        if error is not None:
            warnings.append(f"follow: {name}: not fetched ({error})")
            continue
        if parsed.get("bozo") and not parsed.entries:
            warnings.append(f"follow: {name}: not a readable feed ({parsed.get('bozo_exception')})")
            continue
        entries = [e for e in parsed.entries if _entry_id(e)]
        seen = set(seen_by.get(url, []))
        new = [e for e in entries if _entry_id(e) not in seen]
        new.sort(key=lambda e: _entry_time(e) or time.gmtime(0), reverse=True)
        own = _topics_of(feed)
        rank = 0
        for e in new:
            if rank == per_feed:
                break
            stamp = _entry_time(e)
            item = {
                "title": " ".join(str(e.get("title") or e.get("link") or "").split()),
                "url": str(e.get("link") or ""),
                "source": name,
                "date": time.strftime("%Y-%m-%d", stamp) if stamp else None,
                "summary": _summary(e),
            }
            found = tag(f"{item['title']} {item['summary']}", _lang("", item["title"]), [item["url"]])
            if feed.get("only_topic") and not set(found) & set(own):
                continue  # a mixed feed: only entries whose words name the feed's topic
            item["topics"] = list(dict.fromkeys(own + found))
            ranked.append((rank, stamp or time.gmtime(0), {k: v for k, v in item.items() if v}))
            rank += 1
        kept[url] = sorted({_entry_id(e) for e in entries})  # beyond the limits: skipped for good
    state["follow_seen"] = kept
    ranked.sort(key=lambda r: (r[0], tuple(-x for x in r[1][:6])))
    out, counts = [], {}
    for _, _, item in ranked:
        topics = item.get("topics", [])
        if topics and all(counts.get(t, 0) >= per_topic for t in topics):
            continue
        for t in topics:
            counts[t] = counts.get(t, 0) + 1
        out.append(item)
    return out, warnings


def _try(fn, *args):
    try:
        return fn(*args), None
    except Exception as e:  # noqa: BLE001 - a failing feed is skipped, retried next time
        return None, e


# inbox and interests


def _link(line: str) -> str | None:
    """The URL on an inbox line, or None for blank lines and # lines."""
    s = line.strip()
    if not s or s.startswith("#"):
        return None
    s = re.sub(r"^([-*+]|\d+\.)\s+", "", s).strip("<>")
    return s.split()[0] if s else None


def _same(a: str, b: str) -> bool:
    return a.rstrip("/") == b.rstrip("/")


def inbox(h: Home) -> tuple[list[str], list[str]]:
    if not h.inbox.exists():
        return [], []
    links, warnings = [], []
    for n, line in enumerate(h.inbox.read_text(encoding="utf-8").splitlines(), 1):
        link = _link(line)
        if link is None:
            continue
        if link.startswith(("http://", "https://")):
            links.append(link)
        else:
            warnings.append(f"inbox: {h.inbox}:{n}: '{line.strip()[:60]}' is not a link; write one http(s) link per line")
    return links, warnings


def remove_from_inbox(h: Home, urls: list[str]) -> list[str]:
    """Drop the lines holding these links from inbox.md; returns the links removed."""
    if not urls or not h.inbox.exists():
        return []
    removed, keep = [], []
    for line in h.inbox.read_text(encoding="utf-8").splitlines(keepends=True):
        link = _link(line)
        if link and any(_same(link, u) for u in urls):
            removed.append(link)
        else:
            keep.append(line)
    if removed:
        tmp = h.inbox.with_name(h.inbox.name + ".tmp")
        tmp.write_text("".join(keep), encoding="utf-8")
        tmp.replace(h.inbox)
    return removed


def interests(h: Home) -> str | None:
    if not h.interests.exists():
        return None
    return h.interests.read_text(encoding="utf-8").strip() or None


def feed(h: Home, topic: str | None = None) -> dict:
    """Every candidate since the last run: news, follow, inbox, interests, warnings.

    With `topic`, only that topic's stories and entries, and the run is not remembered. Once
    today's issue went out nothing is used up either: the stories are held for the next issue."""
    state = read_json(h.feed_state, {})
    since = state.get("last")
    today = date.today()
    engine = settings.news_engine(h)
    raw, w_news = news(state, today, engine)
    entries, w_follow = follow(h, state)
    shown = raw[:MAX_NEWS]
    stories = [_story(s) for s in shown]
    if topic:
        stories = [s for s in stories if topic in s["topics"]]
        entries = [e for e in entries if topic in e.get("topics", [])]
    w_primary = primary.attach(stories, engine, today, lambda *a: run_node(*a)) if stories else []
    links, w_inbox = inbox(h)
    if not topic and _issue_out(h):
        kept = read_json(h.feed_state, {})
        kept["news_held"] = raw
        write_json(pending(h.feed_state), kept)
    elif not topic:
        cutoff = (today - timedelta(days=KEEP_DAYS)).isoformat()
        seen = {u: d for u, d in state.get("news_seen", {}).items() if d >= cutoff}
        seen.update({x["url"]: s["day"] for s in shown for x in s["sources"]})
        state["news_seen"] = seen
        state["news_held"] = raw[MAX_NEWS:]
        state["last"] = datetime.now().astimezone().isoformat(timespec="seconds")
        write_json(pending(h.feed_state), state)
    return {
        "since": since,
        "news": stories,
        "follow": entries,
        "inbox": links,
        "interests": interests(h),
        "warnings": w_news + w_primary + w_follow + w_inbox,
    }
