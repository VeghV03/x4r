import json
import shutil
from types import SimpleNamespace
from datetime import date
from pathlib import Path

import pytest
from click.testing import CliRunner

from x4 import feed as feedmod
from x4 import primary
from x4.cli import main
from x4.convert import net
from x4.feed import feed as run_feed
from x4.feed import remove_from_inbox
from x4.home import commit_pending, pending, read_json


def feed(h, topic=None):
    """`feed`, then the `x4 sync` that commits its cursor."""
    out = run_feed(h, topic)
    commit_pending(h.feed_state)
    return out


def test_news_waits_for_sync(h, engine):
    first = run_feed(h)["news"]
    assert first and run_feed(h)["news"] == first  # no sync yet: the same stories again
    assert pending(h.feed_state).exists()
    commit_pending(h.feed_state)
    assert run_feed(h)["news"] == []

FIXTURES = Path(__file__).parent / "fixtures" / "convert"
FEED_URL = "https://slow.example/feed.xml"
TODAY = date.today().isoformat()


def member(brand: str, title: str, link: str) -> dict:
    return {"brand": brand, "title": title, "link": link}


def discovery(day: str) -> dict:
    """The shape of the engine's out/discovery/<day>.json."""
    vip = "VIP-bankkártyával éttermeztek a hivatal vezetői"
    return {
        "day": day,
        "candidates": [
            {
                "claimKey": "vip+bankkár+hivatal",
                "title": vip,
                "members": [
                    member("24", vip, "https://24.hu/belfold/vip/"),
                    member("telex", vip, "https://telex.hu/belfold/vip?utm_source=rss&amp;utm_medium=rss"),
                    member("telex", vip, "https://telex.hu/belfold/vip?utm_source=rss&amp;utm_medium=rss"),
                    member("444", "49 500 forint közpénz egy limonádéért", "https://444.hu/vip?utm_source=rss_feed&amp;x=1"),
                    member("reuters", "Hungarian officials used VIP cards", "https://reuters.com/vip"),
                ],
                "pont": 70,
                "verdikt": "EPIZÓD",
            },
            {
                "claimKey": "storm+london",
                "title": "Storm hits London",
                "members": [member("bbc", "Storm hits London", "https://bbc.co.uk/storm")],
                "pont": 0,
            },
            {
                "claimKey": "wire",
                "title": "Wire only",
                "members": [member("reuters", "Wire only", "https://reuters.com/w"), member("ap", "Wire only", "https://ap.org/w")],
                "pont": 5,
            },
        ],
    }


@pytest.fixture
def engine(tmp_path, monkeypatch):
    """A fake engine folder; run_node writes what the real scripts would."""
    root = tmp_path / "engine"
    (root / "scripts").mkdir(parents=True)
    (root / "scripts" / "discover.mjs").write_text("// stub")
    snaps = root / "out" / "feed-snapshots"
    snaps.mkdir(parents=True)
    (snaps / "manifest.jsonl").write_text(
        "\n".join(json.dumps({"id": i, "org": o}) for i, o in [("telex", "Telex"), ("24", "24.hu"), ("bbc", "BBC")]) + "\n"
    )
    calls: list[tuple[str, list[str]]] = []
    payload = {"data": discovery}

    def run_node(r: Path, script: str, args: list[str], timeout: int = 0) -> None:
        assert r == root
        calls.append((script, args))
        if script == "snapshot-feeds.mjs":
            (snaps / TODAY).mkdir(exist_ok=True)
        elif script in ("kozlony.mjs", "eurlex.mjs"):
            for name, text in payload.get(script, {}).items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
        else:
            (root / "out" / "discovery").mkdir(parents=True, exist_ok=True)
            (root / "out" / "discovery" / f"{args[0]}.json").write_text(json.dumps(payload["data"](args[0])))

    pages: dict[str, bytes] = {}

    def fetch(url: str) -> bytes:
        if url not in pages:
            raise OSError(f"unreachable: {url}")
        return pages[url]

    monkeypatch.setenv("X4_NEWS_ENGINE", str(root))
    monkeypatch.setattr(feedmod, "run_node", run_node)
    monkeypatch.setattr(feedmod, "check_node", lambda: None)
    monkeypatch.setattr(net, "fetch", fetch)
    monkeypatch.setattr(primary, "_engine_links", lambda root, stories, acts, warnings: set())
    return SimpleNamespace(root=root, calls=calls, payload=payload, pages=pages)


@pytest.fixture
def web(monkeypatch):
    pages = {FEED_URL: (FIXTURES / "feed.xml").read_bytes()}

    def fetch(url: str) -> bytes:
        if url not in pages:
            raise OSError(f"unreachable: {url}")
        return pages[url]

    monkeypatch.setattr(net, "fetch", fetch)
    return pages


def test_news_ranked_by_independent_sources(h, engine):
    out = feed(h)
    assert engine.calls == [("snapshot-feeds.mjs", []), ("discover.mjs", [TODAY]), ("kozlony.mjs", ["--limit=3"])]
    assert out["since"] is None
    assert out["warnings"] == ["primary: 6 of 6 source articles not read; their links are missing"]
    first, *rest = out["news"]
    assert first == {
        "title": "VIP-bankkártyával éttermeztek a hivatal vezetői",
        "k": 3,
        "day": TODAY,
        "sources": [
            {"org": "Telex", "url": "https://telex.hu/belfold/vip", "lang": "hu"},
            {"org": "444", "url": "https://444.hu/vip?x=1", "lang": "hu", "title": "49 500 forint közpénz egy limonádéért"},
            {"org": "reuters", "url": "https://reuters.com/vip", "lang": "en", "title": "Hungarian officials used VIP cards"},
            {"org": "24.hu", "url": "https://24.hu/belfold/vip/", "lang": "hu"},
        ],
        "claim": "vip+bankkár+hivatal",
        "topics": ["hungary"],
    }
    assert [(s["title"], s["k"]) for s in rest] == [("Wire only", 1), ("Storm hits London", 1)]
    assert rest[1]["sources"] == [{"org": "BBC", "url": "https://bbc.co.uk/storm", "lang": "en"}]
    assert [s["topics"] for s in rest] == [[], []]


def test_news_reports_each_url_once(h, engine):
    feed(h)
    assert feed(h)["news"] == []  # same snapshot, nothing new

    def more(day: str) -> dict:
        d = discovery(day)
        d["candidates"][0]["members"].append(member("hvg", "Új részletek a VIP-kártyákról", "https://hvg.hu/vip"))
        return d

    engine.payload["data"] = more
    again = feed(h)
    assert [(s["title"], s["k"], [x["org"] for x in s["sources"]]) for s in again["news"]] == [
        ("Új részletek a VIP-kártyákról", 1, ["hvg"])
    ]
    assert again["since"] is not None


def issue_out(h, day: str = TODAY):
    """Today's issue went out: what `x4 sync` and `x4 stage` leave in .sync.json."""
    h.sync_state.write_text(json.dumps({"issue": {"date": day, "reads": []}}))


def test_after_the_issue_went_out_stories_are_held_for_the_next_one(h, engine):
    issue_out(h)
    afternoon = feed(h)["news"]  # the second pass: it cannot print a piece
    assert afternoon[0]["title"] == "VIP-bankkártyával éttermeztek a hivatal vezetői"
    state = read_json(h.feed_state, {})
    assert "news_seen" not in state and "follow_seen" not in state and len(state["news_held"]) == 3

    # The next morning, in a fresh clone of the engine: the feeds no longer carry the story.
    h.sync_state.unlink()
    engine.payload["data"] = lambda day: {"day": day, "candidates": [{"claimKey": "new", "pont": 5, "members": [
        member("hvg", "Új részletek a VIP-kártyákról", "https://hvg.hu/vip"),
        member("telex", "VIP-bankkártyával éttermeztek a hivatal vezetői", "https://telex.hu/belfold/vip"),
    ]}]}  # fmt: skip
    morning = feed(h)["news"]
    first = morning[0]
    assert first["k"] == 4 and [x["org"] for x in first["sources"]] == ["Telex", "444", "reuters", "hvg", "24.hu"]
    assert len(morning) == 3  # the held story and its new coverage are one story
    state = read_json(h.feed_state, {})
    assert state["news_held"] == [] and "https://444.hu/vip?x=1" in state["news_seen"]
    assert feed(h)["news"] == []


def test_a_held_story_waits_two_days(h, engine):
    old = date.fromordinal(date.today().toordinal() - 3).isoformat()
    story = {"day": old, "claim": "old", "score": 1, "primary": [], "sources": [
        {"org": "Telex", "url": "https://telex.hu/old", "lang": "hu", "title": "Régi hír", "brand": "telex"}]}  # fmt: skip
    h.feed_state.write_text(json.dumps({"news_held": [story, story | {"day": TODAY, "claim": "kept"}]}))
    engine.payload["data"] = lambda day: {"day": day, "candidates": []}
    assert [s["claim"] for s in feed(h)["news"]] == ["kept"]


def test_stories_below_the_cap_come_back(h, engine, monkeypatch):
    monkeypatch.setattr(feedmod, "MAX_NEWS", 1)
    assert [s["title"] for s in feed(h)["news"]] == ["VIP-bankkártyával éttermeztek a hivatal vezetői"]
    assert [s["title"] for s in feed(h)["news"]] == ["Wire only"]
    assert [s["title"] for s in feed(h)["news"]] == ["Storm hits London"]
    assert feed(h)["news"] == []


def test_aligned_and_paywalled_sources_count_but_are_never_the_piece(h, engine):
    def day(d: str) -> dict:
        return {"day": d, "candidates": [{"claimKey": "budget", "pont": 9, "members": [
            member("magyarnemzet", "Elfogadták a költségvetést", "https://magyarnemzet.hu/b"),
            member("ft", "Hungary passes its budget", "https://ft.com/b"),
            member("telex", "Megszavazták a 2027-es költségvetést", "https://telex.hu/b"),
        ]}]}  # fmt: skip

    engine.payload["data"] = day
    (story,) = feed(h)["news"]
    assert story["k"] == 3
    assert story["title"] == "Megszavazták a 2027-es költségvetést"
    assert [(x["org"], x.get("piece"), x.get("paywalled")) for x in story["sources"]] == [
        ("Telex", None, None), ("magyarnemzet", False, None), ("ft", False, True),
    ]  # fmt: skip


def test_news_covers_days_since_last_run(h, engine):
    (engine.root / "out" / "feed-snapshots" / "2000-01-01").mkdir()
    state = {"news_day": (date.today().fromordinal(date.today().toordinal() - 2)).isoformat()}
    h.feed_state.write_text(json.dumps(state))
    earlier = date.fromordinal(date.today().toordinal() - 1).isoformat()
    (engine.root / "out" / "feed-snapshots" / earlier).mkdir()
    feed(h)
    assert [a for s, a in engine.calls if s == "discover.mjs"] == [[earlier], [TODAY]]
    assert read_json(h.feed_state, {})["news_day"] == TODAY


def test_missing_engine_is_a_warning(h, tmp_path, monkeypatch, web):
    monkeypatch.setenv("X4_NEWS_ENGINE", str(tmp_path / "nowhere"))
    out = feed(h)
    assert out["news"] == []
    assert out["warnings"] == [f"news: no news engine at {tmp_path / 'nowhere'}; set news_engine in settings.yaml or X4_NEWS_ENGINE"]


def test_no_engine_set_up_is_quiet(h, monkeypatch, web):
    monkeypatch.delenv("X4_NEWS_ENGINE", raising=False)
    out = feed(h)
    assert out["news"] == [] and not any(w.startswith("news:") for w in out["warnings"])


def test_missing_node_is_a_warning(h, engine, monkeypatch):
    monkeypatch.setattr(feedmod, "check_node", lambda: "news: Node.js not found; install Node 22+ (brew install node)")
    out = feed(h)
    assert out["news"] == [] and out["warnings"] == ["news: Node.js not found; install Node 22+ (brew install node)"]
    assert "news_day" not in read_json(h.feed_state, {})


def test_sources_the_snapshot_could_not_reach_are_a_warning(h, engine, monkeypatch):
    fake = feedmod.run_node

    def run_node(r, script, args, timeout=0):
        fake(r, script, args, timeout)
        if script == "snapshot-feeds.mjs":
            return "  37 él, 2 nem.\n  Nem élő: telex, merce — az URL-jeik feltételezettek voltak.\n"
        return ""

    monkeypatch.setattr(feedmod, "run_node", run_node)
    out = feed(h)
    assert "news: not reached: Telex, merce" in out["warnings"]
    assert out["news"]


def test_every_source_reached_is_no_warning(h, engine):
    out = feed(h)
    assert not [w for w in out["warnings"] if "not reached" in w]


def test_engine_uses_the_proxy_only_where_there_is_one(monkeypatch):
    for k in ("https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "NODE_USE_ENV_PROXY"):
        monkeypatch.delenv(k, raising=False)
    assert "NODE_USE_ENV_PROXY" not in feedmod.engine_env()
    monkeypatch.setenv("HTTPS_PROXY", "http://127.0.0.1:3128")
    env = feedmod.engine_env()
    assert env["NODE_USE_ENV_PROXY"] == "1" and env["HTTPS_PROXY"] == "http://127.0.0.1:3128"


def test_failing_engine_is_a_warning(h, engine, monkeypatch):
    def run_node(root, script, args):
        raise feedmod.EngineError(f"{script} failed (exit 1): boom")

    monkeypatch.setattr(feedmod, "run_node", run_node)
    out = feed(h)
    assert out["news"] == []
    assert out["warnings"] == [f"news: snapshot-feeds.mjs failed (exit 1): boom"]


@pytest.mark.skipif(not shutil.which("node"), reason="needs node")
def test_run_node_reports_the_failing_line(tmp_path):
    (tmp_path / "scripts").mkdir()
    (tmp_path / "scripts" / "ok.mjs").write_text("import {writeFileSync} from 'node:fs'; writeFileSync('done', process.argv[2]);")
    (tmp_path / "scripts" / "bad.mjs").write_text("console.error('first'); console.error('NINCS_PILLANATKÉP'); process.exit(3);")
    feedmod.run_node(tmp_path, "ok.mjs", ["2026-11-03"])
    assert (tmp_path / "done").read_text() == "2026-11-03"
    with pytest.raises(feedmod.EngineError, match=r"bad.mjs failed \(exit 3\): NINCS_PILLANATKÉP"):
        feedmod.run_node(tmp_path, "bad.mjs", [])


def test_follow_inbox_interests(h, web, monkeypatch, tmp_path):
    monkeypatch.setenv("X4_NEWS_ENGINE", str(tmp_path / "nowhere"))
    h.feeds.write_text(f"max_per_feed: 1\nfeeds:\n  - url: {FEED_URL}\n    name: Slow Reading\n  - url: https://down.example/f\n")
    h.inbox.write_text("# saved\n\nhttps://a.example/one\n- https://b.example/two  why not\nnot a link\n")
    h.interests.write_text("Stoicism, urban design.\n")
    out = feed(h)
    assert len(out["follow"]) == 1
    entry = out["follow"][0]
    assert entry["source"] == "Slow Reading" and entry["url"].startswith("https://slow.example/")
    assert set(entry) <= {"title", "url", "source", "date", "summary"} and len(entry.get("summary", "")) <= 201
    assert out["inbox"] == ["https://a.example/one", "https://b.example/two"]
    assert out["interests"] == "Stoicism, urban design."
    assert out["warnings"][1:] == [
        "follow: https://down.example/f: not fetched (unreachable: https://down.example/f)",
        f"inbox: {h.inbox}:5: 'not a link' is not a link; write one http(s) link per line",
    ]
    assert feed(h)["follow"] == []  # the rest of the feed counts as seen


def rss(*items: tuple[str, str, int]) -> bytes:
    """A feed of (title, url, day of November 2026) items."""
    body = "".join(
        f"<item><title>{t}</title><link>{u}</link><guid>{u}</guid><pubDate>{d:02d} Nov 2026 08:00:00 GMT</pubDate></item>"
        for t, u, d in items
    )
    return f"<?xml version='1.0'?><rss version='2.0'><channel><title>x</title>{body}</channel></rss>".encode()


def test_follow_topics_and_caps(h, web, monkeypatch, tmp_path):
    monkeypatch.setenv("X4_NEWS_ENGINE", str(tmp_path / "nowhere"))
    web["https://a.example/f"] = rss(("Beijing's new data law", "https://a.example/1", 3), ("Shanghai port", "https://a.example/2", 2), ("Old", "https://a.example/3", 1))
    web["https://b.example/f"] = rss(("Xi meets Lula", "https://b.example/1", 1), ("More", "https://b.example/2", 1))
    web["https://mixed.example/f"] = rss(("Rain in Paris", "https://mixed.example/1", 3), ("Gaza aid convoy stopped", "https://mixed.example/2", 2))
    h.feeds.write_text(
        "max_per_feed: 2\nmax_per_topic: 3\nfeeds:\n"
        "  - {url: 'https://a.example/f', name: A, topic: china}\n"
        "  - {url: 'https://b.example/f', name: B, topic: china}\n"
        "  - {url: 'https://mixed.example/f', name: M, topic: palestine, only_topic: true}\n"
    )
    out = feed(h)
    assert [(e["title"], e["topics"]) for e in out["follow"]] == [
        ("Beijing's new data law", ["china"]),
        ("Gaza aid convoy stopped", ["palestine"]),
        ("Xi meets Lula", ["china"]),
        ("Shanghai port", ["china"]),
    ]  # each feed's newest before anyone's second; B's second is past the china cap
    assert [e["title"] for e in feed(h, "china")["follow"]] == []


def test_remove_from_inbox(h):
    h.inbox.write_text("# keep\nhttps://a.example/one/\n- https://b.example/two\n")
    assert remove_from_inbox(h, ["https://a.example/one"]) == ["https://a.example/one/"]
    assert h.inbox.read_text() == "# keep\n- https://b.example/two\n"


def test_cli_feed(h, engine):
    result = CliRunner().invoke(main, ["feed"])
    assert result.exit_code == 0
    assert result.output.splitlines()[:2] == ["news: 3 stories", "  3  VIP-bankkártyával éttermeztek a hivatal vezetői  [hungary]"]
    commit_pending(h.feed_state)
    result = CliRunner().invoke(main, ["feed", "--json"])
    assert json.loads(result.output)["news"] == []
