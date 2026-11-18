import json
import shutil
from datetime import date

import pytest

from x4 import primary
from x4.feed import feed
from x4.home import read_json
from x4.topics import tag

from test_feed import TODAY, engine, member  # noqa: F401 - the engine fixture

ARTICLE = (
    "<html><body><nav><a href='https://kormany.hu/'>Kormány</a></nav><article><h1>Új rendelet</h1>"
    "<p>A kormány a 345/2026. (X. 30.) Korm. rendeletben döntött, a szöveg a "
    "<a href='https://magyarkozlony.hu/dokumentumok/abc/megtekintes'>Magyar Közlönyben</a> jelent meg. "
    "A 2026. évi XLVII. törvény módosítását az <a href='https://www.parlament.hu/irom43/1234/1234.pdf'>"
    "Országgyűlés honlapján</a> lehet olvasni, az indoklás pedig "
    "<a href='https://telex.hu/belfold/masik'>egy másik cikkben</a> áll.</p>"
    "<p>Brüsszel szerint az (EU) 2024/1689 rendelet és a Regulation (EU) 2016/679 is érintett, "
    "a jelentés <a href='https://ngo.example/report.pdf'>itt</a> olvasható, a bíróság döntése a "
    "<a href='https://curia.europa.eu/juris/document/document.jsf?docid=1'>Curia oldalán</a>.</p>"
    "</article></body></html>"
)

ACT = {
    "id": "345/2026. (X. 30.) Korm. rendelet",
    "issue": "2026/160",
    "issueDate": TODAY,
    "number": "345/2026. (X. 30.)",
    "actType": "Korm. rendelet",
    "title": "a szociális tűzifa támogatásáról",
    "source": {"url": "https://magyarkozlony.hu/hivatalos-lapok/x/dokumentumok/y/letoltes"},
}


def discovery(day: str) -> dict:
    title = "Rendeletben döntött a kormány a tűzifáról"
    return {
        "day": day,
        "candidates": [
            {
                "claimKey": "tűzifa",
                "title": title,
                "members": [
                    member("telex", title, "https://telex.hu/belfold/tuzifa"),
                    member("444", "Tűzifa: itt a rendelet", "https://444.hu/tuzifa"),
                    member("kozlony", "Magyar Közlöny 2026. évi 160. szám", "https://magyarkozlony.hu/dokumentumok/160/megtekintes"),
                ],
                "pont": 70,
            },
            {
                "claimKey": "gdpr",
                "title": "Brussels fines a Chinese app under the GDPR",
                "members": [member("politico-eu", "Brussels fines a Chinese app under the GDPR", "https://politico.eu/gdpr")],
                "pont": 10,
            },
        ],
    }


@pytest.fixture
def story_engine(engine):  # noqa: F811
    engine.payload["data"] = discovery
    engine.payload["kozlony.mjs"] = {"out/kozlony/acts.jsonl": json.dumps(ACT, ensure_ascii=False) + "\n"}
    watch = {"dossier": "gdpr", "celex": "52026DC0100", "date": TODAY, "title": "Report on the GDPR"}
    engine.payload["eurlex.mjs"] = {"out/eurlex/watch.jsonl": json.dumps(watch) + "\n"}
    (engine.root / "data").mkdir()
    (engine.root / "data" / "eu-dossiers.json").write_text(
        json.dumps({"dossiers": [{"id": "gdpr", "nev": "Általános adatvédelmi rendelet", "celex": "32016R0679"}]})
    )
    engine.pages["https://telex.hu/belfold/tuzifa"] = ARTICLE.encode()
    return engine


def test_primary_sources_of_top_stories(h, story_engine):
    out = feed(h)
    first, second = out["news"]
    assert [x["org"] for x in first["sources"]] == ["Telex", "444"]  # the gazette item is not coverage
    assert first["topics"] == ["hungary"]
    assert first["primary"] == [
        {"title": "Magyar Közlöny 2026. évi 160. szám", "url": "https://magyarkozlony.hu/dokumentumok/160/megtekintes", "kind": "gazette", "date": TODAY},
        {"title": "Magyar Közlönyben", "url": "https://magyarkozlony.hu/dokumentumok/abc/megtekintes", "kind": "gazette"},
        {"title": "Országgyűlés honlapján", "url": "https://www.parlament.hu/irom43/1234/1234.pdf", "kind": "parliament"},
        {"title": "ngo.example/report.pdf", "url": "https://ngo.example/report.pdf", "kind": "document"},
        {"title": "Curia oldalán", "url": "https://curia.europa.eu/juris/document/document.jsf?docid=1", "kind": "court"},
        {
            "title": "345/2026. (X. 30.) Korm. rendelet a szociális tűzifa támogatásáról",
            "url": ACT["source"]["url"],
            "kind": "gazette",
            "date": TODAY,
        },
    ]  # the cited law and EU acts come next, past MAX_PRIMARY
    assert second["topics"] == ["eu", "china"]
    assert second["primary"] == [
        {"title": "Általános adatvédelmi rendelet", "url": "https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32016R0679", "kind": "law"},
        {"title": "Report on the GDPR", "url": "https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:52026DC0100", "kind": "document", "date": TODAY},
    ]
    assert ("eurlex.mjs", []) in story_engine.calls
    assert out["warnings"] == ["primary: 2 of 3 source articles not read; their links are missing"]


def test_citations():
    acts = {"345/2026.(x.30.)|korm.rendelet": ACT}
    text = "A 345/2026. (X. 30.) Korm. rendelettel és a 2026. évi XLVII. törvénnyel; Regulation (EU) No 1308/2013, (EU) 2024/1689 rendelet, 2026/12 (EU) nothing"
    assert [p["kind"] for p in primary._hu_citations(text, acts)] == ["gazette", "law"]
    assert primary._hu_citations(text, acts)[1]["url"] == "https://njt.hu/jogszabaly/2026-47-00-00"
    assert [p["url"].rsplit(":", 1)[1] for p in primary._eu_citations(text, {})] == ["32013R1308", "32024R1689"]


def test_kinds():
    assert primary.kind_of("https://www.gov.cn/zhengce/content/202611/content_1.htm") == "government"
    assert primary.kind_of("https://www.mfa.gov.cn/eng/xw/fyrbt/") == "government"
    assert primary.kind_of("http://www.npc.gov.cn/npc/c2/c30834/") == "parliament"
    assert primary.kind_of("https://www.federalregister.gov/documents/2026/11/03/x") == "gazette"
    assert primary.kind_of("https://www.state.gov/releases/x") == "government"
    assert primary.kind_of("https://www.gov.uk/government/news/x") == "government"
    assert primary.kind_of("https://english.news.cn/20261103/x.htm") == "state-media"
    assert primary.kind_of("https://news.un.org/en/story/2026/11/1") == "international"
    assert primary.kind_of("https://telex.hu/belfold/x") is None


def test_topic_filter_leaves_the_run_unremembered(h, story_engine):
    out = feed(h, "china")
    assert [s["title"] for s in out["news"]] == ["Brussels fines a Chinese app under the GDPR"]
    assert read_json(h.feed_state, {}) == {}
    assert len(feed(h)["news"]) == 2


def test_topics():
    assert tag("Tell us about the EU-ban rules", "en") == ["eu"]
    assert tag("A kormány új rendeletet fogadott el", "hu") == ["hungary"]
    assert tag("Trump kormánya vámot emelt", "hu") == ["us"]
    assert tag("Kínában új törvény", "hu") == ["china"]
    assert tag("Magyarázat a kínálatról és a gázárakról", "hu") == ["hungary"]  # not china, not palestine
    assert tag("Az orosz kormány döntött", "hu") == []
    assert tag("Megvan a Madách Színház új igazgatója", "hu") == ["hungary"]
    assert tag("Could the AI boom make borrowing more expensive in Europe?", "en") == ["eu"]
    assert tag("Israeli strikes on Gaza City", "en") == ["palestine"]
    assert tag("A weekly digest", "en", ["https://www.sixthtone.com/news/1"], ["design"]) == ["design", "china"]


@pytest.mark.skipif(not shutil.which("node"), reason="needs node")
def test_link_runner_uses_the_engine_clustering(tmp_path):
    (tmp_path / "scripts").mkdir()
    (tmp_path / "scripts" / "cluster-claims.mjs").write_text(
        "export function clusterClaims(items) {\n"
        "  const by = new Map();\n"
        "  for (const it of items) { const k = it.title.split(' ')[0]; by.set(k, [...(by.get(k) ?? []), it]); }\n"
        "  return [...by.values()].map((g) => ({members: g.map((m) => ({brand: m.brand, title: m.title, link: m.link}))}));\n"
        "}\n"
    )
    stories = [{"title": "tűzifa a télre", "sources": [{"title": "minimálbér emelés"}]}, {"title": "Gaza", "sources": []}]
    acts = [{"id": "a1", "title": "tűzifa támogatás"}, {"id": "a2", "title": "minimálbér rendelet"}, {"id": "a3", "title": "más"}]
    warnings: list[str] = []
    assert primary._engine_links(tmp_path, stories, acts, warnings) == {(0, "a1"), (0, "a2")}
    assert warnings == []


def test_unread_gazette_issues_are_read_again(tmp_path):
    (tmp_path / "out" / "kozlony" / "2026").mkdir(parents=True)
    (tmp_path / "out" / "kozlony" / "2026" / "MK-163.pdf").write_bytes(b"%PDF")
    calls, warnings = [], []
    primary._kozlony(tmp_path, date.today(), lambda *a: calls.append(a[1:3]), warnings)
    assert calls == [("kozlony.mjs", ["--limit=3", "--force"])]
    assert warnings == ["primary: Közlöny issues without text; install pdftotext (brew install poppler)"]


def test_engine_scripts_run_outside_this_virtualenv(monkeypatch):
    import sys

    from x4 import feed as feedmod

    monkeypatch.setenv("PATH", f"{sys.prefix}/bin:/usr/bin")
    monkeypatch.setenv("VIRTUAL_ENV", sys.prefix)
    env = feedmod.engine_env()
    assert env["PATH"] == "/usr/bin" and "VIRTUAL_ENV" not in env


def test_engine_scripts_see_no_secrets(monkeypatch):
    from x4 import feed as feedmod

    monkeypatch.setenv("ANTHROPIC_API_KEY", "sk-test")
    monkeypatch.setenv("GITHUB_TOKEN", "ghp-test")
    monkeypatch.setenv("LATOTER_HEADLESS", "1")
    monkeypatch.setenv("HTTPS_PROXY", "http://proxy:3128")
    monkeypatch.setenv("LC_ALL", "hu_HU.UTF-8")
    env = feedmod.engine_env()
    assert "ANTHROPIC_API_KEY" not in env and "GITHUB_TOKEN" not in env
    assert env["LATOTER_HEADLESS"] == "1" and env["HTTPS_PROXY"] == "http://proxy:3128" and env["LC_ALL"] == "hu_HU.UTF-8"
    assert "HOME" in env and "PATH" in env
