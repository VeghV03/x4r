"""Records from docs/DATA.md, their limits, and validation that tells an agent what to fix.

Host-written files (decks, stream, Read list, glossaries) are validated strictly before upload.
Device-written records are parsed leniently: unknown fields are ignored everywhere.
"""

import json
import re
from collections.abc import Mapping
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import Annotated, Literal

from pydantic import (
    AfterValidator,
    BaseModel,
    ConfigDict,
    Field,
    TypeAdapter,
    ValidationError,
    model_validator,
)

from .errors import InvalidData

ID_BYTES = 32
TEXT_BYTES = 1000
BODY_BYTES = 4000
MAX_CARDS = 1000
ITEM_BYTES = 200  # puzzle items and options
LINE_BYTES = 8191  # JSONL_LINE_MAX less the newline: the X4 skips longer lines
STREAM_LINES = 512  # STREAM_SCAN_MAX: the X4 reads no further
TERM_BYTES = 255  # StarDict headwords are at most 255 bytes plus NUL
CLOZE = re.compile(r"\{\{([^{}]*)\}\}")
DECK_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


def _size(n: int):
    def check(v: str) -> str:
        b = len(v.encode())
        if b > n:
            raise ValueError(f"is {b} bytes, the limit is {n}; shorten it")
        return v

    return check


def _id(v: str) -> str:
    if not v or v != v.strip() or any(c.isspace() for c in v):
        raise ValueError("must be a non-empty string without spaces")
    return _size(ID_BYTES)(v)


def _deck_id(v: str) -> str:
    _id(v)
    if not DECK_ID.fullmatch(v):
        raise ValueError(f"'{v}' is used as a file name; use only letters, digits, '.', '_' and '-'")
    return v


def _nonempty(v: str) -> str:
    if not v.strip():
        raise ValueError("is empty; write the text")
    return v


def _read_id(v: str) -> str:
    _deck_id(v)
    if not v.startswith("r-"):
        raise ValueError(f"'{v}' must start with 'r-'")
    return v


Id = Annotated[str, AfterValidator(_id)]
ReadId = Annotated[str, AfterValidator(_read_id)]
Text = Annotated[str, AfterValidator(_nonempty), AfterValidator(_size(TEXT_BYTES))]
Body = Annotated[str, AfterValidator(_nonempty), AfterValidator(_size(BODY_BYTES))]
Item = Annotated[str, AfterValidator(_nonempty), AfterValidator(_size(ITEM_BYTES))]


def _count(name: str, items: list, lo: int, hi: int) -> None:
    if not lo <= len(items) <= hi:
        raise ValueError(f"{name}: must have {lo} to {hi} items, got {len(items)}")


def _distinct(name: str, items: list[str]) -> None:
    if len(set(items)) != len(items):
        raise ValueError(f"{name}: has the same text twice; make every entry different")


def _index(answer: int, options: list) -> None:
    if not 0 <= answer < len(options):
        raise ValueError(
            f"answer: must be the 0-based index of the right option (0 to {len(options) - 1}), got {answer}"
        )


class Strict(BaseModel):
    model_config = ConfigDict(strict=True, extra="ignore")


# Decks


class DeckHeader(Strict):
    deck: Annotated[str, AfterValidator(_deck_id)]
    title: Text | None = None
    book: str | None = None


class CardBase(Strict):
    id: Id
    highlight: Id | None = None
    why: Text | None = None


class Flip(CardBase):
    type: Literal["flip"]
    front: Text
    back: Text


class Choice(CardBase):
    type: Literal["choice"]
    prompt: Text
    options: list[Text]
    answer: int

    @model_validator(mode="after")
    def _check(self):
        _count("options", self.options, 2, 4)
        _index(self.answer, self.options)
        return self


class Cloze(CardBase):
    type: Literal["cloze"]
    text: Text
    options: list[Text]

    @model_validator(mode="after")
    def _check(self):
        terms = CLOZE.findall(self.text)
        if len(terms) != 1:
            raise ValueError(f"text: must contain exactly one {{{{term}}}}, found {len(terms)}")
        _count("options", self.options, 2, 4)
        if terms[0] not in self.options:
            raise ValueError(f"options: must include the cloze term '{terms[0]}'")
        return self

    @property
    def term(self) -> str:
        return CLOZE.findall(self.text)[0]


class Explain(CardBase):
    type: Literal["explain"]
    prompt: Text
    points: list[Text]

    @model_validator(mode="after")
    def _check(self):
        _count("points", self.points, 1, 5)
        return self


Card = Annotated[Flip | Choice | Cloze | Explain, Field(discriminator="type")]
CARD = TypeAdapter(Card)


# Stream


class StreamBase(Strict):
    id: Id


class Bite(StreamBase):
    kind: Literal["bite"]
    title: Text
    body: Body
    book: str | None = None
    highlight: Id | None = None  # the question this bite answers, if any


class HighlightItem(StreamBase):
    kind: Literal["highlight"]
    highlight: Id
    text: Body  # copied from the pulled highlight by check_stream
    title: Text  # the book's title


class Predict(Strict):
    """A question the article answers, asked before it. `quote` is the article's sentence that
    settles it: check_local_stream refuses one that is not in the article."""

    prompt: Text
    options: list[Item]
    answer: int
    quote: Text

    @model_validator(mode="after")
    def _check(self):
        _count("options", self.options, 2, 4)
        _distinct("options", self.options)
        _index(self.answer, self.options)
        return self


class LinkOption(Strict):
    text: Item
    ref: str  # a highlight id, a finished Read article's id or a book key (links.py checks it)


class Link(Strict):
    """A question after the article: which of three things read earlier connects to it, in the
    way `prompt` names."""

    prompt: Text
    options: list[LinkOption]
    answer: int
    why: Text

    @model_validator(mode="after")
    def _check(self):
        _count("options", self.options, 3, 3)
        _distinct("options", [o.text for o in self.options])
        _index(self.answer, self.options)
        return self


THREADS_MAX = 32
THREAD_BYTES = 600  # one screen in the reader font, as a book's recap


class Thread(Strict):
    """A line of threads.jsonl: a running story so far. Other fields are the host's own."""

    id: Id
    title: Item
    body: Text

    @model_validator(mode="after")
    def _check(self):
        size = len(self.body.encode())
        if size > THREAD_BYTES:
            raise ValueError(f"body is {size} bytes; one screen holds {THREAD_BYTES}")
        if "\n" in self.body:
            raise ValueError("body must be one paragraph, without line breaks")
        return self


class Puzzle(StreamBase):
    prompt: Body
    book: str | None = None


class Riddle(Puzzle):
    kind: Literal["riddle"]
    answer: Text


class Order(Puzzle):
    kind: Literal["order"]
    items: list[Item]  # in the correct order
    why: Text | None = None

    @model_validator(mode="after")
    def _check(self):
        _count("items", self.items, 3, 6)
        _distinct("items", self.items)
        return self


class Odd(Puzzle):
    kind: Literal["odd"]
    options: list[Item]
    answer: int
    why: Text

    @model_validator(mode="after")
    def _check(self):
        _count("options", self.options, 3, 5)
        _distinct("options", self.options)
        _index(self.answer, self.options)
        return self


class Logic(Puzzle):
    kind: Literal["logic"]
    options: list[Item]
    answer: int
    why: Text

    @model_validator(mode="after")
    def _check(self):
        _count("options", self.options, 2, 4)
        _distinct("options", self.options)
        _index(self.answer, self.options)
        return self


PUZZLES = ("riddle", "order", "odd", "logic")
StreamItem = Annotated[
    Bite | HighlightItem | Riddle | Order | Odd | Logic,
    Field(discriminator="kind"),
]
STREAM_ITEM = TypeAdapter(StreamItem)


# Read list


READ_KINDS = ("news", "follow", "found", "link", "translation", "dossier", "field", "film", "analysis")
HEAD_CHARS = 30  # one line on the front page
LEAD_CHARS = 60  # two lines under a headline


class PieceForecast(Strict):
    """The forecast a piece carries: a statement about what happens next in its story, asked
    once the piece was read."""

    id: Id
    text: Text
    by: str

    @model_validator(mode="after")
    def _check(self):
        try:
            date.fromisoformat(self.by)
        except ValueError:
            raise ValueError(f"by must be a day as YYYY-MM-DD, got {self.by!r}") from None
        if len(self.by) != 10:
            raise ValueError(f"by must be a day as YYYY-MM-DD, got {self.by!r}")
        return self


class Film(Strict):
    """What a Film pick is, so `x4` can hold it against X4_HOME/seen.md: its original title,
    the year it came out, and for a series the season picked."""

    title: Text
    year: int = Field(ge=1880, le=2100)
    season: int | None = Field(default=None, ge=1)


class ReadSource(Strict):
    """A line of ~/x4/read.jsonl as Claude writes it. `x4 sync` fills in the rest."""

    id: ReadId
    kind: Literal[READ_KINDS]
    url: str | None = None
    file: str | None = None
    title: Text | None = None
    source: Text | None = None
    lang: str | None = None
    minutes: int | None = None
    # The paper (sections.py): where the piece stands, its headline for the front page and
    # what it says, both in the piece's language. `kept`: a saved link, in no section.
    section: str | None = None
    head: Text | None = None
    lead: Text | None = None
    kept: bool | None = None
    # What the piece carries (SPEC: A piece): the running story it belongs to, a question asked
    # before it, and after it a link or a forecast, never both.
    thread: Id | None = None
    predict: Predict | None = None
    link: Link | None = None
    forecast: PieceForecast | None = None
    film: Film | None = None  # a Film pick (kind film): what it is, for seen.md

    @model_validator(mode="after")
    def _check(self):
        if (self.kind == "film") != (self.film is not None):
            raise ValueError("film: a film pick (kind film) gives film: {\"title\": its original title, \"year\": …, \"season\": … for a series}; no other kind has it")
        if self.link is not None and self.forecast is not None:
            raise ValueError("link, forecast: one thing follows a piece beside its answer; give it a link or a forecast")
        for field, limit in (("head", HEAD_CHARS), ("lead", LEAD_CHARS)):
            value = getattr(self, field)
            if value is not None and len(value) > limit:
                raise ValueError(f"{field}: {len(value)} characters; it has room for {limit}")
        if (self.url is None) == (self.file is None):
            raise ValueError("url: give either url (http or https) or file (a local path), not both")
        if self.url is not None and not self.url.startswith(("http://", "https://")):
            raise ValueError(f"url: '{self.url}' must start with http:// or https://")
        if self.minutes is not None and self.minutes < 1:
            raise ValueError("minutes: must be 1 or more; leave it out and x4 sync estimates it")
        return self


# Device-written (lenient)


class Lenient(BaseModel):
    model_config = ConfigDict(extra="ignore")


class Pos(Lenient):
    spine: int = 0
    start: int = 0
    end: int = 0


class ItemRef(Lenient):
    """The step a highlight was made on: a bite, a puzzle or a card (as `<deck>/<card id>`)."""

    id: str
    kind: str | None = None
    title: str | None = None


STEP_BOOK = "stream/"  # book key prefix of a highlight made on a step without a book


class Highlight(Lenient):
    """A highlights.jsonl record. `deleted` marks a deletion record: only id, day and at.
    `item` is set for a highlight made on a step; its `book` is the step's book, or
    `stream/<item id>` when it has none."""

    id: str
    deleted: bool = False
    kind: Literal["highlight", "question"] = "highlight"
    book: str = ""
    title: str | None = None
    author: str | None = None
    chapter: str | None = None
    text: str = ""
    pos: Pos = Pos()
    item: ItemRef | None = None
    set: int | None = None  # before the day (v9 step 7)
    day: int | None = None
    at: int | None = None

    @property
    def section(self) -> str | None:
        """The heading it goes under in its book's notes: the chapter, or the step it was made on."""
        if self.chapter:
            return self.chapter
        return self.item.title if self.item and not self.book.startswith(STEP_BOOK) else None


class Review(Lenient):
    card: str
    type: str | None = None
    grade: int
    correct: bool | None = None
    pick: int | None = None  # choice and cloze: the option chosen
    ms: int | None = None
    set: int | None = None  # before the day (v9 step 7)
    day: int | None = None
    at: int | None = None

    @property
    def passed(self) -> bool:
        return self.correct if self.correct is not None else self.grade > 1


class BookStats(Lenient):
    read_ms: int = 0
    pages: int = 0
    progress: float = 0.0


class GameStats(Lenient):
    level: int = 1
    played: int = 0
    won: int = 0


class Stats(Lenient):
    day: int = 0
    retention: float | None = None
    cards: int = 0
    due: int = 0
    books: dict[str, BookStats] = {}
    play: dict[str, GameStats] = {}


class Seen(Lenient):
    id: str
    correct: bool | None = None  # book puzzles only
    pick: int | None = None  # odd and logic: the option chosen
    opened: bool | None = None  # articles: gone into; with a predict, also a page turned
    predict: dict | None = None  # an article's guess: pick, correct, ms
    link: dict | None = None  # its link's answer: pick, correct, ms
    p: int | None = None  # a forecast: the row picked, in percent
    ms: int | None = None
    set: int | None = None  # before the day (v9 step 7)
    day: int | None = None
    at: int | None = None


class Play(Lenient):
    game: str
    mode: str = "daily"
    level: int | None = None
    won: bool = False
    lang: str | None = None
    score: int | None = None
    tries: int | None = None
    ms: int | None = None
    set: int | None = None  # before the day (v9 step 7)
    day: int | None = None
    at: int | None = None


class ReadLog(Lenient):
    book: str
    progress: float = 0.0
    finished: bool = False
    ms: int | None = None
    set: int | None = None  # before the day (v9 step 7)
    day: int | None = None
    at: int | None = None


class Lookup(Lenient):
    word: str
    book: str
    title: str | None = None
    set: int | None = None  # before the day (v9 step 7)
    day: int | None = None
    at: int | None = None


class KeptRecord(Lenient):
    """A hold of Confirm on a piece: kept, or let go."""

    id: str
    kept: bool = True
    set: int | None = None  # before the day (v9 step 7)
    day: int | None = None
    at: int | None = None


class Visit(Lenient):
    """A stay on a section's screen, or Play's, up to the last press on it (D24)."""

    section: str
    issue: str | None = None
    ms: int = 0
    day: int | None = None
    at: int | None = None


DEVICE_RECORDS: dict[str, type[Lenient]] = {
    "highlights": Highlight,
    "reviews": Review,
    "seen": Seen,
    "play": Play,
    "read-log": ReadLog,
    "lookups": Lookup,
    "kept": KeptRecord,
    "visits": Visit,
}


# Validation of whole files


@dataclass(frozen=True)
class Checked:
    """A validated host file: its id, canonical bytes to upload, and record count."""

    id: str
    body: bytes
    count: int
    records: list[dict]


TAGS = ("flip", "choice", "cloze", "explain", "bite", "highlight") + PUZZLES


def _describe(err) -> tuple[str, str]:
    loc = [str(p) for p in err["loc"]]
    kind = err["type"]
    if loc and loc[0] in TAGS:
        loc = loc[1:]  # discriminator tag pydantic puts first
    field = ".".join(loc)
    if kind == "missing":
        return field, "is required; add it"
    if kind == "union_tag_invalid":
        tags = err["ctx"]["expected_tags"]
        return field or err["ctx"]["discriminator"].strip("'"), f"must be one of {tags}, got {err['ctx']['tag']!r}"
    if kind == "union_tag_not_found":
        return err["ctx"]["discriminator"].strip("'"), "is required; set it"
    if kind == "value_error":
        msg = str(err["ctx"]["error"])
        if not field and ": " in msg:  # model validators name their own field
            field, msg = msg.split(": ", 1)
        return field, msg
    if kind == "string_type":
        return field, "must be a string"
    if kind in ("int_type", "int_parsing"):
        return field, "must be an integer"
    if kind == "list_type":
        return field, "must be a list"
    if kind == "bool_type":
        return field, "must be true or false"
    if kind == "literal_error":
        return field, f"must be one of {err['ctx']['expected']}, got {err['input']!r}"
    return field, err["msg"]


def _errors(name: str, line: int | None, exc: ValidationError) -> list[str]:
    where = f"{name}:{line}" if line else name
    out = []
    for err in exc.errors():
        field, msg = _describe(err)
        out.append(f"{where}: {field}: {msg}" if field else f"{where}: {msg}")
    return out


def _lines(name: str, data: bytes) -> tuple[list[tuple[int, dict]], list[str]]:
    rows, errors = [], []
    try:
        text = data.decode("utf-8").removeprefix("﻿")
    except UnicodeDecodeError as e:
        return [], [f"{name}: not UTF-8 (byte {e.start}); save it as UTF-8"]
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError as e:
            errors.append(f"{name}:{n}: not valid JSON ({e.msg}, column {e.colno}); each line must be one JSON object")
            continue
        if not isinstance(obj, dict):
            errors.append(f"{name}:{n}: must be a JSON object, got {type(obj).__name__}")
            continue
        rows.append((n, obj))
    return rows, errors


def _by_line(name: str, errors: list[str]) -> list[str]:
    def line(e: str) -> int:
        head = e[len(name) + 1 :].split(":", 1)[0]
        return int(head) if head.isdigit() else 1 << 30

    return sorted(errors, key=line)


def jsonl_bytes(records: list[dict]) -> bytes:
    """Canonical upload form: compact JSON, one record per line."""
    return "".join(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n" for r in records).encode()


def _too_long(name: str, rows: list[tuple[int, dict]]) -> list[str]:
    errors = []
    for n, obj in rows:
        size = len(jsonl_bytes([obj])) - 1
        if size > LINE_BYTES:
            errors.append(f"{name}:{n}: is {size} bytes on the device, which skips lines over {LINE_BYTES}; shorten it")
    return errors


def _unique(name: str, rows: list[tuple[int, dict]], what: str) -> list[str]:
    seen: dict[str, int] = {}
    errors = []
    for n, obj in rows:
        i = obj.get("id")
        if not isinstance(i, str):
            continue
        if i in seen:
            errors.append(f"{name}:{n}: id: '{i}' is already used on line {seen[i]}; {what} ids must be unique")
        else:
            seen[i] = n
    return errors


def check_deck(path: Path, data: bytes | None = None) -> Checked:
    """Validate a deck file. Raises InvalidData listing every problem."""
    name = str(path)
    rows, errors = _lines(name, path.read_bytes() if data is None else data)
    if not rows and not errors:
        raise InvalidData(f"{name}: invalid deck", [f'{name}: is empty; line 1 must be the header {{"deck":"<id>"}}'])
    deck_id = ""
    if rows:
        n, head = rows[0]
        if "deck" not in head:
            errors.append(f'{name}:{n}: line 1 must be the header {{"deck":"<id>","title":"…","book":"…"}}')
        else:
            try:
                deck_id = DeckHeader.model_validate(head).deck
            except ValidationError as e:
                errors += _errors(name, n, e)
        cards = rows[1:]
        if not cards:
            errors.append(f"{name}: has no cards; add at least one card line after the header")
        if len(cards) > MAX_CARDS:
            errors.append(f"{name}: has {len(cards)} cards, the limit is {MAX_CARDS}; split it into two decks")
        for n, obj in cards:
            try:
                CARD.validate_python(obj)
            except ValidationError as e:
                errors += _errors(name, n, e)
        errors += _unique(name, cards, "card")
        errors += _too_long(name, cards)
    if errors:
        raise InvalidData(f"{name}: invalid deck", _by_line(name, errors))
    records = [obj for _, obj in rows]
    return Checked(deck_id, jsonl_bytes(records), len(records) - 1, records)


def _inline(name: str, n: int, item: dict, known: Mapping[str, "Highlight"]) -> list[str]:
    """Copy `text` and `title` into a highlight item from the pulled highlight it names."""
    ref = item.get("highlight")
    if not isinstance(ref, str):
        return [f"{name}:{n}: highlight: is required; add the id of a pulled highlight"]
    source = known.get(ref)
    if source is not None:
        item.setdefault("text", source.text)
        item.setdefault("title", source.title or Path(source.book).stem)
    elif "text" not in item:
        return [
            f"{name}:{n}: highlight: '{ref}' is not in the pulled highlights; "
            "use an id from x4 context, run x4 pull, or give text and title inline"
        ]
    return []


def check_threads(path: Path) -> Checked:
    """Validate X4_HOME/threads.jsonl. An empty file is valid and clears the threads."""
    name = str(path)
    rows, errors = _lines(name, path.read_bytes())
    for n, obj in rows:
        try:
            Thread.model_validate(obj)
        except ValidationError as e:
            errors += _errors(name, n, e)
    errors += _unique(name, rows, "thread")
    if len(rows) > THREADS_MAX:
        errors.append(f"{name}: has {len(rows)} threads, the X4 keeps track of {THREADS_MAX}; drop the stories that ended")
    if errors:
        raise InvalidData(f"{name}: invalid threads", _by_line(name, errors))
    records = [obj for _, obj in rows]
    return Checked("threads", jsonl_bytes(records), len(records), records)


def check_stream(path: Path, data: bytes | None = None, highlights: Mapping[str, "Highlight"] | None = None) -> Checked:
    """Validate a stream file: bites, resurfaced highlights and book puzzles. An empty file is
    valid and clears the stream. `highlight` items get `text` and `title` from `highlights`
    (id -> pulled highlight)."""
    name = str(path)
    rows, errors = _lines(name, path.read_bytes() if data is None else data)
    for n, obj in rows:
        if obj.get("kind") == "highlight":
            found = _inline(name, n, obj, highlights or {})
            if found:
                errors += found
                continue
        try:
            STREAM_ITEM.validate_python(obj)
        except ValidationError as e:
            errors += _errors(name, n, e)
    errors += _unique(name, rows, "stream item")
    errors += _too_long(name, rows)
    if len(rows) > STREAM_LINES:
        errors.append(f"{name}: has {len(rows)} items, the X4 reads the first {STREAM_LINES}; drop seen ones")
    if errors:
        raise InvalidData(f"{name}: invalid stream", _by_line(name, errors))
    records = [obj for _, obj in rows]
    return Checked("stream", jsonl_bytes(records), len(records), records)


def check_read(path: Path, data: bytes | None = None) -> list[ReadSource]:
    """Validate ~/x4/read.jsonl, the Read list as Claude writes it. An empty file clears it."""
    name = str(path)
    rows, errors = _lines(name, path.read_bytes() if data is None else data)
    items = []
    for n, obj in rows:
        try:
            items.append(ReadSource.model_validate(obj))
        except ValidationError as e:
            errors += _errors(name, n, e)
    errors += _unique(name, rows, "Read list")
    if errors:
        raise InvalidData(f"{name}: invalid Read list", _by_line(name, errors))
    return items


def check_glossary(path: Path) -> dict[str, str]:
    """Validate a glossary file: one JSON object, term -> definition."""
    name = str(path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise InvalidData(f"{name}: invalid glossary", [f"{name}: not valid JSON ({e}); write one object of term to definition"])
    if not isinstance(data, dict):
        raise InvalidData(f"{name}: invalid glossary", [f'{name}: must be a JSON object like {{"term": "definition"}}'])
    errors = []
    for term, definition in data.items():
        if not term.strip() or term != term.strip() or "\n" in term:
            errors.append(f"{name}: {term!r}: term must be non-empty, on one line, without surrounding spaces")
        elif len(term.encode()) > TERM_BYTES:
            errors.append(f"{name}: {term[:40]!r}: term is {len(term.encode())} bytes, the limit is {TERM_BYTES}")
        if not isinstance(definition, str) or not definition.strip():
            errors.append(f"{name}: {term!r}: definition must be a non-empty string")
    if not data:
        errors.append(f"{name}: has no terms; add at least one")
    if errors:
        raise InvalidData(f"{name}: invalid glossary", errors)
    return data
