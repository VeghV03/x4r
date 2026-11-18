import json

import pytest
from records import DECK, write_jsonl

from x4.errors import InvalidData
from x4.models import check_deck, check_glossary, check_stream

HEAD = DECK[0]


def deck_errors(tmp_path, *cards: dict, head: dict = HEAD) -> list[str]:
    path = write_jsonl(tmp_path / "d.jsonl", [head, *cards])
    with pytest.raises(InvalidData) as e:
        check_deck(path)
    return e.value.details


def test_valid_deck_is_canonicalised(tmp_path):
    path = tmp_path / "d.jsonl"
    path.write_text("﻿" + json.dumps(DECK[0], indent=None) + "\n\n" + "\n".join(json.dumps(c) for c in DECK[1:]))
    c = check_deck(path)
    assert c.id == "meditations" and c.count == 2
    assert c.body.endswith(b"\n") and b"\n\n" not in c.body and b": " not in c.body


@pytest.mark.parametrize(
    "card,message",
    [
        ({"id": "c", "type": "flip", "front": "a"}, "d.jsonl:2: back: is required; add it"),
        ({"id": "c", "type": "quiz"}, "d.jsonl:2: type: must be one of 'flip', 'choice', 'cloze', 'explain', got 'quiz'"),
        ({"id": "c", "front": "a", "back": "b"}, "d.jsonl:2: type: is required; set it"),
        ({"id": "c" * 33, "type": "flip", "front": "a", "back": "b"}, "d.jsonl:2: id: is 33 bytes, the limit is 32; shorten it"),
        ({"id": "c", "type": "flip", "front": "é" * 501, "back": "b"}, "d.jsonl:2: front: is 1002 bytes, the limit is 1000; shorten it"),
        ({"id": "c", "type": "flip", "front": " ", "back": "b"}, "d.jsonl:2: front: is empty; write the text"),
        (
            {"id": "c", "type": "choice", "prompt": "p", "options": ["a"], "answer": 0},
            "d.jsonl:2: options: must have 2 to 4 items, got 1",
        ),
        (
            {"id": "c", "type": "choice", "prompt": "p", "options": ["a", "b"], "answer": 2},
            "d.jsonl:2: answer: must be the 0-based index of the right option (0 to 1), got 2",
        ),
        (
            {"id": "c", "type": "choice", "prompt": "p", "options": ["a", "b"], "answer": "1"},
            "d.jsonl:2: answer: must be an integer",
        ),
        (
            {"id": "c", "type": "cloze", "text": "The {{a}} and {{b}}", "options": ["a", "b"]},
            "d.jsonl:2: text: must contain exactly one {{term}}, found 2",
        ),
        (
            {"id": "c", "type": "cloze", "text": "The {{way}}", "options": ["path", "road"]},
            "d.jsonl:2: options: must include the cloze term 'way'",
        ),
        (
            {"id": "c", "type": "explain", "prompt": "p", "points": ["1", "2", "3", "4", "5", "6"]},
            "d.jsonl:2: points: must have 1 to 5 items, got 6",
        ),
    ],
)
def test_card_rules(tmp_path, card, message):
    assert [e.removeprefix(str(tmp_path) + "/") for e in deck_errors(tmp_path, card)] == [message]


def test_header_and_duplicates(tmp_path):
    flip = {"id": "c1", "type": "flip", "front": "a", "back": "b"}
    errors = deck_errors(tmp_path, flip, flip, head={"deck": "my deck"})
    assert errors[0].endswith(":1: deck: must be a non-empty string without spaces")
    assert errors[1].endswith(":3: id: 'c1' is already used on line 2; card ids must be unique")
    errors = deck_errors(tmp_path, head=flip)
    assert 'line 1 must be the header {"deck":"<id>"' in errors[0]


def test_bad_json_line_named(tmp_path):
    path = tmp_path / "d.jsonl"
    path.write_text(json.dumps(HEAD) + "\n{oops\n")
    with pytest.raises(InvalidData) as e:
        check_deck(path)
    assert e.value.details[0].startswith(f"{path}:2: not valid JSON")
    assert e.value.exit_code == 2


def test_many_errors_are_capped(tmp_path):
    cards = [{"id": f"c{i}", "type": "flip"} for i in range(40)]
    errors = deck_errors(tmp_path, *cards)
    assert len(errors) == 31 and errors[-1].startswith("... and")


def test_stream_rules(tmp_path):
    path = write_jsonl(
        tmp_path / "s.jsonl",
        [
            {"id": "s1", "kind": "bite", "title": "t", "body": "x" * 4001},
            {"id": "s1", "kind": "highlight"},
        ],
    )
    with pytest.raises(InvalidData) as e:
        check_stream(path)
    d = [x.removeprefix(f"{path}:") for x in e.value.details]
    assert d == [
        "1: body: is 4001 bytes, the limit is 4000; shorten it",
        "2: highlight: is required; add the id of a pulled highlight",
        "2: id: 's1' is already used on line 1; stream item ids must be unique",
    ]


@pytest.mark.parametrize("kind", ["letter", "brief", "article", "game", "forecast", "outcome", "test"])
def test_the_sets_kinds_are_refused(tmp_path, kind):
    path = write_jsonl(tmp_path / "s.jsonl", [{"id": "s1", "kind": kind, "title": "T", "body": "B"}])
    with pytest.raises(InvalidData) as e:
        check_stream(path)
    assert "kind" in e.value.details[0]


def _piece(**extra) -> dict:
    return {"id": "r-1", "kind": "news", "url": "https://a.example/1"} | extra


def test_a_piece_may_ask_a_question_first():
    from x4.models import ReadSource

    predict = {"prompt": "P", "options": ["a", "b"], "answer": 1, "quote": "Q"}
    assert ReadSource.model_validate(_piece(predict=predict)).predict.quote == "Q"
    for bad in ({"answer": 2}, {"options": ["a"]}, {"options": ["a", "a"]}, {"quote": ""}):
        with pytest.raises(ValueError):
            ReadSource.model_validate(_piece(predict=predict | bad))


def test_a_piece_may_ask_which_earlier_reading_connects():
    from x4.models import ReadSource

    options = [{"text": t, "ref": f"h-{t}"} for t in "abc"]
    link = {"prompt": "P", "options": options, "answer": 2, "why": "W"}
    assert ReadSource.model_validate(_piece(link=link)).link.answer == 2
    for bad in ({"answer": 3}, {"options": options[:2]}, {"options": [options[0], options[0], options[1]]}, {"why": ""}, {"prompt": ""}):
        with pytest.raises(ValueError):
            ReadSource.model_validate(_piece(link=link | bad))


def test_empty_stream_is_valid(tmp_path):
    path = tmp_path / "s.jsonl"
    path.write_text("")
    assert check_stream(path).count == 0


def test_glossary_rules(tmp_path):
    path = tmp_path / "g.json"
    path.write_text(json.dumps({"logos": "Reason ordering the world.", " bad": "x", "empty": ""}))
    with pytest.raises(InvalidData) as e:
        check_glossary(path)
    assert e.value.details == [
        f"{path}: ' bad': term must be non-empty, on one line, without surrounding spaces",
        f"{path}: 'empty': definition must be a non-empty string",
    ]
    path.write_text("[1]")
    with pytest.raises(InvalidData):
        check_glossary(path)


def test_bite_may_name_the_question_it_answers(tmp_path):
    bite = {"id": "s1", "kind": "bite", "title": "T", "body": "B", "highlight": "q1"}
    assert check_stream(write_jsonl(tmp_path / "s.jsonl", [bite])).count == 1
    with pytest.raises(InvalidData) as e:
        check_stream(write_jsonl(tmp_path / "s.jsonl", [bite | {"highlight": "q 1"}]))
    assert e.value.details[0].endswith(":1: highlight: must be a non-empty string without spaces")


def test_highlight_items_are_filled_from_pulled_highlights(tmp_path):
    from x4.models import Highlight

    known = {"h1": Highlight(id="h1", book="books/meditations.epub", title="Meditations", text="Power over your mind.")}
    items = [
        {"id": "s1", "kind": "highlight", "highlight": "h1"},
        {"id": "s2", "kind": "highlight", "highlight": "h9", "text": "Inline.", "title": "Other"},
    ]
    c = check_stream(write_jsonl(tmp_path / "s.jsonl", items), highlights=known)
    assert c.records[0] == items[0] | {"text": "Power over your mind.", "title": "Meditations"}
    assert c.records[1] == items[1]
    assert b'"text":"Power over your mind."' in c.body


def test_unknown_highlight_without_text(tmp_path):
    path = write_jsonl(tmp_path / "s.jsonl", [{"id": "s1", "kind": "highlight", "highlight": "h9"}])
    with pytest.raises(InvalidData) as e:
        check_stream(path, highlights={})
    assert e.value.details == [
        f"{path}:1: highlight: 'h9' is not in the pulled highlights; "
        "use an id from x4 context, run x4 pull, or give text and title inline"
    ]


def stream_errors(tmp_path, *items: dict) -> list[str]:
    path = write_jsonl(tmp_path / "s.jsonl", list(items))
    with pytest.raises(InvalidData) as e:
        check_stream(path)
    return [x.removeprefix(f"{path}:") for x in e.value.details]


@pytest.mark.parametrize(
    "item,message",
    [
        ({"id": "s", "kind": "riddle", "prompt": "p"}, "1: answer: is required; add it"),
        ({"id": "s", "kind": "order", "prompt": "p", "items": ["a", "b"]}, "1: items: must have 3 to 6 items, got 2"),
        (
            {"id": "s", "kind": "order", "prompt": "p", "items": ["a", "b", "a"]},
            "1: items: has the same text twice; make every entry different",
        ),
        (
            {"id": "s", "kind": "odd", "prompt": "p", "options": ["a", "b", "c"], "answer": 3, "why": "w"},
            "1: answer: must be the 0-based index of the right option (0 to 2), got 3",
        ),
        ({"id": "s", "kind": "odd", "prompt": "p", "options": ["a", "b", "c"], "answer": 0}, "1: why: is required; add it"),
        (
            {"id": "s", "kind": "logic", "prompt": "p", "options": ["a", "b", "c", "d", "e"], "answer": 0, "why": "w"},
            "1: options: must have 2 to 4 items, got 5",
        ),
    ],
)
def test_v2_stream_rules(tmp_path, item, message):
    assert stream_errors(tmp_path, item) == [message]


def test_v2_stream_kinds_valid(tmp_path):
    items = [
        {"id": "s3", "kind": "riddle", "prompt": "p", "answer": "a", "book": "books/a.epub"},
        {"id": "s4", "kind": "order", "prompt": "p", "items": ["a", "b", "c"]},
        {"id": "s5", "kind": "odd", "prompt": "p", "options": ["a", "b", "c"], "answer": 2, "why": "w"},
        {"id": "s6", "kind": "logic", "prompt": "p", "options": ["a", "b"], "answer": 1, "why": "w"},
    ]
    assert check_stream(write_jsonl(tmp_path / "s.jsonl", items)).count == 4


def test_read_list_rules(tmp_path):
    from x4.models import check_read

    path = write_jsonl(
        tmp_path / "read.jsonl",
        [
            {"id": "x1", "kind": "news", "url": "https://a.example/1"},
            {"id": "r-2", "kind": "blog", "url": "https://a.example/2"},
            {"id": "r-3", "kind": "link"},
            {"id": "r-4", "kind": "found", "url": "ftp://a.example/4"},
            {"id": "r-4", "kind": "found", "url": "https://a.example/5", "minutes": 0},
            {"id": "r/6", "kind": "found", "file": "/tmp/a.html"},
        ],
    )
    with pytest.raises(InvalidData) as e:
        check_read(path)
    assert [x.removeprefix(f"{path}:") for x in e.value.details] == [
        "1: id: 'x1' must start with 'r-'",
        "2: kind: must be one of 'news', 'follow', 'found', 'link', 'translation', 'dossier', 'field', 'film' or 'analysis', got 'blog'",
        "3: url: give either url (http or https) or file (a local path), not both",
        "4: url: 'ftp://a.example/4' must start with http:// or https://",
        "5: minutes: must be 1 or more; leave it out and x4 sync estimates it",
        "5: id: 'r-4' is already used on line 4; Read list ids must be unique",
        "6: id: 'r/6' is used as a file name; use only letters, digits, '.', '_' and '-'",
    ]
    ok = write_jsonl(tmp_path / "ok.jsonl", [{"id": "r-1", "kind": "link", "url": "https://a.example/"}])
    assert [i.id for i in check_read(ok)] == ["r-1"]


def test_a_card_too_long_for_the_device_line_is_refused(tmp_path):
    q = '"' * 1000  # within the field limit; each quote takes 2 bytes in JSON
    card = {"id": "c", "type": "choice", "prompt": q, "options": [q, q[1:] + "a", q[1:] + "b", q[1:] + "c"], "answer": 0}
    [error] = deck_errors(tmp_path, card)
    assert "d.jsonl:2: is 10" in error and "skips lines over 8191" in error


def test_a_stream_longer_than_the_device_reads_is_refused(tmp_path):
    items = [{"id": f"s{i}", "kind": "bite", "title": "t", "body": "b"} for i in range(513)]
    with pytest.raises(InvalidData) as e:
        check_stream(write_jsonl(tmp_path / "s.jsonl", items))
    assert e.value.details == [f"{tmp_path}/s.jsonl: has 513 items, the X4 reads the first 512; drop seen ones"]
