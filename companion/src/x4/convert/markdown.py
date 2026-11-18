"""Markdown articles Claude writes (a long story, a translation) to an Article, as written.

Web pages go through trafilatura to find the article; a Markdown file is the article already.

    ---
    title: A kormány tűzifa-rendelete
    source: LátóTér
    lang: hu
    author: Claude
    date: 2026-11-03
    translated_from: {url: https://www.thepaper.cn/..., lang: zh, source: 澎湃新闻}
    sources:
      - {title: 345/2026. (X. 30.) Korm. rendelet, url: https://magyarkozlony.hu/...}
    ---
    Body in Markdown: headings, emphasis, lists, quotes, tables, images, [1] references.

`sources` become a numbered list at the end, so `[1]` in the text points at the first one.
Images take a path next to the file or a URL.
"""

import re
from datetime import date
from html import escape
from pathlib import Path
from urllib.parse import urlsplit

import yaml
from markdown_it import MarkdownIt

from .article import Article

FRONT = re.compile(r"\A﻿?---[ \t]*\r?\n(.*?)\r?\n---[ \t]*(?:\r?\n|\Z)", re.S)
END = {"hu": ("Források", "Eredeti"), "en": ("Sources", "Original")}
FIELDS = {"title", "source", "lang", "author", "date", "translated_from", "sources"}


def _domain(url: str) -> str:
    return (urlsplit(url).hostname or "").removeprefix("www.")


def _text(meta: dict, key: str, where: str) -> str:
    value = meta.get(key)
    if value is None:
        return ""
    if isinstance(value, date):
        return value.isoformat()
    if not isinstance(value, (str, int, float)):
        raise ValueError(f"{where}: {key}: write it as plain text")
    return str(value).strip()


def _url(value, where: str) -> str:
    url = str(value or "").strip()
    if not url.startswith(("http://", "https://")):
        raise ValueError(f"{where}: url '{url}' must start with http:// or https://")
    return url


def _front(text: str, where: str) -> tuple[dict, str]:
    m = FRONT.match(text)
    if not m:
        raise ValueError(f"{where}: no front matter; start the file with ---, title: …, ---")
    try:
        meta = yaml.safe_load(m.group(1)) or {}
    except yaml.YAMLError as e:
        raise ValueError(f"{where}: front matter is not YAML ({e})") from None
    if not isinstance(meta, dict):
        raise ValueError(f"{where}: front matter must be key: value lines")
    unknown = sorted(set(map(str, meta)) - FIELDS)
    if unknown:
        raise ValueError(f"{where}: unknown front matter {', '.join(unknown)}; use {', '.join(sorted(FIELDS))}")
    return meta, text[m.end() :]


def _sources(meta: dict, where: str) -> list[tuple[str, str]]:
    items = meta.get("sources") or []
    if not isinstance(items, list):
        raise ValueError(f"{where}: sources: write a list of {{title, url}}")
    out = []
    for n, s in enumerate(items, 1):
        if isinstance(s, str):
            s = {"url": s}
        if not isinstance(s, dict):
            raise ValueError(f"{where}: sources.{n}: write {{title, url}}")
        out.append((str(s.get("title") or "").strip(), _url(s.get("url"), f"{where}: sources.{n}")))
    return out


def _end(meta: dict, lang: str, where: str) -> str:
    sources_word, original_word = END.get(lang, END["en"])
    parts = []
    original = meta.get("translated_from")
    if original is not None:
        if not isinstance(original, dict):
            raise ValueError(f"{where}: translated_from: write {{url, lang, source}}")
        url = _url(original.get("url"), f"{where}: translated_from")
        name = str(original.get("source") or "").strip()
        parts.append(f"<p>{original_word}: {escape(' · '.join(x for x in (name, _domain(url)) if x))}</p>")
    sources = _sources(meta, where)
    if sources:
        items = "".join(f"<li>{escape(' · '.join(x for x in (t, _domain(u)) if x))}</li>" for t, u in sources)
        parts.append(f"<h2>{sources_word}</h2><ol>{items}</ol>")
    return "<hr/>" + "".join(parts) if parts else ""


def read_markdown(path: Path) -> Article:
    """The Article in a Markdown file with front matter; ValueError names what to fix."""
    where = path.name
    meta, body = _front(path.read_text(encoding="utf-8"), where)
    title = _text(meta, "title", where)
    if not title:
        raise ValueError(f"{where}: title: missing; add title: … to the front matter")
    lang = _text(meta, "lang", where).lower()
    html = MarkdownIt("commonmark", {"html": False}).enable(["table", "strikethrough"]).render(body)
    original = meta.get("translated_from") if isinstance(meta.get("translated_from"), dict) else {}
    sources = _sources(meta, where)
    return Article(
        title=title,
        url=path.resolve().as_uri(),
        body=html + _end(meta, lang, where),
        author=_text(meta, "author", where),
        date=_text(meta, "date", where)[:10],
        site=_text(meta, "source", where),
        lang=lang,
        source_url=str(original.get("url") or "") or (sources[0][1] if sources else ""),
    )
