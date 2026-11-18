import io
import json
import logging
import os
import zipfile
from pathlib import Path

import pytest
from PIL import Image

from x4.convert import net, rss
from x4.convert.article import _author
from x4.convert.books import prepare_book
from x4.convert.rss import sync_feeds

FIXTURES = Path(__file__).parent / "fixtures" / "convert"
FEED_URL = "https://slow.example/feed.xml"


def png() -> bytes:
    buf = io.BytesIO()
    Image.linear_gradient("L").resize((900, 900)).convert("RGB").save(buf, "PNG")
    return buf.getvalue()


@pytest.fixture
def web(monkeypatch: pytest.MonkeyPatch) -> dict[str, bytes]:
    """An offline web: URL -> bytes. Anything else fails like an unreachable host."""
    pages = {
        FEED_URL: (FIXTURES / "feed.xml").read_bytes(),
        "https://slow.example/stoic-week": (FIXTURES / "article.html").read_bytes(),
        "https://slow.example/img/marcus.png": png(),
    }

    def fetch(url: str) -> bytes:
        if url not in pages:
            raise OSError(f"unreachable: {url}")
        return pages[url]

    monkeypatch.setattr(net, "fetch", fetch)
    return pages


def epub_text(path: Path) -> str:
    with zipfile.ZipFile(path) as zf:
        return "".join(zf.read(n).decode() for n in zf.namelist() if n.endswith(".xhtml"))


def test_sync_feeds(tmp_path: Path, web: dict[str, bytes], caplog: pytest.LogCaptureFixture) -> None:
    feeds = tmp_path / "feeds.yaml"
    feeds.write_text(
        f"max_per_feed: 2\nfeeds:\n  - url: {FEED_URL}\n    name: Slow Reading\n"
        "  - url: https://down.example/feed.xml\n    name: Down\n"
    )
    state = tmp_path / "state.json"
    with caplog.at_level(logging.WARNING):
        uploads = sync_feeds(feeds, state, tmp_path / "work")
    assert "Down: feed not fetched" in caplog.text

    assert [u.remote for u in uploads] == [
        "/books/articles/2026-11-02-the-stoic-week.epub",
        "/books/articles/2026-11-01-notes-on-attention.epub",
    ]
    full = epub_text(uploads[0].local)
    assert "dichotomy of control" in full  # the full article, not the feed teaser
    assert "Subscribe" not in full and "Copyright" not in full and "trackPageView" not in full
    assert "Jane Doe" in full
    with zipfile.ZipFile(uploads[0].local) as zf:
        assert any(n.startswith("OEBPS/images/") for n in zf.namelist())
    fallback = epub_text(uploads[1].local)  # page unreachable: the feed's own content
    assert "rarest and purest form of generosity" in fallback

    seen = json.loads(state.read_text())["seen"][FEED_URL]
    assert len(seen) == 3  # the third entry is beyond max_per_feed on a first sync: skipped for good
    assert sync_feeds(feeds, state, tmp_path / "work") == []


def test_failed_entry_is_retried(tmp_path: Path, web: dict[str, bytes]) -> None:
    feeds = tmp_path / "feeds.yaml"
    feeds.write_text(f"feeds:\n  - url: {FEED_URL}\n")
    state = tmp_path / "state.json"

    def broken(*args: object) -> object:
        raise RuntimeError("disk full")

    with pytest.MonkeyPatch.context() as mp:
        mp.setattr(rss, "write_article", broken)
        assert sync_feeds(feeds, state, tmp_path / "work") == []
    assert json.loads(state.read_text())["seen"][FEED_URL] == []
    assert len(sync_feeds(feeds, state, tmp_path / "work")) == 3


def test_prepare_book_url(tmp_path: Path, web: dict[str, bytes]) -> None:
    uploads = prepare_book("https://slow.example/stoic-week", tmp_path / "work")
    assert [u.remote for u in uploads] == ["/books/articles/2026-11-02-the-stoic-week.epub"]
    text = epub_text(uploads[0].local)
    assert text.count("The Stoic Week</h1>") == 1
    again = prepare_book("https://slow.example/stoic-week", tmp_path / "again")
    assert again[0].local.read_bytes() == uploads[0].local.read_bytes()


@pytest.mark.skipif(not os.environ.get("X4_NETWORK_TESTS"), reason="set X4_NETWORK_TESTS=1 to fetch the web")
def test_live_article(tmp_path: Path) -> None:
    uploads = prepare_book("https://en.wikipedia.org/wiki/Stoicism", tmp_path)
    assert uploads[0].remote.startswith("/books/articles/") and uploads[0].local.stat().st_size > 10_000


def test_author_keeps_real_names_over_url_slugs():
    assert _author("Szerzo; Kovacs-Pal; Kovács Pál") == "Kovács Pál"
    assert _author("Ann Lee; Ann Lee") == "Ann Lee"
    assert _author("Reuters") == "Reuters"


def test_rss_sync_marks_entries_seen_only_after_upload(dev, h, web, monkeypatch) -> None:
    from x4 import ops
    from x4.errors import DeviceError

    h.feeds.parent.mkdir(parents=True, exist_ok=True)
    h.feeds.write_text(f"feeds:\n  - url: {FEED_URL}\n")
    real = ops.upload_all

    def cut(dev, uploads):
        raise DeviceError("io", "unplugged")

    monkeypatch.setattr(ops, "upload_all", cut)
    with pytest.raises(DeviceError):
        ops.rss_sync(dev, h)
    assert not h.rss_state.exists()
    monkeypatch.setattr(ops, "upload_all", real)
    assert ops.rss_sync(dev, h)["uploaded"]
    assert json.loads(h.rss_state.read_text())["seen"][FEED_URL]
    assert ops.rss_sync(dev, h)["uploaded"] == []
