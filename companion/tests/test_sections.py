import json

import pytest
from records import write_jsonl

from x4 import ops, sections
from x4.errors import InvalidData
from x4.home import read_json

from test_readlist import ARTICLE, HU_ARTICLE, device_list, web  # noqa: F401

SECTIONS = """# The paper
Hungary: the day's biggest Hungarian stories
World: the US, the EU, Palestine and China
Élet és irodalom: essays
"""


def piece(n, section="hungary", **more):
    return {"id": f"r-{n}", "kind": "news", "url": ARTICLE if n % 2 else HU_ARTICLE, "section": section,
            "head": "A short headline", "lead": "What the piece says, in a line or two."} | more


def test_sections_are_names_in_order(h):
    assert sections.load(h) is None
    h.sections.write_text(SECTIONS, encoding="utf-8")
    assert [(s.id, s.name) for s in sections.load(h)] == [
        ("hungary", "Hungary"), ("world", "World"), ("elet-es-irodalom", "Élet és irodalom")]


@pytest.mark.parametrize("text, error", [
    ("Hungary\n", "write a section as"),
    ("Hungary: a\nhungary: b\n", "is there twice"),
    ("A name far too long to stand on the page: x\n", "a name is 24 at most"),
    ("".join(f"S{n}: x\n" for n in range(9)), "the front page holds 8"),
])
def test_a_bad_sections_file_names_the_fix(h, text, error):
    h.sections.write_text(text, encoding="utf-8")
    with pytest.raises(InvalidData) as e:
        sections.load(h)
    assert error in "\n".join(e.value.details)


def test_the_issue_starts_with_its_date_and_sections(dev, sim, h, web):  # noqa: F811
    h.sections.write_text(SECTIONS, encoding="utf-8")
    write_jsonl(h.read, [piece(1), piece(2, "world")])
    ops.sync(dev, h)
    head, *pieces = device_list(sim)
    assert head["sections"] == [{"id": "hungary", "name": "Hungary"}, {"id": "world", "name": "World"},
                                {"id": "elet-es-irodalom", "name": "Élet és irodalom"}]
    assert len(head["date"]) == 10
    assert [(p["id"], p["section"], p["head"], p["lead"]) for p in pieces] == [
        ("r-1", "hungary", "A short headline", "What the piece says, in a line or two."),
        ("r-2", "world", "A short headline", "What the piece says, in a line or two.")]
    sent = {r["id"]: r for r in map(json.loads, h.sent.read_text().splitlines())}
    assert sent["r-1"]["section"] == "hungary" and sent["r-1"]["issue"] == head["date"]


@pytest.mark.parametrize("item, error", [
    (piece(1, None), "give it a section"),
    (piece(1, "sport"), "section 'sport' is not in sections.md"),
    (piece(1, head=None), "needs head"),
    (piece(1, lead=None), "needs lead"),
    (piece(1, head="x" * 31), "head: 31 characters; it has room for 30"),
    (piece(1, lead="x" * 61), "lead: 61 characters; it has room for 60"),
])
def test_a_piece_that_does_not_fit_the_paper_is_refused(dev, sim, h, web, item, error):  # noqa: F811
    h.sections.write_text(SECTIONS, encoding="utf-8")
    write_jsonl(h.read, [{k: v for k, v in item.items() if v is not None}])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert error in "\n".join(e.value.details)


def test_a_section_holds_three(dev, sim, h, web):  # noqa: F811
    h.sections.write_text(SECTIONS, encoding="utf-8")
    write_jsonl(h.read, [piece(1), piece(3), piece(5), piece(7)])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert "4 pieces in 'hungary'; a section holds 3" in "\n".join(e.value.details)


def test_an_issue_is_fixed_for_its_day_and_saved_links_still_arrive(dev, sim, h, web):  # noqa: F811
    h.sections.write_text(SECTIONS, encoding="utf-8")
    write_jsonl(h.read, [piece(1)])
    ops.sync(dev, h)
    assert read_json(h.sync_state, {})["issue"]["reads"][0]["id"] == "r-1"
    # The same day: another piece is left out and said so; a saved link comes in, kept.
    write_jsonl(h.read, [piece(2, "world"), {"id": "r-9", "kind": "link", "url": HU_ARTICLE, "kept": True}])
    out = ops.sync(dev, h)
    assert any("today's issue went out already; left out r-2" in w for w in out["warnings"])
    _, *pieces = device_list(sim)
    assert [(p["id"], p.get("kept")) for p in pieces] == [("r-1", None), ("r-9", True)]


def test_without_sections_the_read_list_is_a_plain_list(dev, sim, h, web):  # noqa: F811
    write_jsonl(h.read, [{"id": "r-1", "kind": "news", "url": ARTICLE}])
    ops.sync(dev, h)
    assert [r["id"] for r in device_list(sim)] == ["r-1"]


def test_the_mac_leaves_the_papers_articles_for_the_x4_to_remove(dev, sim, h, web, monkeypatch):  # noqa: F811
    from datetime import date as real_date

    h.sections.write_text(SECTIONS, encoding="utf-8")
    write_jsonl(h.read, [piece(1), piece(2, "world")])
    ops.sync(dev, h)

    class Tomorrow(real_date):
        @classmethod
        def today(cls):
            return real_date.fromordinal(real_date.today().toordinal() + 1)

    monkeypatch.setattr(ops, "date", Tomorrow)
    write_jsonl(h.read, [piece(2, "world")])  # listed again, as a weekly piece is; r-1 is not
    out = ops.sync(dev, h)
    # A kept piece needs its article: only the X4 knows which those are.
    assert out["deleted"] == []
    assert sorted(p.name for p in (sim.root / "books/read").iterdir()) == ["r-1.epub", "r-2.epub"]
    head, *pieces = device_list(sim)
    assert head["date"] == Tomorrow.today().isoformat() and [p["id"] for p in pieces] == ["r-2"]


HU_QUOTE = "A kormány szerdán hirdette ki az új rendeletet"


def asking(**more):
    return piece(2, predict={"prompt": "Mikor hirdették ki?", "options": ["Szerdán", "Pénteken"], "answer": 0,
                             "quote": HU_QUOTE}) | more


def test_a_piece_carries_its_question_to_the_x4(dev, sim, h, web):  # noqa: F811
    h.sections.write_text(SECTIONS, encoding="utf-8")
    forecast = {"id": "f-1", "text": "A rendeletet módosítják.", "by": (ops.date.today() + __import__("datetime").timedelta(days=10)).isoformat()}
    write_jsonl(h.threads, [{"id": "t-1", "title": "A rendelet", "body": "Eddig."}])
    write_jsonl(h.read, [asking(thread="t-1", forecast=forecast)])
    ops.sync(dev, h)
    _, sent = device_list(sim)
    assert sent["predict"]["quote"] == HU_QUOTE and sent["thread"] == "t-1" and sent["forecast"]["id"] == "f-1"
    # The forecast is in the ledger from the day its piece went out, and the context can name
    # the question a guess belongs to.
    assert [r["id"] for r in map(json.loads, h.forecasts.read_text().splitlines())] == ["f-1"]
    assert read_json(h.sync_state, {})["predicts"]["r-2"]["prompt"] == "Mikor hirdették ki?"


@pytest.mark.parametrize("more, error", [
    ({"predict": {"prompt": "P", "options": ["a", "b"], "answer": 0, "quote": "Ez nincs a cikkben"}}, "predict.quote: not in the article"),
    ({"thread": "t-9"}, "thread: 't-9' is not in threads.jsonl"),
    ({"forecast": {"id": "f-1", "text": "T", "by": "2020-01-01"}}, "by: 2020-01-01 has passed"),
    ({"link": {"prompt": "P", "options": [{"text": "a", "ref": "h-1"}, {"text": "b", "ref": "h-2"}, {"text": "c", "ref": "h-3"}],
               "answer": 0, "why": "W"}}, "no record that 'h-1' was read"),
    ({"link": {"prompt": "P", "options": [{"text": "a", "ref": "h-1"}, {"text": "b", "ref": "h-2"}, {"text": "c", "ref": "h-3"}],
               "answer": 0, "why": "W"},
      "forecast": {"id": "f-1", "text": "T", "by": "2030-01-01"}}, "give it a link or a forecast"),
])
def test_what_a_piece_carries_is_checked(dev, sim, h, web, more, error):  # noqa: F811
    h.sections.write_text(SECTIONS, encoding="utf-8")
    write_jsonl(h.read, [asking(**more)])
    with pytest.raises(InvalidData) as e:
        ops.sync(dev, h)
    assert error in "\n".join(e.value.details)


def test_a_guess_and_its_links_answer_are_both_pulled(dev, sim, h):
    write_jsonl(sim.root / "x4/seen.jsonl", [
        {"id": "r-1", "opened": False, "predict": {"pick": 1, "correct": True, "ms": 900}, "set": 3, "at": None},
        {"id": "r-1", "opened": True, "link": {"pick": 0, "correct": True, "ms": 700}, "set": 3, "at": None},
    ])
    assert ops.pull(dev, h, "seen")["seen"]["new"] == 2
