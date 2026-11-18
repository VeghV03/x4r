"""Crosswords Claude clues on the Mac: X4_HOME/crosswords/<lang>.jsonl (docs/DATA.md, "Crosswords").

`x4 words grids` makes the grids, Claude writes the clues, `x4 sync` checks the file here and
pushes it to /x4/crosswords/<lang>.jsonl. The device plays new lines in file order, so new
puzzles are appended.
"""

import re
import unicodedata
from pathlib import Path

from .errors import InvalidData
from .models import Checked, _by_line, _lines, _unique, jsonl_bytes
from .words import ALPHABETS, SIZE, numbered, slots

RECENT = 30  # puzzles whose answers a new grid leaves out
ID_BYTES = 15  # words::ID_MAX
LINE_BYTES = 4095  # words::CROSSWORD_LINE_MAX, less the NUL
CLUE_BYTES = 96  # words::CLUE_MAX: the device cuts a longer clue
SRC_BYTES = 48  # words::SRC_MAX: the device cuts a longer source title
SOURCES = 6  # words::SOURCES_MAX: distinct sources per puzzle
SIZES = (5, SIZE)  # new grids are 7x7; 5x5 puzzles written before still play
# CrosswordGame draws a clue in NotoSans 14 regular, three lines across the frame: the 480 px
# portrait screen less max(36, 480 / 11) on each side (learn/ui/LearnUi.cpp, frame()).
CLUE_LINES = 3
CLUE_WIDTH = 480 - 2 * 43

# Advance widths of the clue font in 1/16 px (lib/EpdFont/builtinFonts/notosans_14_regular.h),
# from each range's first code point: ASCII, Latin-1 and Latin Extended-A, dashes to the ellipsis.
_ADVANCES = {
    0x0020: (
        "121 126 190 302 267 388 342 105 140 140 257 267 125 150 125 174 273 206 259 267 267 267 267 239 275 "
        "267 125 125 267 267 267 203 420 298 303 295 341 260 242 340 346 158 127 289 245 423 355 365 282 365 "
        "290 256 260 341 280 434 274 264 267 154 174 154 267 207 131 262 287 224 287 263 161 287 288 120 120 "
        "249 120 436 288 282 287 287 193 224 168 288 237 367 247 238 219 177 257 177 267 "
    ),
    0x00A0: (
        "121 126 267 267 267 267 257 239 271 388 167 238 267 150 388 233 200 267 163 163 131 291 306 125 105 "
        "163 175 238 348 360 365 203 298 298 298 298 298 298 411 295 260 260 260 260 158 158 158 158 341 355 "
        "365 365 365 365 365 267 365 341 341 341 341 264 282 295 262 262 262 262 262 262 403 224 263 263 263 "
        "263 120 120 120 120 282 288 282 282 282 282 282 267 282 288 288 288 288 238 287 238 298 262 298 262 "
        "298 262 295 224 295 224 295 224 295 224 341 287 341 288 260 263 260 263 260 263 260 263 260 263 340 "
        "287 340 287 340 287 340 287 346 288 346 288 158 120 158 120 158 120 158 120 158 120 286 241 127 120 "
        "289 249 249 245 120 245 120 245 120 245 125 245 120 355 288 355 288 355 288 321 355 288 365 282 365 "
        "282 365 282 433 442 290 193 290 193 290 193 256 224 256 224 256 224 256 224 260 168 260 168 260 168 "
        "341 288 341 288 341 288 341 288 341 288 341 288 434 367 264 238 264 267 219 267 219 267 219 152 "
    ),
    0x2010: (
        "150 150 267 233 467 467 257 192 82 82 117 82 168 168 194 168 239 239 175 172 233 250 369 "
    ),
}
ADVANCE = {
    first + i: int(w) for first, widths in _ADVANCES.items() for i, w in enumerate(widths.split())
}


def _width(text: str) -> int:
    """Pixels, kerning ignored: kerning only narrows text."""
    return -(-sum(ADVANCE[ord(ch)] for ch in text) // 16)


def clue_lines(text: str) -> int:
    """How many lines the device wraps `text` into, as learn/ui flowText does."""
    lines, line = 0, ""
    for word in text.split(" "):
        if not word:
            continue
        if _width(f"{line} {word}" if line else word) <= CLUE_WIDTH:
            line = f"{line} {word}" if line else word
            continue
        if line:
            lines += 1
        line = ""
        for ch in word:  # a word wider than a line breaks at a character
            if line and _width(line + ch) > CLUE_WIDTH:
                lines += 1
                line = ""
            line += ch
    return lines + (1 if line else 0)


def _gives_away(clue: str, answer: str) -> bool:
    """The answer is a word of the clue or starts one: `ház` in "Házak sora"."""
    return re.search(rf"(?<!\w){re.escape(answer)}", clue.casefold()) is not None


def _check_clue(where: str, clue, answer: str) -> list[str]:
    if not isinstance(clue, str) or not clue.strip():
        return [f"{where}.clue: is required; write a clue for {answer!r}"]
    clue = unicodedata.normalize("NFC", clue)
    if any(unicodedata.category(ch).startswith("C") for ch in clue):
        return [f"{where}.clue: has a line break or control character; write it on one line"]
    missing = sorted({ch for ch in clue if ord(ch) not in ADVANCE})
    if missing:
        return [f"{where}.clue: the device font has no {', '.join(map(repr, missing))}; use other characters"]
    errors = []
    if len(clue.encode()) > CLUE_BYTES:
        errors.append(f"{where}.clue: is {len(clue.encode())} bytes, the device shows {CLUE_BYTES}; shorten it")
    elif (n := clue_lines(clue)) > CLUE_LINES:
        errors.append(f"{where}.clue: takes {n} lines on the device, it shows {CLUE_LINES}; shorten it")
    if _gives_away(clue, answer):
        errors.append(f"{where}.clue: contains the answer {answer!r}; clue it without the word")
    return errors


def _check_grid(where: str, obj: dict, lang: str) -> list[str]:
    letters = set(ALPHABETS[lang])
    rows = obj.get("rows")
    n = obj.get("size")
    if n not in SIZES:
        return [f"{where}: size: must be {SIZE}"]
    if not isinstance(rows, list) or len(rows) != n or not all(isinstance(r, str) for r in rows):
        return [f"{where}: rows: must be {n} strings of {n} letters, . for a black square"]
    errors = []
    for i, row in enumerate(rows):
        if len(row) != n:
            errors.append(f"{where}: rows.{i}: {row!r} has {len(row)} characters; a row has {n}")
        bad = sorted({ch for ch in row if ch != "." and ch not in letters})
        if bad:
            errors.append(f"{where}: rows.{i}: {', '.join(map(repr, bad))} not in the {lang} alphabet; "
                          "use lowercase letters, . for a black square")  # fmt: skip
    if errors:
        return errors
    runs = slots(tuple(r.replace(".", "#") for r in rows))
    if not runs:
        return [f"{where}: rows: the grid has no words"]
    covered = {c for run in runs for c in run.cells}
    for i in range(n * n):
        if rows[i // n][i % n] != "." and i not in covered:
            errors.append(f"{where}: rows.{i // n}: cell {i % n + 1} is in no word; make it . or run a word through it")
    return errors


def _check_src(where: str, src) -> list[str]:
    if not isinstance(src, str) or not src.strip():
        return [f"{where}.src: must be the title of the book or article the word came from; leave it out otherwise"]
    src = unicodedata.normalize("NFC", src)
    if any(unicodedata.category(ch).startswith("C") for ch in src):
        return [f"{where}.src: has a line break or control character; write it on one line"]
    missing = sorted({ch for ch in src if ord(ch) not in ADVANCE})
    if missing:
        return [f"{where}.src: the device font has no {', '.join(map(repr, missing))}; use other characters"]
    if len(src.encode()) > SRC_BYTES:
        return [f"{where}.src: is {len(src.encode())} bytes, the device keeps {SRC_BYTES}; shorten the title"]
    return []


def _check_entries(where: str, obj: dict) -> list[str]:
    """Numbers as numbered(rows) gives them, each with a clue that fits and keeps its answer, and
    at most SOURCES distinct sources."""
    errors = []
    sources: list[str] = []
    for key, entries in zip(("across", "down"), numbered(obj["rows"]), strict=True):
        items = obj.get(key)
        want = [n for n, _ in entries]
        if not isinstance(items, list) or not all(isinstance(e, dict) for e in items):
            errors.append(f"{where}: {key}: must be a list of {{\"n\", \"clue\"}}, numbers {want}")
            continue
        got = [e.get("n") for e in items]
        if got != want:
            errors.append(f"{where}: {key}: numbers must be {want} (from rows), got {got}")
            continue
        for i, (item, (n, answer)) in enumerate(zip(items, entries, strict=True)):
            at = f"{where}: {key}.{i}"
            if "answer" in item and item["answer"] != answer:
                errors.append(f"{at}.answer: is {item['answer']!r}, the grid has {answer!r} at {n} {key}")
            errors += _check_clue(at, item.get("clue"), answer)
            if "src" in item:
                found = _check_src(at, item["src"])
                errors += found
                if not found and (src := unicodedata.normalize("NFC", item["src"])) not in sources:
                    sources.append(src)
    if len(sources) > SOURCES:
        errors.append(f"{where}: src: {len(sources)} different sources, the device shows {SOURCES}; "
                      "leave src out on the words of the others")  # fmt: skip
    return errors


def _device(obj: dict) -> dict:
    """The device's copy: clues and sources NFC; answers (the rows have them) and seed marks left out."""
    out = dict(obj)
    for key in ("across", "down"):
        out[key] = [{k: unicodedata.normalize("NFC", v) if k in ("clue", "src") else v for k, v in e.items()
                     if k in ("n", "clue", "src")} for e in obj[key]]  # fmt: skip
    return out


def check_crosswords(path: Path, lang: str) -> Checked:
    """Validate X4_HOME/crosswords/<lang>.jsonl. Raises InvalidData listing every problem."""
    name = str(path)
    rows, errors = _lines(name, path.read_bytes())
    id_form = re.compile(rf"{lang}-\d+")
    records = []
    for n, obj in rows:
        where = f"{name}:{n}"
        pid = obj.get("id")
        if not isinstance(pid, str) or not id_form.fullmatch(pid) or len(pid) > ID_BYTES:
            errors.append(f"{where}: id: must be like {lang}-0001, at most {ID_BYTES} bytes, got {pid!r}")
        found = _check_grid(where, obj, lang)
        if not found:
            found = _check_entries(where, obj)
        errors += found
        if found:
            continue
        record = _device(obj)
        size = len(jsonl_bytes([record])) - 1
        if size > LINE_BYTES:
            errors.append(f"{where}: is {size} bytes on the device, which reads {LINE_BYTES}; shorten the clues")
        records.append(record)
    errors += _unique(name, rows, "crossword")
    if errors:
        raise InvalidData(f"{name}: invalid crosswords", _by_line(name, errors))
    return Checked(lang, jsonl_bytes(records), len(records), records)


def _puzzles(path: Path, lang: str) -> list[dict]:
    """The file's puzzles, read leniently: check_crosswords reports the errors."""
    if not path.exists():
        return []
    rows, _ = _lines(str(path), path.read_bytes())
    return [obj for _, obj in rows if isinstance(obj.get("id"), str)]


def next_number(path: Path, lang: str) -> int:
    """The number after the highest `<lang>-<number>` id in the file."""
    form = re.compile(rf"{lang}-(\d+)")
    found = [int(m.group(1)) for p in _puzzles(path, lang) if (m := form.fullmatch(p["id"]))]
    return max(found, default=0) + 1


def recent_answers(path: Path, lang: str, count: int = RECENT) -> set[str]:
    """The answers of the last `count` puzzles in the file."""
    out: set[str] = set()
    for p in _puzzles(path, lang)[-count:]:
        rows = p.get("rows")
        if isinstance(rows, list) and rows and all(isinstance(r, str) and len(r) == len(rows) for r in rows):
            across, down = numbered(rows)
            out |= {w for _, w in across + down}
    return out
