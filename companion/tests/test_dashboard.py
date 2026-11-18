import json
import shutil
import subprocess
import threading
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

import pytest
import yaml

from x4 import dashboard, settings
from x4.errors import InvalidData
from x4.home import Home

STARTER = Path(__file__).parents[1] / "examples" / "home"


@pytest.fixture
def h(tmp_path):
    root = tmp_path / "x4"
    shutil.copytree(STARTER, root)
    return Home(root)


def test_state_of_the_starter_home(h):
    s = dashboard.state(h)
    assert [x["name"] for x in s["sections"]] == ["News", "Ideas", "Science", "Field"]
    assert s["settings"]["language"] == "en"
    assert len(s["feeds"]) == 11 and s["inbox"] == [] and s["repo"] is False
    assert s["fields"][0] == {"name": "Thermodynamics", "week": None}


def test_sections_round_trip_and_refusal(h):
    dashboard.save(h, "sections", [{"name": "World", "what": "the day's stories"}, {"name": "Mind", "what": "essays"}])
    assert [x["name"] for x in dashboard.state(h)["sections"]] == ["World", "Mind"]
    before = h.sections.read_text()
    with pytest.raises(InvalidData):
        dashboard.save(h, "sections", [{"name": f"S{i}", "what": ""} for i in range(9)])
    assert h.sections.read_text() == before


def test_a_section_stays_one_line(h):
    dashboard.save(h, "sections", [{"name": "World", "what": "the day's\nstories"}])
    assert "World: the day's stories\n" in h.sections.read_text()
    with pytest.raises(InvalidData):
        dashboard.save(h, "sections", [{"name": "A: B", "what": "x"}])


def test_feeds_keep_comments_when_lines_come_and_go(h):
    h.feeds.write_text(h.feeds.read_text().replace("  - {name: Aeon", "  # essays\n  - {name: Aeon"))
    feeds = dashboard.state(h)["feeds"]
    feeds = [f for f in feeds if f["name"] != "NPR"] + [{"name": "Lapham", "url": "https://example.org/rss", "topic": ""}]
    dashboard.save(h, "feeds", feeds)
    text = h.feeds.read_text()
    assert "# essays" in text and "NPR" not in text and "max_per_feed: 2" in text
    assert yaml.safe_load(text)["feeds"][-1] == {"name": "Lapham", "url": "https://example.org/rss"}


def test_feeds_refuse_a_non_link(h):
    with pytest.raises(InvalidData):
        dashboard.save(h, "feeds", [{"name": "x", "url": "example.org"}])


def test_fields_keep_their_header_and_weeks(h):
    dashboard.save(h, "fields", [{"name": "Immunology", "week": "2026-w47"}, {"name": "Thermodynamics", "week": None}])
    text = (h.root / "fields.md").read_text()
    assert text.startswith("# Fields") and "- Immunology (2026-w47)\n- Thermodynamics\n" in text


def test_settings_keep_other_keys_and_refuse_bad_language(h):
    h.settings.write_text("language: en\nnews_engine: ~/engine\n")
    dashboard.save(h, "settings", {"language": "hu"})
    assert settings.load(h) == {"language": "hu", "news_engine": "~/engine"}
    with pytest.raises(InvalidData):
        dashboard.save(h, "settings", {"language": "magyar"})
    assert settings.load(h)["language"] == "hu"


def test_football_key_stays_out_of_the_yaml(h):
    dashboard.save(h, "football", {"key": "abc123", "clubs": [397], "competitions": ["PL", "XX"]})
    assert h.football_key.read_text().strip() == "abc123"
    cfg = yaml.safe_load(h.football_cfg.read_text())
    assert cfg == {"clubs": [397], "competitions": ["PL"]} and "abc123" not in h.football_cfg.read_text()


def test_a_save_is_sent_to_the_data_repo(h, tmp_path):
    remote = tmp_path / "remote.git"
    subprocess.run(["git", "init", "-q", "--bare", str(remote)], check=True)
    for args in (["init", "-q", "-b", "main"], ["config", "user.name", "t"], ["config", "user.email", "t@t"],
                 ["remote", "add", "origin", str(remote)], ["add", "-A"], ["commit", "-q", "-m", "start"], ["push", "-q", "origin", "main"]):
        subprocess.run(["git", "-C", str(h.root), *args], check=True)
    out = dashboard.save(h, "inbox", ["https://example.org/a"])
    assert out == {"saved": "inbox.md", "sent": True}
    log = subprocess.run(["git", "-C", str(remote), "log", "--format=%s", "-1"], capture_output=True, text=True).stdout
    assert log.strip() == "dashboard: inbox.md"


def test_the_page_and_the_api_over_http(h):
    server = ThreadingHTTPServer(("127.0.0.1", 0), dashboard.handler(h))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        assert b"<title>x4 paper</title>" in urllib.request.urlopen(base + "/").read()
        assert json.loads(urllib.request.urlopen(base + "/api/state").read())["sections"][0]["name"] == "News"
        req = urllib.request.Request(base + "/api/taste", data=b'"Slow films"', method="PUT", headers={"Content-Type": "application/json"})
        assert json.loads(urllib.request.urlopen(req).read())["saved"] == "taste.md"
        assert h.taste.read_text() == "Slow films\n"
        form = urllib.request.Request(base + "/api/taste", data=b'"x"', method="PUT", headers={"Content-Type": "text/plain"})
        with pytest.raises(urllib.error.HTTPError) as e:
            urllib.request.urlopen(form)
        assert e.value.code == 415
    finally:
        server.shutdown()
