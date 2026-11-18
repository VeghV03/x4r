"""The paper played in the simulator by the firmware's own logic and drawing (`x4 sim --play`)."""

import io
import json

import pytest
from click.testing import CliRunner
from PIL import Image
from records import TODAY, write_jsonl

from x4.cli import main
from x4.home import read_jsonl
from x4.sim import BUILD, PAINTER, PLAYER, Sim, serve_pty

pytestmark = pytest.mark.skipif(not PLAYER.is_file(), reason="x4play is not built (CLAUDE.md: host tests)")

ISSUE = [
    {"date": "2026-11-12", "sections": [{"id": "hungary", "name": "Hungary"}, {"id": "world", "name": "World"}]},
    {"id": "r-1", "section": "hungary", "kind": "news", "title": "A long headline", "head": "Short one",
     "lead": "What it says.", "source": "Telex", "book": "books/read/r-1.epub"},
    {"id": "r-2", "section": "hungary", "kind": "news", "title": "Second headline", "head": "Second",
     "lead": "Second lead.", "source": "444", "book": "books/read/r-2.epub"},
    {"id": "r-3", "section": "world", "kind": "news", "title": "World headline", "head": "World short",
     "lead": "Abroad.", "source": "DW", "book": "books/read/r-3.epub"},
]


@pytest.fixture
def sd(tmp_path):
    root = tmp_path / "sd"
    (root / "books/read").mkdir(parents=True)
    for n in (1, 2, 3):
        (root / f"books/read/r-{n}.epub").write_bytes(b"x")
    write_jsonl(root / "x4/read.jsonl", ISSUE)
    return root


@pytest.fixture
def x4(sd, h, monkeypatch):
    sim = Sim(sd, play=PLAYER)
    port, stop = serve_pty(sim)
    monkeypatch.setenv("X4_PORT", port)
    runner = CliRunner()
    yield lambda *args: runner.invoke(main, list(args), catch_exceptions=False)
    stop.set()
    sim.player.close()


def screen(x4) -> dict:
    return json.loads(x4("--json", "screen").output)


def test_the_paper_is_leafed_through_by_presses(x4, sd):
    at = screen(x4)
    assert at["activity"] == "Home"
    assert [(s["name"], s["head"]) for s in at["paper"]["sections"]] == [("Hungary", "Short one"), ("World", "World short")]

    x4("press", "confirm")
    at = screen(x4)
    assert (at["activity"], at["section"], at["piece"]) == ("Section", "hungary", "r-1")
    assert [p["title"] for p in at["paper"]["pieces"]] == ["A long headline", "Second headline"]

    # Reading a piece to its end and leaving: it is read, and the front page moves on to the next.
    x4("press", "confirm")
    assert screen(x4) == {"activity": "EpubReader", "book": "books/read/r-1.epub", "page": "first"}
    x4("press", "right")
    x4("press", "right")
    x4("press", "back")
    at = screen(x4)
    assert at["activity"] == "Section" and at["paper"]["pieces"][0]["read"] is True
    assert read_jsonl(sd / "x4/read-log.jsonl")[0]["finished"] is True

    x4("press", "right")
    assert screen(x4)["section"] == "world"
    x4("press", "back")
    at = screen(x4)
    assert at["activity"] == "Home" and at["row"] == 1
    assert at["paper"]["sections"][0]["head"] == "Second"


@pytest.mark.skipif(not PAINTER.is_file() or not (BUILD / "fonts").is_dir(), reason="x4paper or its fonts are not built")
def test_a_screenshot_is_the_firmwares_own_drawing(x4, tmp_path):
    out = tmp_path / "front.png"
    x4("screenshot", str(out))
    image = Image.open(io.BytesIO(out.read_bytes()))
    assert image.size == (480, 800)
    dark = sum(1 for v in image.convert("L").getdata() if v < 128)
    assert 2000 < dark < 60000  # ink, and mostly paper


def test_a_hold_keeps_a_piece_through_the_next_issue(x4, sd, h):
    x4("press", "confirm")  # Hungary
    x4("press", "confirm", "--long")  # keep its first piece
    at = screen(x4)
    assert [p["kept"] for p in at["paper"]["pieces"]] == [True, False]
    assert read_jsonl(sd / "x4/kept.jsonl")[0] | {"at": None} == {"id": "r-1", "kept": True, "day": TODAY, "at": None}
    x4("press", "back")

    # The next day's issue lists neither: the kept one stays on the card, the other leaves.
    (sd / "books/read/r-9.epub").write_bytes(b"x")
    write_jsonl(sd / "x4/read.jsonl", [
        {"date": "2026-11-13", "sections": [{"id": "hungary", "name": "Hungary"}]},
        {"id": "r-9", "section": "hungary", "kind": "news", "title": "Tomorrow", "head": "Tomorrow",
         "lead": "New.", "source": "Telex", "book": "books/read/r-9.epub"},
    ])
    assert screen(x4)["paper"]["sections"][0]["head"] == "Tomorrow"
    assert sorted(p.name for p in (sd / "books/read").iterdir()) == ["r-1.epub", "r-9.epub"]

    # It waits in the Library's Kept, the last row of the front page.
    x4("press", "up")
    x4("press", "confirm")
    at = screen(x4)
    assert (at["activity"], at["tab"], at["kept"]) == ("Library", "Kept", ["A long headline"])
    # A hold there lets it go.
    x4("press", "confirm", "--long")
    assert screen(x4)["kept"] == []
    assert [r["kept"] for r in read_jsonl(sd / "x4/kept.jsonl")] == [True, False]

    assert json.loads(x4("--json", "pull", "kept").output)["kept"]["new"] == 2
    assert [r["id"] for r in read_jsonl(h.archive("kept"))] == ["r-1", "r-1"]


def test_inside_an_article_keep_is_the_menus_first_row(x4, sd):
    x4("press", "confirm")  # Hungary
    x4("press", "confirm")  # its first piece
    x4("press", "confirm")  # the reader's menu
    assert screen(x4) == {"activity": "EpubReaderMenu", "book": "books/read/r-1.epub", "page": "first", "row": "Keep"}
    x4("press", "confirm")  # Keep
    assert screen(x4)["activity"] == "EpubReader"
    x4("press", "confirm")
    assert screen(x4)["row"] == "Let go"
    x4("press", "back")  # the menu closes, the piece stays kept
    x4("press", "back")
    at = screen(x4)
    assert at["activity"] == "Section" and at["paper"]["pieces"][0]["kept"] is True
    assert [r["kept"] for r in read_jsonl(sd / "x4/kept.jsonl")] == [True]


@pytest.mark.skipif(not PAINTER.is_file() or not (BUILD / "fonts").is_dir(), reason="x4paper or its fonts are not built")
def test_a_kept_piece_carries_its_mark_in_the_drawing(x4, tmp_path):
    x4("press", "confirm")
    before, after = tmp_path / "before.png", tmp_path / "after.png"
    x4("screenshot", str(before))
    x4("press", "confirm", "--long")
    x4("screenshot", str(after))
    a, b = (Image.open(io.BytesIO(p.read_bytes())).convert("L") for p in (before, after))
    changed = [i for i, (x, y) in enumerate(zip(a.getdata(), b.getdata())) if x != y]
    # The ribbon: a small mark at the right margin of the first piece's outlet line.
    assert 60 < len(changed) < 200
    assert all(430 <= i % 480 <= 444 for i in changed)


ASKS = {
    "predict": {"prompt": "Which way did it go?", "options": ["Up", "Down", "Flat"], "answer": 1,
                "quote": "It went down."},
    "link": {"prompt": "Which says the opposite?", "options": [{"text": "One", "ref": "h-1"},
                                                              {"text": "Two", "ref": "h-2"},
                                                              {"text": "Three", "ref": "h-3"}],
             "answer": 0, "why": "One denies it."},
}


def asking(sd, **more):
    """The issue again, its first piece carrying a question and a link, its third `more`."""
    head, first, second, third = ISSUE
    write_jsonl(sd / "x4/read.jsonl", [head, first | ASKS, second, third | more])


def test_a_piece_asks_before_and_answers_once_read(x4, sd):
    asking(sd)
    x4("press", "confirm")  # Hungary
    x4("press", "confirm")  # its first piece: the question first
    at = screen(x4)
    assert (at["activity"], at["piece"]) == ("Questions", "r-1")
    assert at["step"] == {"phase": "prompt", "kind": "predict", "id": "r-1", "selected": 0, "prompt": "Which way did it go?"}
    x4("press", "down")
    x4("press", "confirm")  # the guess: nothing is marked, the article opens
    assert screen(x4)["activity"] == "EpubReader"
    guess = read_jsonl(sd / "x4/seen.jsonl")[0]
    assert (guess["id"], guess["predict"]["pick"], guess["predict"]["correct"]) == ("r-1", 1, True)

    # Left unread, nothing follows; opened again, it does not ask twice.
    x4("press", "back")
    assert screen(x4)["activity"] == "Section"
    x4("press", "confirm")
    assert screen(x4)["activity"] == "EpubReader"

    # A page turned earns the answer, then one more thing: the link.
    x4("press", "right")
    x4("press", "back")
    at = screen(x4)
    assert at["step"] == {"phase": "answered", "kind": "predict", "id": "r-1", "picked": 1, "prompt": "Which way did it go?"}
    x4("press", "confirm")
    at = screen(x4)
    assert (at["step"]["kind"], at["step"]["phase"], at["step"]["prompt"]) == ("link", "prompt", "Which says the opposite?")
    x4("press", "down")
    x4("press", "confirm")
    assert screen(x4)["step"]["picked"] == 1
    x4("press", "confirm")
    assert screen(x4)["activity"] == "Section"
    link = read_jsonl(sd / "x4/seen.jsonl")[-1]
    assert (link["id"], link["link"]["pick"], link["link"]["correct"]) == ("r-1", 1, False)

    # Done: read again, it asks nothing.
    x4("press", "confirm")
    x4("press", "right")
    x4("press", "back")
    assert screen(x4)["activity"] == "Section"


def test_a_story_shows_where_it_stands_and_its_forecast_follows(x4, sd):
    asking(sd, thread="t-1", forecast={"id": "f-1", "text": "The vote passes.", "by": "2030-12-16"})
    write_jsonl(sd / "x4/threads.jsonl", [{"id": "t-1", "title": "The vote", "body": "So far."}])
    x4("press", "down")
    x4("press", "confirm")  # World
    x4("press", "confirm")  # its piece: the story so far first
    assert screen(x4) == {"activity": "Thread", "piece": "r-3"}
    x4("press", "confirm")
    assert screen(x4)["activity"] == "EpubReader"
    x4("press", "right")
    x4("press", "back")
    at = screen(x4)
    assert (at["step"]["kind"], at["step"]["id"], at["step"]["selected"]) == ("forecast", "f-1", 2)
    assert at["step"]["prompt"].startswith("The vote passes.")
    x4("press", "down")
    x4("press", "confirm")
    assert screen(x4)["activity"] == "Section"
    row = read_jsonl(sd / "x4/seen.jsonl")[-1]
    assert (row["id"], row["p"], row["read"]) == ("f-1", 70, "r-3")
    # The story so far shows once for each version of its text.
    x4("press", "confirm")
    assert screen(x4)["activity"] == "EpubReader"


def test_a_kept_piece_still_answers_when_its_issue_has_gone(x4, sd):
    asking(sd)
    x4("press", "confirm")
    x4("press", "confirm", "--long")  # keep the first piece
    x4("press", "confirm")
    x4("press", "confirm")  # guess its first option
    x4("press", "back")  # unread
    x4("press", "back")
    (sd / "books/read/r-9.epub").write_bytes(b"x")
    write_jsonl(sd / "x4/read.jsonl", [
        {"date": "2026-11-13", "sections": [{"id": "hungary", "name": "Hungary"}]},
        {"id": "r-9", "section": "hungary", "kind": "news", "title": "Tomorrow", "head": "Tomorrow",
         "lead": "New.", "source": "Telex", "book": "books/read/r-9.epub"},
    ])
    screen(x4)  # the front page takes the new issue
    x4("press", "up")
    x4("press", "confirm")  # the Library's Kept
    x4("press", "confirm")  # the kept piece: guessed already, so the article
    assert screen(x4)["activity"] == "EpubReader"
    x4("press", "right")
    x4("press", "back")
    at = screen(x4)
    assert (at["activity"], at["step"]["kind"], at["step"]["phase"], at["step"]["picked"]) == ("Questions", "predict", "answered", 0)
    x4("press", "confirm")
    assert screen(x4)["step"]["kind"] == "link"
    x4("press", "back")  # what was left is not asked again
    assert screen(x4)["activity"] == "Library"
