"""Record builders shared by the tests."""

import json
from pathlib import Path

from x4 import days

TODAY = days.today()


def write_jsonl(path: Path, records: list[dict]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(r) + "\n" for r in records))
    return path


def highlight(i: str, **kw) -> dict:
    base = {
        "id": i,
        "kind": "highlight",
        "book": "books/meditations.epub",
        "title": "Meditations",
        "author": "Marcus Aurelius",
        "chapter": "Book II",
        "text": f"text {i}",
        "pos": {"spine": 3, "start": 100, "end": 120},
        "day": TODAY - 2,
        "at": None,
    }
    return base | kw


def step_highlight(i: str, item: str, book: str | None = None, **kw) -> dict:
    """A highlight made in the daily set, as the device writes it."""
    base = {
        "id": i,
        "kind": "highlight",
        "book": book or f"stream/{item}",
        "item": {"id": item, "kind": "bite", "title": f"Title {item}"},
        "text": f"text {i}",
        "pos": {"start": 4, "end": 20},
        "day": TODAY - 2,
        "at": None,
    }
    if not book:
        base["title"] = f"Title {item}"
    return base | kw


DECK = [
    {"deck": "meditations", "title": "Meditations", "book": "books/meditations.epub"},
    {"id": "c1", "type": "flip", "front": "Inner citadel?", "back": "The mind no event can reach."},
    {"id": "c2", "type": "choice", "prompt": "In our control?", "options": ["Fame", "Judgement"], "answer": 1},
]
