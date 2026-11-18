import json
import os

from PIL import Image
from records import DECK, highlight, write_jsonl

from x4 import device


def test_status_text_and_json(x4, sim):
    write_jsonl(sim.root / "x4/highlights.jsonl", [highlight("h1")])
    r = x4("status")
    assert r.exit_code == 0
    assert "not pulled: 1 highlights, 0 reviews\nheap 150 KB free, 120 KB lowest, 64 KB largest block\n" in r.output
    for args in (("--json", "status"), ("status", "--json")):
        data = json.loads(x4(*args).output)
        assert data["new_highlights"] == 1 and data["battery"] == 82
        assert "retention" not in data


def test_status_names_the_stage_at_each_new_low_in_free_memory(x4, sim):
    sim.heap_low = [{"at": "net:weather", "min": 14336}, {"at": "usb", "min": 8192}]
    assert "64 KB largest block\n  low 14 KB after net:weather\n  low 8 KB after usb\n" in x4("status").output
    assert json.loads(x4("--json", "status").output)["heap_low"] == sim.heap_low


def test_status_shows_retention_once_known(x4, sim):
    write_jsonl(sim.root / "x4/stats.json", [{"day": 3, "retention": 0.96}])
    assert "day 3  retention 96%\n" in x4("status").output
    assert json.loads(x4("--json", "status").output)["retention"] == 0.96


def test_validation_exit_code_2(x4, tmp_path):
    bad = write_jsonl(tmp_path / "d.jsonl", [DECK[0], {"id": "c1", "type": "flip", "front": "a"}])
    r = x4("--json", "push", "deck", str(bad))
    assert r.exit_code == 2
    err = json.loads(r.output)["error"]
    assert err["code"] == "invalid" and err["details"] == [f"{bad}:2: back: is required; add it"]
    r = x4("push", "deck", str(bad))
    assert r.exit_code == 2 and "back: is required" in r.output


def test_device_error_exit_code_1(x4):
    r = x4("--json", "get", "/missing")
    assert r.exit_code == 1
    assert json.loads(r.output) == {"error": {"code": "not_found", "message": "get /missing: not_found: no such file"}}


def test_no_device_exit_code_1(h, monkeypatch):
    from click.testing import CliRunner

    from x4 import device
    from x4.cli import main

    monkeypatch.delenv("X4_PORT", raising=False)
    monkeypatch.setattr(device.list_ports, "comports", lambda: [])
    monkeypatch.setattr(device.glob, "glob", lambda _: [])
    r = CliRunner().invoke(main, ["status"])
    assert r.exit_code == 1 and "no X4 on USB" in r.output


def test_files(x4, sim, tmp_path):
    local = tmp_path / "a.txt"
    local.write_text("hello")
    assert x4("put", str(local), "/x4/docs/").exit_code == 0
    assert (sim.root / "x4/docs/a.txt").read_text() == "hello"
    assert x4("ls", "/x4/docs").output == "a.txt  5 B\n"
    assert json.loads(x4("--json", "ls", "/x4").output)["entries"] == [{"name": "docs", "dir": True, "size": 0}]
    out = tmp_path / "b.txt"
    assert x4("get", "/x4/docs/a.txt", str(out)).exit_code == 0 and out.read_text() == "hello"
    assert x4("rm", "/x4/docs").exit_code == 1
    assert x4("rm", "-r", "/x4/docs").exit_code == 0 and not (sim.root / "x4/docs").exists()


def test_get_large_file_over_a_lossy_port(x4, sim, tmp_path, monkeypatch):
    monkeypatch.setattr(device, "GET_TIMEOUT", 0.3)
    monkeypatch.setattr(device, "DRAIN_QUIET", 0.05)
    blob = os.urandom(512 * 1024 + 1)
    (sim.root / "x4").mkdir()
    (sim.root / "x4/book.epub").write_bytes(blob)
    sim.lossy = True
    sim.faults = ["cut", "drop"]
    out = tmp_path / "book.epub"
    r = x4("--json", "get", "/x4/book.epub", str(out))
    assert r.exit_code == 0 and json.loads(r.output)["bytes"] == len(blob)
    assert out.read_bytes() == blob


def test_screenshot_press_screen(x4, sim, tmp_path):
    png = tmp_path / "s.png"
    assert x4("screenshot", str(png)).exit_code == 0
    img = Image.open(png)
    assert img.size == (800, 480) and img.getpixel((0, 0)) == 255
    assert x4("press", "confirm", "--long").output == "home\n"
    assert sim.presses == [{"button": "confirm", "long": True}]
    assert json.loads(x4("--json", "screen").output) == {"activity": "home"}
    assert x4("press", "menu").exit_code == 2


def test_press_retries_while_busy(x4, sim, monkeypatch):
    monkeypatch.setattr("x4.device.PRESS_RETRY_WAIT", 0)
    sim.busy_presses = 2
    assert x4("press", "down").exit_code == 0
    assert sim.presses == [{"button": "down", "long": False}]
    sim.busy_presses = 10
    r = x4("press", "down")
    assert r.exit_code == 1 and "a press is in progress" in r.output
    assert len(sim.presses) == 1


def test_lag_lists_presses(x4, sim):
    assert x4("lag").output == "no presses yet\n"
    press = {"activity": "Reader", "idle": 12300, "up": 95, "panel": 140, "done": 560, "last": 1210, "updates": 2, "mode": "fast", "low_clock": True}
    sim.lag = [press, {**press, "up": 0, "low_clock": False, "idle": 800}]
    assert x4("lag").output == (
        "Reader       after  12.3 s  up   95  panel  140  done  560  last 1210 (2)  fast  idle clock\n"
        "Reader       after   0.8 s  held     panel  140  done  560  last 1210 (2)  fast\n"
    )
    assert json.loads(x4("--json", "lag").output) == {"presses": sim.lag, "marks": []}


def test_pull_push_sync_context(x4, sim, h, tmp_path):
    write_jsonl(sim.root / "x4/highlights.jsonl", [highlight("h1")])
    assert x4("pull").output.startswith("highlights: 1 new\n")
    deck = write_jsonl(tmp_path / "d.jsonl", DECK)
    assert x4("push", "deck", str(deck)).output == "meditations: 2 cards -> /x4/decks/meditations.jsonl\n"
    data = json.loads(x4("--json", "sync").output)
    assert data["pushed"] == [] and data["unchanged"] == 1
    c = json.loads(x4("context").output)
    assert c["highlights"][0]["id"] == "h1" and c["decks"]["meditations"]["cards"] == 2


def test_frame_layout():
    from x4.ops import frame_to_image

    frame = bytes([0b0111_1111]) + b"\xff" * 99 + b"\x00" * 100
    img = frame_to_image(frame, 800, 2)
    assert img.getpixel((0, 0)) == 0 and img.getpixel((1, 0)) == 255  # MSB first, 0 is black
    assert img.getpixel((0, 1)) == 0 and img.getpixel((799, 1)) == 0  # rows of 100 bytes


def test_a_bad_home_file_is_an_error_not_a_traceback(x4, h):
    h.archive("reviews").parent.mkdir(parents=True, exist_ok=True)
    h.archive("reviews").write_text('{"card": 1}\n')
    r = x4("--json", "context")
    assert r.exit_code == 1
    assert json.loads(r.output)["error"]["code"] == "invalid"


def test_rm_and_put_leave_device_files_alone(x4, sim, tmp_path):
    write_jsonl(sim.root / "x4/highlights.jsonl", [highlight("h1")])
    (sim.root / "x4/state").mkdir()
    local = tmp_path / "a.txt"
    local.write_text("x")
    for args in (
        ("rm", "-r", "/"),
        ("rm", "-r", "/X4"),
        ("rm", "/x4/highlights.jsonl"),
        ("rm", "-r", "/x4/state/"),
        ("put", str(local), "/x4/stats.json"),
        ("put", str(local), "/x4/state/"),
    ):
        r = x4("--json", *args)
        assert r.exit_code == 1 and json.loads(r.output)["error"]["code"] == "refused", args
    assert (sim.root / "x4/highlights.jsonl").exists()
    assert x4("put", str(local), "/x4/notes.txt").exit_code == 0
    assert x4("rm", "--force", "/x4/highlights.jsonl").exit_code == 0
    assert not (sim.root / "x4/highlights.jsonl").exists()
