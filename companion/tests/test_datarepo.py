import json
import subprocess

import pytest
from records import highlight, write_jsonl

from x4 import datarepo, ops
from x4.errors import X4Error
from x4.home import Home

BITE = {"id": "s1", "kind": "bite", "title": "T", "body": "B"}


def git(root, *args) -> str:
    r = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, check=True)
    return r.stdout.strip()


@pytest.fixture
def repo(h, tmp_path):
    """X4_HOME as a checkout of a bare repo standing in for GitHub."""
    remote = tmp_path / "x4-data.git"
    subprocess.run(["git", "init", "-q", "--bare", "-b", "main", str(remote)], check=True)
    git(h.root, "init", "-q", "-b", "main")
    git(h.root, "config", "user.email", "x4@example.com")
    git(h.root, "config", "user.name", "x4")
    git(h.root, "remote", "add", "origin", str(remote))
    (h.root / ".gitignore").write_text("\n".join(datarepo.IGNORE) + "\n")
    return remote


def other(remote, tmp_path) -> Home:
    """A second copy of the repo: the writer somewhere else."""
    root = tmp_path / "elsewhere"
    subprocess.run(["git", "clone", "-q", str(remote), str(root)], check=True)
    git(root, "config", "user.email", "cloud@example.com")
    git(root, "config", "user.name", "cloud")
    return Home(root)


def test_a_home_that_is_no_checkout_is_left_alone(h):
    write_jsonl(h.stream, [BITE])
    assert datarepo.fetch_first(h) is None and datarepo.send_after(h, "x") is None
    assert "repo" not in ops.stage(h)


def test_stage_sends_the_outbox_and_the_memory_but_no_secret(h, repo, tmp_path):
    write_jsonl(h.stream, [BITE])
    h.net.write_text('{"calendars":[{"url":"https://secret"}]}')
    h.repo.write_text('{"name":"a/b","token":"t"}')
    h.football_cfg.write_text("clubs: [397]\n")
    h.football_key.write_text("k\n")
    (h.cache / "a").mkdir(parents=True)
    (h.cache / "a" / "x.epub").write_text("x")
    out = ops.stage(h)
    assert out["repo"] == {"sent": True}
    there = git(repo, "ls-tree", "-r", "--name-only", "main").splitlines()
    assert {".gitignore", "outbox/manifest.json", "outbox/x4/stream.jsonl", "stream.jsonl", ".sync.json"} <= set(there)
    assert "football.yaml" in there
    assert not {"net.json", "repo.json", "football.key", "cache/a/x.epub"} & set(there)
    assert git(repo, "log", "-1", "--format=%s", "main") == f"issue {out['issue']}"
    assert datarepo.send_after(h, "again") == {"sent": False}


def test_fetch_first_brings_what_the_other_writer_sent(h, repo, tmp_path):
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    cloud = other(repo, tmp_path)
    write_jsonl(cloud.stream, [BITE | {"title": "From the cloud"}])
    ops.stage(cloud)
    (h.root / "interests.md").write_text("written here meanwhile, not committed\n")
    assert datarepo.fetch_first(h) == {"fetched": 1}
    assert json.loads(h.stream.read_text())["title"] == "From the cloud"
    assert (h.root / "interests.md").read_text().startswith("written here")
    assert datarepo.fetch_first(h) == {"fetched": 0}


def test_a_copy_that_is_behind_does_not_send(h, repo, tmp_path):
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    cloud = other(repo, tmp_path)
    write_jsonl(cloud.stream, [BITE | {"title": "From the cloud"}])
    ops.stage(cloud)
    write_jsonl(h.stream, [BITE | {"title": "From the Mac"}])
    with pytest.raises(X4Error) as e:
        ops.stage(h)
    assert "not sent" in str(e.value)
    assert git(repo, "show", "main:stream.jsonl").count("From the cloud") == 1
    assert git(h.root, "status", "--porcelain") == ""  # what was written is committed here


def put_inbox(h, name: str, records) -> None:
    h.repo_inbox.mkdir(exist_ok=True)
    path = h.repo_inbox / name
    if isinstance(records, list):
        write_jsonl(path, records)
    else:
        path.write_text(json.dumps(records))


REVIEW = {"card": "meditations/c2", "type": "choice", "grade": 3, "correct": True, "pick": 1, "ms": 5400, "set": 12, "at": None}


def test_the_inbox_is_taken_in_like_a_cable_pull(h):
    put_inbox(h, "20261111T093501Z-highlights.jsonl", [highlight("h-1"), highlight("h-2")])
    put_inbox(h, "20261111T093501Z-reviews.jsonl", [REVIEW])
    put_inbox(h, "20261111T103000Z-highlights.jsonl", [{"id": "h-1", "deleted": True, "set": 13, "at": None}, highlight("h-3")])
    put_inbox(h, "20261111T093501Z-stats.json", {"set": 12, "streak": 1})
    put_inbox(h, "20261111T103000Z-stats.json", {"set": 13, "streak": 2})
    (h.repo_inbox / "20261111T103000Z-taken.json").write_text("not json")
    (h.repo_inbox / "notes.txt").write_text("left alone")
    out = ops.take_inbox(h)
    assert out["highlights"] == {"new": 3, "duplicates": 0, "unreadable": 0, "deleted": 1}
    assert out["reviews"] == {"new": 1, "duplicates": 0, "unreadable": 0}
    assert out["stats"] is True and out["skipped"] == ["20261111T103000Z-taken.json"]
    assert [x.id for x in ops.highlights(h)] == ["h-2", "h-3"]  # the later file's deletion took h-1
    assert json.loads(h.stats.read_text())["set"] == 13  # the newest
    assert sorted(p.name for p in h.repo_inbox.iterdir()) == ["20261111T103000Z-taken.json", "notes.txt"]
    assert out["notes"] and ops.take_inbox(h) == {"skipped": ["20261111T103000Z-taken.json"]}


def test_records_that_came_by_wifi_and_by_cable_are_kept_once(h, dev, sim):
    put_inbox(h, "20261111T093501Z-reviews.jsonl", [REVIEW])
    write_jsonl(sim.root / "x4/reviews.pulling", [REVIEW])  # its upload arrived; the answer was lost
    out = ops.pull(dev, h, "reviews")
    assert out["inbox"]["reviews"]["new"] == 1
    assert out["reviews"] == {"new": 0, "duplicates": 1, "unreadable": 0}
    assert len(h.archive("reviews").read_text().splitlines()) == 1


def test_taken_from_the_inbox_marks_the_issue_as_on_the_x4(h):
    write_jsonl(h.stream, [BITE])
    staged = ops.stage(h)
    put_inbox(h, "20261111T093501Z-taken.json", {"issue": staged["issue"], "files": ["/x4/stream.jsonl"]})
    assert ops.take_inbox(h)["taken"] == staged["issue"]
    from x4.home import read_json

    state = read_json(h.sync_state, {})
    assert state["lan"]["issue"] == staged["issue"] and "/x4/stream.jsonl" in state["pushed"]


def test_a_session_starts_from_the_repo_and_its_inbox(h, repo, tmp_path):
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    x4 = other(repo, tmp_path)  # the X4 puts a file in the repo's inbox
    put_inbox(x4, "20261111T093501Z-highlights.jsonl", [highlight("h-9")])
    git(x4.root, "add", "-A")
    git(x4.root, "commit", "-q", "-m", "x4")
    git(x4.root, "push", "-q", "origin", "HEAD")
    from x4.context import context

    context(h)
    assert [x.id for x in ops.highlights(h)] == ["h-9"]
    assert git(repo, "log", "-1", "--format=%s", "main") == "inbox taken in"  # sent at once
    write_jsonl(h.stream, [BITE | {"title": "The next set"}])
    assert ops.stage(h)["repo"] == {"sent": True}
    assert "inbox/20261111T093501Z-highlights.jsonl" not in git(repo, "ls-tree", "-r", "--name-only", "main")
    assert "h-9" in git(repo, "show", "main:pulled/highlights.jsonl")


def test_what_the_x4_sent_meanwhile_does_not_stop_a_writer(h, repo, tmp_path):
    write_jsonl(h.stream, [BITE])
    ops.stage(h)
    x4 = other(repo, tmp_path)  # after this copy fetched, the X4 sends twice
    for name in ("20261111T093501Z-reviews.jsonl", "20261111T103000Z-reviews.jsonl"):
        put_inbox(x4, name, [REVIEW])
        git(x4.root, "add", "-A")
        git(x4.root, "commit", "-q", "-m", "x4")
        git(x4.root, "push", "-q", "origin", "HEAD")
    write_jsonl(h.stream, [BITE | {"title": "Written meanwhile"}])
    assert ops.stage(h)["repo"] == {"sent": True}
    there = git(repo, "ls-tree", "-r", "--name-only", "main")
    assert "inbox/20261111T093501Z-reviews.jsonl" in there and "inbox/20261111T103000Z-reviews.jsonl" in there
    assert "Written meanwhile" in git(repo, "show", "main:stream.jsonl")
    assert (h.repo_inbox / "20261111T103000Z-reviews.jsonl").exists()  # here too, for the next session


def test_a_key_left_in_football_yaml_is_not_sent(h, repo):
    write_jsonl(h.stream, [BITE])
    h.football_cfg.write_text("key: k\nclubs: [397]\n")
    with pytest.raises(X4Error) as e:
        datarepo.send_after(h, "x")
    assert "football.key" in e.value.details[0]
