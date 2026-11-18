import json
import plistlib
import threading
import zlib
from pathlib import Path

import httpx
import pytest
from records import write_jsonl

from x4 import lan, net, ops
from x4.errors import InvalidData
from x4.home import read_json

BITE = {"id": "s1", "kind": "bite", "title": "T", "body": "B"}


def staged(h) -> dict:
    return json.loads((h.outbox / "manifest.json").read_text())


@pytest.fixture
def article(h):
    """A Read list of one Markdown article, converted without the network."""
    path = h.root / "articles" / "r-1.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("---\ntitle: Brighton\nlang: en\n---\nOwnership and recruitment, in brief.\n")
    write_jsonl(h.read, [{"id": "r-1", "kind": "dossier", "file": "articles/r-1.md"}])
    return path


def test_stage_writes_the_outbox(h, article):
    write_jsonl(h.stream, [BITE])
    h.football.write_text('{"fetched":1,"matches":[],"tables":[]}\n')
    out = ops.stage(h)
    m = staged(h)
    assert out["issue"] == m["issue"] and len(m["issue"]) == 19
    assert [f["path"] for f in m["files"]] == ["/books/read/r-1.epub", "/x4/read.jsonl", "/x4/stream.jsonl", "/x4/football.json"]
    assert m["prune"] == ["/books/read", "/x4/decks"]
    for f in m["files"]:
        data = (h.outbox / f["path"].lstrip("/")).read_bytes()
        assert f["size"] == len(data) and f["crc32"] == zlib.crc32(data)
    listed = json.loads((h.outbox / "x4/read.jsonl").read_text())
    assert listed["kind"] == "dossier" and listed["book"] == "books/read/r-1.epub"
    assert read_json(h.sync_state, {})["read"]["r-1"]["kind"] == "dossier"
    assert sorted(p.name for p in h.root.iterdir() if p.name.startswith(".outbox")) == []  # no temp dirs left


def test_stage_leaves_out_an_article_it_cannot_fetch(h, monkeypatch):
    from x4.convert import net as fetcher

    def fail(url):
        raise OSError("unreachable")

    monkeypatch.setattr(fetcher, "fetch", fail)
    write_jsonl(h.read, [{"id": "r-9", "kind": "found", "url": "https://gone.example/a"}])
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    assert [json.loads(x)["id"] for x in (h.outbox / "x4/stream.jsonl").read_text().splitlines()] == ["s1"]
    assert (h.outbox / "x4/read.jsonl").read_text() == ""


def test_stage_replaces_the_previous_issue(h, article):
    ops.stage(h)
    (h.outbox / "stale.txt").write_text("x")
    h.read.unlink()
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    assert not (h.outbox / "stale.txt").exists() and not (h.outbox / "books").exists()
    assert staged(h)["prune"] == ["/x4/decks"] and [f["path"] for f in staged(h)["files"]] == ["/x4/stream.jsonl"]


def test_stage_refuses_invalid_files(h):
    write_jsonl(h.stream, [{"id": "s1", "kind": "bite"}])
    with pytest.raises(InvalidData) as e:
        ops.stage(h)
    assert "staged nothing" in e.value.message
    assert not h.outbox.exists()


def test_stage_carries_what_the_writer_writes_beside_the_issue(h):
    from records import DECK

    write_jsonl(h.stream, [BITE])
    write_jsonl(h.threads, [{"id": "t-1", "title": "T", "body": "So far."}])
    write_jsonl(h.decks / "meditations.jsonl", DECK)
    write_jsonl(h.previously, [{"book": "books/meditations.epub", "progress": 0.4, "text": "Eddig."}])
    ops.stage(h)
    m = staged(h)
    assert [f["path"] for f in m["files"]] == [
        "/x4/stream.jsonl",
        "/x4/threads.jsonl",
        "/x4/decks/meditations.jsonl",
        "/x4/previously.jsonl",
    ]
    assert m["prune"] == ["/x4/decks"]  # no Read list in this issue: its articles stay


def test_manifest_limits():
    big = lan.MAX_FILE + 1
    found = lan.problems(
        [
            ("/x4/stream.jsonl", 10),
            ("/x4/decks/a.jsonl", 1),
            ("/x4/crosswords/hu.jsonl", 1),
            ("/x4/next.jsonl", 1),
            ("/x4/crosswords/hu.grids.jsonl", 1),
            ("/x4/decks/.a.jsonl", 1),
            ("/books/read/../x.epub", 1),
            ("/books/read/r-1.epub", big),
        ],
        ["/books"],
    )
    assert found == [
        "/x4/next.jsonl: not a path the X4 takes over Wi-Fi",
        "/x4/crosswords/hu.grids.jsonl: not a path the X4 takes over Wi-Fi",
        "/x4/decks/.a.jsonl: not a path the X4 takes over Wi-Fi",
        "/books/read/../x.epub: not a path the X4 takes over Wi-Fi",
        f"/books/read/r-1.epub: {big} bytes, more than {lan.MAX_FILE}",
        "prune /books: only /books/read, /x4/decks",
    ]
    many = [(f"/books/read/r-{i}.epub", 400_000) for i in range(49)]
    assert lan.problems(many, []) == ["49 files, more than 48", f"{49 * 400_000} bytes in all, more than {lan.MAX_TOTAL}"]
    with pytest.raises(InvalidData):
        lan.write_outbox(lan.Home(Path("/nonexistent")), [("/x4/next.jsonl", b"x")], [])


@pytest.fixture
def served(h):
    """x4 serve on 127.0.0.1, an ephemeral port."""
    s = lan.server(h, "127.0.0.1", 0)
    threading.Thread(target=s.serve_forever, daemon=True).start()
    token = lan.config(h)["token"]
    client = httpx.Client(base_url=f"http://127.0.0.1:{s.server_address[1]}", headers={"Authorization": f"Bearer {token}"})
    yield client
    client.close()
    s.shutdown()
    s.server_close()


def test_serve_needs_the_token(h, served):
    assert served.get("/issue", headers={"Authorization": "Bearer nope"}).status_code == 401
    assert httpx.get(served.base_url.join("/issue")).status_code == 401
    assert served.post("/taken", json={}, headers={"Authorization": ""}).status_code == 401
    assert (h.serve.stat().st_mode & 0o777) == 0o600


def test_serve_hands_out_the_issue(h, served, article):
    assert served.get("/issue").status_code == 204
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    r = served.get("/issue")
    assert r.status_code == 200 and r.json() == staged(h)
    for f in r.json()["files"]:
        got = served.get("/file", params={"path": f["path"]})
        assert got.status_code == 200 and zlib.crc32(got.content) == f["crc32"]
    (h.root / "secret.txt").write_text("no")
    for path in ("/x4/decks/a.jsonl", "/../secret.txt", "/books/read/../../secret.txt", "/x4/football.json", ""):
        assert served.get("/file", params={"path": path}).status_code == 404
    assert served.get("/elsewhere").status_code == 404


def test_taken_marks_files_pushed(h, served, article, sim, dev):
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    m = staged(h)
    for f in m["files"]:  # the X4 took the files over Wi-Fi
        dst = sim.root / f["path"].lstrip("/")
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes((h.outbox / f["path"].lstrip("/")).read_bytes())
    assert served.post("/taken", content=b"not json").status_code == 400
    r = served.post("/taken", json={"issue": m["issue"], "files": [f["path"] for f in m["files"]] + ["/x4/stats.json"]})
    assert r.status_code == 204
    state = read_json(h.sync_state, {})
    assert state["pushed"] == {f["path"]: f["crc32"] for f in m["files"]}
    assert state["lan"]["issue"] == m["issue"]

    out = ops.sync(dev, h)
    assert out["pushed"] == ["/x4/net.json"] and out["unchanged"] == 3  # net.json: serve.json is new
    assert not h.outbox.exists()  # emptied by the USB sync


def test_taken_for_an_older_issue_records_no_files(h, served):
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    assert served.post("/taken", json={"issue": "2020-01-01T00:00:00", "files": ["/x4/stream.jsonl"]}).status_code == 204
    state = read_json(h.sync_state, {})
    assert state["pushed"] == {} and state["lan"]["issue"] == "2020-01-01T00:00:00"


def test_config_keeps_its_token_and_saves_a_new_port(h):
    first = lan.config(h)
    assert first["port"] == 8744 and len(first["token"]) == 32
    assert lan.config(h, 9000) == first | {"port": 9000}
    assert lan.config(h) == first | {"port": 9000}


def test_net_json_carries_mac(h):
    assert json.loads(net.device_body(h)).get("mac") is None  # no serve.json yet
    cfg = lan.config(h)
    body = json.loads(net.body(net.load(h), tz="UTC0", mac=lan.mac(h, ip="192.168.1.23")))
    assert body["mac"] == {"url": "http://192.168.1.23:8744", "token": cfg["token"]}


def test_net_json_carries_the_repo_and_x4_never_shows_its_token(h):
    from click.testing import CliRunner

    from x4.cli import main

    seen = []

    def github(request):
        seen.append((str(request.url), request.headers["Authorization"]))
        return httpx.Response(200 if request.headers["Authorization"] == "Bearer good" else 401, json={})

    client = httpx.Client(transport=httpx.MockTransport(github))
    assert json.loads(net.device_body(h)).get("repo") is None
    with pytest.raises(InvalidData) as e:
        net.set_repo(h, "you/x4-data", "bad", client)
    assert e.value.details == ["you/x4-data: GitHub does not know this token"] and not h.repo.exists()
    for name in ("x4-data", "a/b/c", "a/b?x", "/b"):
        with pytest.raises(InvalidData):
            net.set_repo(h, name, "good", client)
    assert net.set_repo(h, "you/x4-data", " good\n", client) == {"repo": "you/x4-data"}
    assert seen[-1] == ("https://api.github.com/repos/you/x4-data", "Bearer good")
    assert h.repo.stat().st_mode & 0o777 == 0o600
    assert json.loads(net.device_body(h))["repo"] == {"name": "you/x4-data", "token": "good"}
    for args in (["repo"], ["--json", "repo"], ["--json", "calendar"]):
        r = CliRunner().invoke(main, args)
        assert r.exit_code == 0 and "good" not in r.output
    assert "you/x4-data" in CliRunner().invoke(main, ["repo"]).output
    assert net.forget_repo(h) == {"repo": None} and json.loads(net.device_body(h)).get("repo") is None


def test_sync_pushes_net_json_with_mac(h, dev, sim, monkeypatch):
    lan.config(h)
    monkeypatch.setattr(lan, "lan_ip", lambda: "10.0.0.5")
    assert ops.sync(dev, h)["pushed"] == ["/x4/net.json"]
    assert json.loads((sim.root / "x4/net.json").read_text())["mac"]["url"] == "http://10.0.0.5:8744"
    assert ops.sync(dev, h)["pushed"] == []
    monkeypatch.setattr(lan, "lan_ip", lambda: "10.0.0.6")  # a new address goes with the next sync
    assert ops.sync(dev, h)["pushed"] == ["/x4/net.json"]


def test_sync_and_stage_fetch_football_first(h, dev, monkeypatch):
    from x4 import football

    h.football_cfg.write_text("key: k\nclubs: [397]\ncompetitions: [PL]\n")
    calls = []

    def fetch(home):
        calls.append(home)
        home.football.write_text('{"fetched":2,"matches":[],"tables":[]}')
        return {"warnings": ["club 1: gone"]}

    monkeypatch.setattr(football, "fetch", fetch)
    out = ops.stage(h)
    assert [f["path"] for f in out["files"]] == ["/x4/football.json"] and out["warnings"] == ["football: club 1: gone"]
    out = ops.sync(dev, h)
    assert out["pushed"] == ["/x4/football.json"] and "football: club 1: gone" in out["warnings"]
    assert len(calls) == 2


def test_plist_runs_serve_at_login(tmp_path):
    data = plistlib.loads(lan.plist("/venv/bin/x4", tmp_path / "x4", tmp_path / "log"))
    assert data["Label"] == "x4r.serve"
    assert data["ProgramArguments"] == ["/venv/bin/x4", "serve"]
    assert data["RunAtLoad"] is True and data["KeepAlive"] is True
    assert data["EnvironmentVariables"] == {"X4_HOME": str(tmp_path / "x4")}


def test_cli_stage_and_football(h, monkeypatch):
    from click.testing import CliRunner

    from x4.cli import main

    write_jsonl(h.stream, [BITE])
    r = CliRunner().invoke(main, ["--json", "stage"])
    assert r.exit_code == 0 and json.loads(r.output)["files"][0]["path"] == "/x4/stream.jsonl"
    r = CliRunner().invoke(main, ["stage"])
    assert r.exit_code == 0 and "/x4/stream.jsonl" in r.output
    r = CliRunner().invoke(main, ["--json", "football"])
    assert r.exit_code == 2 and json.loads(r.output)["error"]["code"] == "invalid"
