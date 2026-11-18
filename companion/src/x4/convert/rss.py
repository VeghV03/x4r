"""`x4 rss sync`: new feed entries become one article EPUB each."""

import json
import logging
import time
from pathlib import Path

import feedparser
import yaml

from . import Upload, net
from .article import Article, extract, write_article

log = logging.getLogger(__name__)

MAX_PER_FEED = 5


def _entry_id(entry: feedparser.FeedParserDict) -> str:
    return str(entry.get("id") or entry.get("link") or entry.get("title") or "")


def _entry_time(entry: feedparser.FeedParserDict) -> time.struct_time | None:
    return entry.get("published_parsed") or entry.get("updated_parsed")


def _entry_article(entry: feedparser.FeedParserDict, feed_name: str) -> Article | None:
    """Full article from the entry's page; the feed's own content when the page fails."""
    link = entry.get("link", "")
    stamp = _entry_time(entry)
    date = time.strftime("%Y-%m-%d", stamp) if stamp else ""
    article = None
    if link:
        try:
            article = extract(net.fetch(link), link)
        except Exception as exc:  # noqa: BLE001 - fall back to the feed text
            log.warning("%s: %s not fetched (%s), using the feed text", feed_name, link, exc)
    if article is None:
        content = entry.get("content") or []
        body = content[0].get("value", "") if content else entry.get("summary", "")
        if not body.strip():
            return None
        article = Article(title="", url=link, body=body)
    article.title = entry.get("title") or article.title or link
    article.author = entry.get("author") or article.author
    article.date = date or article.date
    article.site = feed_name
    return article


def sync_feeds(feeds_file: Path, state_file: Path, workdir: Path, save_to: Path | None = None) -> list[Upload]:
    """Fetch every feed, convert entries not seen before (newest first, `max_per_feed` each).

    A failing feed or entry is logged and skipped; it is retried on the next sync. On a
    feed's first sync only the newest `max_per_feed` entries are taken, older ones are marked seen.
    The new state goes to `save_to` (default `state_file`), for the caller to commit once the
    uploads are on the device.
    """
    config = yaml.safe_load(feeds_file.read_text(encoding="utf-8")) or {}
    limit = int(config.get("max_per_feed", MAX_PER_FEED))
    state = json.loads(state_file.read_text(encoding="utf-8")) if state_file.exists() else {}
    seen_by_feed: dict[str, list[str]] = state.get("seen", {})
    uploads: list[Upload] = []
    for feed in config.get("feeds") or []:
        url = feed["url"]
        name = feed.get("name") or url
        try:
            parsed = feedparser.parse(net.fetch(url))
        except Exception as exc:  # noqa: BLE001 - a failing feed is skipped
            log.warning("%s: feed not fetched (%s)", name, exc)
            continue
        if parsed.get("bozo") and not parsed.entries:
            log.warning("%s: feed not readable (%s)", name, parsed.get("bozo_exception"))
            continue
        entries = [e for e in parsed.entries if _entry_id(e)]
        seen = set(seen_by_feed.get(url, []))
        new = [e for e in entries if _entry_id(e) not in seen]
        new.sort(key=lambda e: _entry_time(e) or time.gmtime(0), reverse=True)
        done = {_entry_id(e) for e in new[limit:]}  # beyond the limit: skipped for good
        for entry in new[:limit]:
            try:
                article = _entry_article(entry, name)
                if article is None:
                    log.warning("%s: %s has no text", name, _entry_id(entry))
                    continue
                uploads.append(write_article(article, workdir))
                done.add(_entry_id(entry))
            except Exception as exc:  # noqa: BLE001 - a failing entry is retried next sync
                log.warning("%s: %s failed (%s)", name, _entry_id(entry), exc)
        current = {_entry_id(e) for e in entries}
        seen_by_feed[url] = sorted((seen | done) & current)  # entries gone from the feed are forgotten
    save_to = save_to or state_file
    save_to.parent.mkdir(parents=True, exist_ok=True)
    tmp = save_to.with_suffix(save_to.suffix + ".tmp")
    tmp.write_text(json.dumps({"seen": seen_by_feed}, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    tmp.replace(save_to)
    return uploads
