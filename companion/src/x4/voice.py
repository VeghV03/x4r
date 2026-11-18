"""The voice's mechanical half: what X4_HOME/voice.md says `x4` refuses in Hungarian texts.

voice.md is the guide Claude writes by (SPEC "Writing", D29). Under its `## x4 refuses` heading
each line is `` - `<text>` <what to write instead> ``. `x4 sync` and `x4 stage` refuse an issue
whose Hungarian texts contain one: the stream, threads, recaps, crossword clues, a piece's
question, link and forecast, and the Markdown articles it lists with `lang: hu`. Matching
ignores case; a line's leading spaces are skipped, so a Markdown list's `-` is no dash.
"""

import json
import re
from pathlib import Path

from .home import Home

HEADING = "## x4 refuses"
RULE = re.compile(r"^- `([^`]+)`\s*(.*)$")
# Fields that are names, keys or other people's words, never Claude's Hungarian.
SKIP = {"id", "kind", "book", "highlight", "ref", "url", "file", "lang", "source", "by", "quote"}
AROUND = 20  # characters shown either side of what was refused


def rules(h: Home) -> list[tuple[str, str]]:
    """(text, what to write instead) from voice.md; none without the file or the heading."""
    path = h.root / "voice.md"
    if not path.exists():
        return []
    out, inside = [], False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            inside = line.strip() == HEADING
        elif inside and (m := RULE.match(line.strip())):
            out.append((m.group(1), m.group(2).strip()))
    return out


def _strings(obj, path: str = ""):
    """(field path, text) of every string under obj, leaving SKIP fields out."""
    if isinstance(obj, str):
        yield path, obj
    elif isinstance(obj, dict):
        for k, v in obj.items():
            if k not in SKIP:
                yield from _strings(v, f"{path}.{k}" if path else k)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            yield from _strings(v, f"{path}.{i}")


def _find(text: str, refused: list[tuple[str, str]]) -> list[str]:
    found = []
    for line in text.splitlines():
        line = line.lstrip()
        low = line.lower()
        for lit, instead in refused:
            # A refused text starting with a space matches after a word, not at a line's start.
            at = low.find(lit.lower())
            if at >= 0:
                clip = line[max(0, at - AROUND): at + len(lit) + AROUND]
                found.append(f"'{lit}' in '…{clip}…': {instead}")
    return found


def _jsonl(path: Path, refused, pick=lambda obj: obj, skip=lambda obj: False) -> list[str]:
    errors = []
    if not path.exists():
        return errors
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            obj = json.loads(line)
        except ValueError:
            continue  # the file's own check reports it
        if not isinstance(obj, dict) or skip(obj):
            continue
        for field, text in _strings(pick(obj)):
            errors += [f"{path}:{n}: {field}: {e} (voice.md)" for e in _find(text, refused)]
    return errors


def _markdown(h: Home, rel: str, refused) -> list[str]:
    path = Path(rel).expanduser()
    path = path if path.is_absolute() else h.root / path  # as readlist resolves a piece's file
    if not path.exists():
        return []
    text = path.read_text(encoding="utf-8")
    head, body = "", text
    if text.startswith("---"):
        _, head, body = (text.split("---", 2) + ["", ""])[:3]
    if not re.search(r"^lang:\s*hu\s*$", head, re.M):
        return []
    lines = text[: len(text) - len(body)].count("\n")
    errors = []
    for n, line in enumerate(body.splitlines(), lines + 1):
        errors += [f"{path}:{n}: {e} (voice.md)" for e in _find(line, refused)]
    return errors


def check(h: Home) -> list[str]:
    """What voice.md refuses in the Hungarian texts the next issue sends."""
    refused = rules(h)
    if not refused:
        return []
    errors = _jsonl(h.stream, refused, skip=lambda o: o.get("kind") == "highlight")
    errors += _jsonl(h.threads, refused)
    errors += _jsonl(h.previously, refused)
    errors += _jsonl(h.crosswords / "hu.jsonl", refused, pick=lambda o: {
        "clue": [e.get("clue") for d in ("across", "down") for e in o.get(d, []) if isinstance(e, dict)]
    })
    errors += _jsonl(h.read, refused, pick=lambda o: {
        "predict": {k: v for k, v in (o.get("predict") or {}).items() if k in ("prompt", "options")},
        "link": {k: v for k, v in (o.get("link") or {}).items() if k in ("prompt", "why")},
        "forecast": {k: v for k, v in (o.get("forecast") or {}).items() if k == "text"},
    })
    if h.read.exists():
        for line in h.read.read_text(encoding="utf-8").splitlines():
            try:
                rel = json.loads(line).get("file")
            except (ValueError, AttributeError):
                continue
            if isinstance(rel, str):
                errors += _markdown(h, rel, refused)
    return errors
