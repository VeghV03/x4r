import io
import json
import zipfile
from pathlib import Path

import pytest
from PIL import Image
from records import TODAY as T, write_jsonl

from x4 import links, ops
from x4.context import context
from x4.convert import net

BITE = {"id": "s1", "kind": "bite", "title": "T", "body": "B"}
from x4.errors import InvalidData
from x4.home import read_json

FIXTURES = Path(__file__).parent / "fixtures" / "convert"
ARTICLE = "https://slow.example/stoic-week"
HU_ARTICLE = "https://hirek.example/cikk"
HU_PAGE = (
    "<html><head><title>Új szabály</title></head><body><article><h1>Új szabály</h1>"
    + "<p>A kormány szerdán hirdette ki az új rendeletet, amely jövő hónaptól érvényes. "
    "Az önkormányzatok szerint a határidő túl rövid, ezért több polgármester is módosítást kér. "
    "A rendelet szövege a közlönyben jelent meg, és a minisztérium szerint nem lesz halasztás.</p>" * 3
    + "</article></body></html>"
).encode()


def png() -> bytes:
    buf = io.BytesIO()
    Image.linear_gradient("L").resize((300, 300)).convert("RGB").save(buf, "PNG")
    return buf.getvalue()


@pytest.fixture
def web(monkeypatch):
    pages = {
        ARTICLE: (FIXTURES / "article.html").read_bytes(),
        "https://slow.example/img/marcus.png": png(),
        HU_ARTICLE: HU_PAGE,
    }
    fetched: list[str] = []

    def fetch(url: str) -> bytes:
        fetched.append(url)
        if url not in pages:
            raise OSError(f"unreachable: {url}")
        return pages[url]

    monkeypatch.setattr(net, "fetch", fetch)
    return fetched


def device_list(sim) -> list[dict]:
    return [json.loads(line) for line in (sim.root / "x4/read.jsonl").read_text().splitlines()]


def test_sync_uploads_the_read_list(dev, sim, h, web):
    write_jsonl(
        h.read,
        [
            {"id": "r-1", "kind": "news", "url": HU_ARTICLE, "title": "Rendelet: rövid a határidő", "source": "Telex"},
            {"id": "r-2", "kind": "link", "url": ARTICLE},
            {"id": "r-3", "kind": "found", "url": "https://gone.example/essay"},
        ],
    )
    write_jsonl(h.stream, [BITE])
    h.inbox.write_text("# saved\nhttps://slow.example/stoic-week/\nhttps://other.example/\n")
    web.clear()
    out = ops.sync(dev, h)

    assert out["warnings"] == [
        f"{h.read}: r-3: https://gone.example/essay not fetched (unreachable: https://gone.example/essay); left out",
    ]
    assert out["pushed"] == ["/books/read/r-1.epub", "/books/read/r-2.epub", "/x4/read.jsonl", "/x4/stream.jsonl"]
    assert device_list(sim) == [
        {"id": "r-1", "kind": "news", "title": "Rendelet: rövid a határidő", "source": "Telex", "lang": "hu", "minutes": 1, "book": "books/read/r-1.epub"},
        {"id": "r-2", "kind": "link", "title": "The Stoic Week", "source": "slow.example", "lang": "en", "minutes": 1, "book": "books/read/r-2.epub"},
    ]  # fmt: skip
    with zipfile.ZipFile(sim.root / "books/read/r-2.epub") as zf:
        assert any("Stoic" in zf.read(n).decode() for n in zf.namelist() if n.endswith(".xhtml"))
    sent = [json.loads(line) for line in (sim.root / "x4/stream.jsonl").read_text().splitlines()]
    assert sent == [BITE]
    assert out["inbox_removed"] == ["https://slow.example/stoic-week/"]
    assert h.inbox.read_text() == "# saved\nhttps://other.example/\n"
    assert read_json(h.sync_state, {})["read"]["r-2"] == {"kind": "link", "title": "The Stoic Week", "source": "slow.example"}
    c = context(h)
    assert c["stream"]["synced"] is True and c["read"]["waiting"] == ["r-1", "r-2"]

    web.clear()
    again = ops.sync(dev, h)
    assert again["pushed"] == [] and again["unchanged"] == 4
    assert web == ["https://gone.example/essay"]  # cached by URL; only the failure is retried


def test_sync_removes_articles_no_longer_listed(dev, sim, h, web):
    write_jsonl(h.read, [{"id": "r-1", "kind": "found", "url": ARTICLE}, {"id": "r-2", "kind": "news", "url": HU_ARTICLE}])
    ops.sync(dev, h)
    (sim.root / "books/read/foreign.txt").write_text("not an article")
    write_jsonl(h.read, [{"id": "r-2", "kind": "news", "url": HU_ARTICLE}])
    out = ops.sync(dev, h)
    assert out["deleted"] == ["/books/read/r-1.epub"]
    assert out["pushed"] == ["/x4/read.jsonl"]
    assert sorted(p.name for p in (sim.root / "books/read").iterdir()) == ["foreign.txt", "r-2.epub"]
    assert [r["id"] for r in device_list(sim)] == ["r-2"]
    assert "/books/read/r-1.epub" not in read_json(h.sync_state, {})["pushed"]

    h.read.write_text("")  # an empty list clears the device
    out = ops.sync(dev, h)
    assert out["deleted"] == ["/books/read/r-2.epub"] and (sim.root / "x4/read.jsonl").read_text() == ""


def test_sync_reads_local_files(dev, sim, h, web, tmp_path):
    page = tmp_path / "essay.html"
    page.write_bytes((FIXTURES / "article.html").read_bytes())
    write_jsonl(h.read, [{"id": "r-f", "kind": "found", "file": str(page), "minutes": 12}])
    ops.sync(dev, h)
    assert device_list(sim) == [
        {"id": "r-f", "kind": "found", "title": "The Stoic Week", "source": "essay.html", "lang": "en", "minutes": 12, "book": "books/read/r-f.epub"}
    ]  # fmt: skip
    first = (sim.root / "books/read/r-f.epub").read_bytes()
    page.write_bytes(page.read_bytes().replace(b"Every autumn", b"Every spring"))
    assert ops.sync(dev, h)["pushed"] == ["/books/read/r-f.epub"]  # the file changed: converted again
    assert (sim.root / "books/read/r-f.epub").read_bytes() != first


STORY = """---
title: A tűzifa-rendelet
source: LátóTér
lang: hu
author: Claude
date: 2026-11-03
translated_from: {url: "https://www.thepaper.cn/newsDetail_forward_1", lang: zh, source: 澎湃新闻}
sources:
  - {title: 345/2026. (X. 30.) Korm. rendelet, url: "https://magyarkozlony.hu/dokumentumok/abc/megtekintes"}
  - https://telex.hu/belfold/tuzifa
---
# A tűzifa-rendelet

A kormány **szerdán** döntött [1], a *Telex* szerint [2] későn.

## Mit ír a rendelet

> A támogatás a rászoruló háztartásoknak jár.

1. első pont
2. második pont

![Térkép](map.png)

<script>alert(1)</script>
"""


def epub_text(path: Path) -> str:
    with zipfile.ZipFile(path) as zf:
        return "".join(zf.read(n).decode() for n in zf.namelist() if n.endswith((".xhtml", ".opf")))


def test_sync_converts_markdown_as_written(dev, sim, h, web, tmp_path, monkeypatch):
    import trafilatura

    monkeypatch.setattr(trafilatura, "extract", lambda *a, **k: pytest.fail("Markdown is not a web page"))
    story = tmp_path / "story.md"
    story.write_text(STORY)
    (tmp_path / "map.png").write_bytes(png())
    write_jsonl(h.read, [{"id": "r-s", "kind": "found", "file": str(story)}])
    out = ops.sync(dev, h)
    assert out["warnings"] == []
    assert device_list(sim) == [
        {"id": "r-s", "kind": "found", "title": "A tűzifa-rendelet", "source": "LátóTér", "lang": "hu", "minutes": 1, "book": "books/read/r-s.epub"}
    ]  # fmt: skip
    text = epub_text(sim.root / "books/read/r-s.epub")
    assert "<strong>szerdán</strong> döntött [1], a <em>Telex</em> szerint [2]" in text
    assert "<h2>Mit ír a rendelet</h2>" in text and "<blockquote>" in text and "<ol>" in text
    assert text.count("<h1>A tűzifa-rendelet</h1>") == 1  # the title heading is not repeated
    assert '<img src="images/image-001.' in text
    assert "<p>Eredeti: 澎湃新闻 · thepaper.cn</p>" in text
    assert "<h2>Források</h2><ol><li>345/2026. (X. 30.) Korm. rendelet · magyarkozlony.hu</li><li>telex.hu</li></ol>" in text
    assert "alert(1)" not in text.replace("&lt;script&gt;alert(1)&lt;/script&gt;", "") and "<script" not in text
    assert "Claude · LátóTér · 2026-11-03" in text
    assert "https://www.thepaper.cn/newsDetail_forward_1" in text  # the EPUB's source


@pytest.mark.parametrize(
    ("body", "error"),
    [
        ("# no front matter\n", "story.md: no front matter; start the file with ---, title: …, ---"),
        ("---\nsource: x\n---\ntext\n", "story.md: title: missing; add title: … to the front matter"),
        ("---\ntitle: t\nsubtitle: s\n---\n", "story.md: unknown front matter subtitle; use author, date, lang, source, sources, title, translated_from"),
        ("---\ntitle: t\nsources: [{title: a, url: ftp://x}]\n---\n", "story.md: sources.1: url 'ftp://x' must start with http:// or https://"),
    ],
)
def test_markdown_errors_name_the_fix(dev, sim, h, web, tmp_path, body, error):
    story = tmp_path / "story.md"
    story.write_text(body)
    write_jsonl(h.read, [{"id": "r-s", "kind": "translation", "file": str(story)}])
    out = ops.sync(dev, h)
    assert out["warnings"] == [f"{h.read}: r-s: {story} not read ({error}); left out"]


def test_invalid_read_list_pushes_nothing(dev, sim, h, web):
    write_jsonl(h.read, [{"id": "r-1", "kind": "news"}])
    write_jsonl(h.stream, [{"id": "s1", "kind": "letter", "title": "Hét", "body": "Kedves olvasó"}])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert e.value.details[0] == f"{h.read}:1: url: give either url (http or https) or file (a local path), not both"
    assert e.value.details[1].startswith(f"{h.stream}:1: kind: must be one of 'bite'")
    assert not (sim.root / "x4/read.jsonl").exists() and web == []


def test_no_read_list_leaves_the_device_alone(dev, sim, h):
    (sim.root / "books/read").mkdir(parents=True)
    (sim.root / "books/read/r-old.epub").write_bytes(b"epub")
    out = ops.sync(dev, h)
    assert out["deleted"] == [] and (sim.root / "books/read/r-old.epub").exists()


def asking(rid: str = "r-s", **predict) -> dict:
    """A piece's question as the issue's check sees it (ops._piece_errors)."""
    question = {"prompt": "Mikor döntött a kormány?", "options": ["Szerdán", "Pénteken"], "answer": 0} | predict
    return {"id": rid, "kind": "article", "book": f"books/read/{rid}.epub", "predict": question}


def test_a_predict_quotes_its_article(h, tmp_path):
    story = tmp_path / "story.md"
    story.write_text(STORY)
    (tmp_path / "map.png").write_bytes(png())
    write_jsonl(h.read, [{"id": "r-s", "kind": "found", "file": str(story)}])

    # As the article has it, whatever the spacing, the case and the quote marks.
    assert ops._predict_errors(h, "read.jsonl", [asking(quote="a kormány   SZERDÁN döntött")]) == []
    assert ops._predict_errors(h, "read.jsonl", [asking(quote="A kormány pénteken döntött")]) == [
        "read.jsonl: r-s: predict.quote: not in the article; copy the sentence that settles it from `x4 article r-s`"
    ]
    [error] = ops._predict_errors(h, "read.jsonl", [asking("r-gone", quote="Q")])
    assert "r-gone is not in" in error


def test_x4_article_prints_the_text_a_quote_is_taken_from(x4, h, tmp_path):
    story = tmp_path / "story.md"
    story.write_text(STORY)
    (tmp_path / "map.png").write_bytes(png())
    write_jsonl(h.read, [{"id": "r-s", "kind": "found", "file": str(story)}])
    out = json.loads(x4("--json", "article", "r-s").output)
    assert out["title"] == "A tűzifa-rendelet" and "A kormány szerdán döntött" in out["text"]
    assert x4("article", "r-none").exit_code == 2


def test_context_tells_how_the_guesses_went(h):
    write_jsonl(
        h.archive("seen"),
        [
            {"id": "s-a", "opened": True, "predict": {"pick": 1, "correct": False, "ms": 4000}, "day": T, "at": None},
            {"id": "s-b", "opened": False, "predict": {"pick": 0, "correct": True, "ms": 2000}, "day": T, "at": None},
            {"id": "s-c", "opened": True, "day": T, "at": None},
        ],
    )
    h.sync_state.write_text(json.dumps({"predicts": {"s-a": {"title": "A", "prompt": "P?"}}}))
    assert context(h)["predict"] == {
        "asked": 2,
        "right": 1,
        "read": 1,
        "results": [
            {"id": "s-a", "title": "A", "prompt": "P?", "pick": 1, "correct": False, "read": True},
            {"id": "s-b", "pick": 0, "correct": True, "read": False},
        ],
    }


def test_a_link_names_only_what_was_read_on_an_earlier_day(h):
    h.stats.write_text(json.dumps({"day": T, "books": {"books/meditations.epub": {"read_ms": 60000}, "books/unread.epub": {"read_ms": 0}}}))
    write_jsonl(
        h.archive("highlights"),
        [
            {"id": "h-old", "book": "books/meditations.epub", "title": "Meditations", "text": "You have power over your mind.", "day": T - 3},
            {"id": "h-now", "book": "books/meditations.epub", "title": "Meditations", "text": "Made today.", "day": T},
            {"id": "h-gone", "book": "books/meditations.epub", "text": "Deleted.", "day": T - 4},
            {"id": "h-gone", "deleted": True, "day": T - 3},
        ],
    )
    write_jsonl(
        h.archive("read-log"),
        [
            {"book": "books/read/r-done.epub", "progress": 1.0, "finished": True, "day": T - 2},
            {"book": "books/read/r-left.epub", "progress": 0.3, "finished": False, "day": T - 2},
        ],
    )
    h.sync_state.write_text(json.dumps({"read": {"r-done": {"kind": "found", "title": "The Stoic Week", "source": "slow.example"}, "r-left": {"kind": "found", "title": "Left", "source": "x"}}}))

    listed = context(h)["linkable"]
    assert listed == {
        "highlights": [{"ref": "h-old", "title": "Meditations", "text": "You have power over your mind.", "day": T - 3}],
        "articles": [{"ref": "r-done", "title": "The Stoic Week", "day": T - 2}],
        "books": [{"ref": "books/meditations.epub", "title": "Meditations"}],
    }

    def errors(*refs: str) -> list[str]:
        options = [{"text": f"Thing {i}", "ref": ref} for i, ref in enumerate(refs)]
        link = {"prompt": "Melyik mond ennek ellent?", "options": options, "answer": 0, "why": "Mert."}
        return links.errors(h, "read.jsonl", [{"id": "r-new", "kind": "article", "book": "books/read/r-new.epub", "link": link}])

    assert errors("h-old", "r-done", "books/meditations.epub") == []
    for bad, said in (("h-now", "no record that 'h-now' was read on an earlier day"), ("r-left", "no record"), ("h-gone", "no record"), ("r-new", "is this article")):
        [error] = errors("h-old", "r-done", bad)
        assert said in error and "r-new: link.options.2.ref" in error
    assert "two options name the same thing" in errors("h-old", "h-old", "r-done")[0]


def test_context_tells_how_the_links_went(h):
    write_jsonl(h.archive("seen"), [{"id": "s-a", "opened": True, "link": {"pick": 2, "correct": True, "ms": 6000}, "day": T, "at": None}])
    h.sync_state.write_text(json.dumps({"links": {"s-a": {"title": "A", "prompt": "P?"}}}))
    assert context(h)["link"] == {"asked": 1, "right": 1, "results": [{"id": "s-a", "title": "A", "prompt": "P?", "pick": 2, "correct": True}]}


def test_threads_are_checked_and_travel_with_the_issue(dev, sim, h):
    thread = {"id": "t-budget", "title": "A költségvetés", "body": "Eddig ennyi történt.", "since": "2026-10-28", "updated": "2026-11-10"}
    write_jsonl(h.threads, [thread])
    assert ops.sync(dev, h)["warnings"] == []
    assert [json.loads(line) for line in (sim.root / "x4/threads.jsonl").read_text().splitlines()] == [thread]

    for bad, said in (
        ({"body": "x" * 601}, "one screen holds 600"),
        ({"body": "Two\nparagraphs"}, "one paragraph"),
        ({"title": ""}, "title"),
    ):
        write_jsonl(h.threads, [thread | bad])
        with pytest.raises(InvalidData) as e:
            ops.sync(dev, h)
        assert any(said in d for d in e.value.details), e.value.details
    write_jsonl(h.threads, [thread, thread])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert any("already used" in d for d in e.value.details)


def test_the_field_of_the_week_is_a_read_kind(h):
    from x4.models import check_read

    write_jsonl(h.read, [{"id": "r-f1", "kind": "field", "url": "https://example.org/primer"}])
    assert [i.kind for i in check_read(h.read)] == ["field"]
    write_jsonl(h.read, [{"id": "r-f1", "kind": "fields", "url": "https://example.org/primer"}])
    with pytest.raises(InvalidData):
        check_read(h.read)
