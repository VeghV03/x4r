"""Web articles to small EPUBs: clean text, title, author, date and optimized images."""

import logging
from dataclasses import dataclass
from html import escape
from pathlib import Path
from urllib.parse import urljoin, urlsplit
from urllib.request import url2pathname

import lxml.html
import trafilatura
from lxml import etree

from . import Upload, epub, net
from .images import optimize_image
from .names import slugify

log = logging.getLogger(__name__)

ARTICLES = "/books/articles"
_KEEP = {
    "p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "blockquote", "pre", "code", "i", "em", "b",
    "strong", "br", "img", "figure", "figcaption", "table", "thead", "tbody", "tr", "td", "th", "sub", "sup", "hr",
}  # fmt: skip
_DROP = {"script", "style", "iframe", "video", "audio", "form", "button", "svg", "noscript", "head"}


@dataclass
class Article:
    title: str
    url: str
    body: str  # untrusted HTML fragment
    author: str = ""
    date: str = ""  # YYYY-MM-DD
    site: str = ""
    lang: str = ""  # guessed from the text when empty
    source_url: str = ""  # the EPUB's source when the body comes from a local file


def _author(raw: str) -> str:
    """Real names only: trafilatura also reads author URL slugs ("/szerzo/kovacs-pal") as names."""
    parts = list(dict.fromkeys(p.strip() for p in raw.split(";") if p.strip()))
    names = [p for p in parts if " " in p]
    return "; ".join(names or parts)


def extract(html: str | bytes, url: str) -> Article | None:
    """Main text and metadata of a web page, or None if trafilatura finds no article."""
    body = trafilatura.extract(
        html, url=url, output_format="html", include_images=True, include_formatting=True, include_links=False
    )
    if not body:
        return None
    meta = trafilatura.extract_metadata(html, default_url=url)
    return Article(
        title=(meta.title if meta and meta.title else "") or url,
        url=url,
        body=body,
        author=_author(meta.author or "") if meta else "",
        date=(meta.date or "")[:10] if meta else "",
        site=(meta.sitename or "") if meta else "",
    )


def _clean(body: str, base_url: str, title: str, images: dict[str, tuple[bytes, str]]) -> str:
    """Whitelist tags, drop attributes, embed images; returns well-formed XHTML."""
    root = lxml.html.document_fromstring(body or "<p></p>").body
    for el in list(root.iter()):
        if el is root or not isinstance(el.tag, str):
            continue
        tag = el.tag.lower()
        if tag in _DROP:
            el.drop_tree()
        elif tag not in _KEEP:
            el.drop_tag()
    for el in list(root.iter()):
        if el is root or not isinstance(el.tag, str):
            continue
        src = el.get("src", "") if el.tag == "img" else ""
        alt = el.get("alt", "")
        el.attrib.clear()
        if el.tag == "img":
            name = _embed(urljoin(base_url, src), images, base_url.startswith("file:")) if src else None
            if name is None:
                el.drop_tree()
                continue
            el.set("src", f"images/{name}")
            el.set("alt", alt)
    # The EPUB adds its own <h1> title; an identical first heading would repeat it.
    first = next((el for el in root if isinstance(el.tag, str)), None)
    if first is not None and first.tag in ("h1", "h2") and first.text_content().strip() == title.strip():
        first.drop_tree()
    for el in root.iter("h1"):
        el.tag = "h2"
    parts = [escape(root.text or "")] + [etree.tostring(el, method="xml", encoding="unicode") for el in root]
    return "".join(parts).strip()


def _embed(url: str, images: dict[str, tuple[bytes, str]], local: bool = False) -> str | None:
    """Fetch, dither and add one image. `local`: the page is a local file, so file: images count too."""
    if not url.startswith(("http://", "https://")) and not (local and url.startswith("file:")):
        return None
    try:
        data = Path(url2pathname(urlsplit(url).path)).read_bytes() if url.startswith("file:") else net.fetch(url)
        result = optimize_image(data)
    except Exception as exc:  # noqa: BLE001 - one missing image never fails an article
        log.warning("image %s skipped: %s", url, exc)
        return None
    if result is None:
        return None
    for name, (data, _) in images.items():
        if data == result.data:
            return name
    name = f"image-{len(images) + 1:03d}.{result.ext}"
    images[name] = (result.data, result.media_type)
    return name


def remote_path(article: Article) -> str:
    prefix = f"{article.date}-" if article.date else ""
    return f"{ARTICLES}/{prefix}{slugify(article.title)}.epub"


def write_article(article: Article, workdir: Path) -> Upload:
    images: dict[str, tuple[bytes, str]] = {}
    body = _clean(article.body, article.url, article.title, images)
    byline = " · ".join(x for x in (article.author, article.site, article.date) if x)
    head = f"<h1>{escape(article.title)}</h1>\n" + (f"<p><i>{escape(byline)}</i></p>\n" if byline else "")
    book = epub.Book(
        title=article.title,
        chapters=[epub.Chapter(article.title, head + body)],
        author=article.author,
        language=article.lang or epub.guess_language(body),
        date=article.date,
        source=article.source_url or article.url,
        images=images,
    )
    remote = remote_path(article)
    out_dir = workdir / "articles"
    out_dir.mkdir(parents=True, exist_ok=True)
    local = out_dir / remote.rsplit("/", 1)[1]
    epub.write_epub(book, local)
    return Upload(local, remote)


def prepare_article(url: str, workdir: Path) -> Upload:
    article = extract(net.fetch(url), url)
    if article is None:
        raise ValueError(f"no article text found at {url}")
    return write_article(article, workdir)
