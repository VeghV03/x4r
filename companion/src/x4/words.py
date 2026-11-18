"""Word lists and 7x7 crosswords for the word games (docs/DATA.md, "Word lists" and "Crosswords").

Words come from the same kaikki.org Wiktionary extract as the dictionaries. Wiktionary has no
word frequencies, so words are ranked by hermitdave/FrequencyWords (OpenSubtitles 2018, top
50k per language, MIT). Without that list the rank falls back to how many senses and
translations Wiktionary gives a word, a rough proxy for how common it is.

Crossword answers are rare words: past the most frequent few thousand. The crosswords
`x4 words install` builds have Wiktionary glosses as clues, English ones for Hungarian words too:
kaikki.org extracts only the English Wiktionary. `grids` makes filled grids without clues instead,
seeded with words from the user's reading, for Claude to clue in X4_HOME/crosswords/<lang>.jsonl.
"""

import gzip
import json
import logging
import random
import re
import unicodedata
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from .convert import Upload
from .dicts import SOURCES, _lines, download

log = logging.getLogger(__name__)

ALPHABETS = {
    "en": "abcdefghijklmnopqrstuvwxyz",
    "hu": "aábcdeéfghiíjklmnoóöőpqrstuúüűvwxyz",
}
FREQ_URL = "https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/{lang}/{lang}_50k.txt"

ANSWERS_MAX = 2500
RARE_FROM = 3000  # crossword words skip this many most frequent
# Crossword words without a frequency rank need this many senses and translations.
UNRANKED_SCORE = {"en": 3, "hu": 1}
CROSSWORDS = 200
CLUE_MAX = 60
FILL_NODES = 20_000  # backtracking steps per pattern before trying the next one
BRANCH = 100  # words tried per slot
SEEDS_PER_GRID = 4  # seed words one grid tries to take
SEED_NODES = 4000  # backtracking steps for a fill that tries one more seed
LEXICON_VERSION = 3  # the cached lexicon's format
NAME_SCORE = 10  # a word that is also a name needs this many senses and translations to count
# Answers need this many senses and translations; English entries list translations, Hungarian ones do not.
ANSWER_SCORE = {"en": 2, "hu": 1}

SKIP_POS = {"name", "character", "symbol", "punct", "romanization", "prefix", "suffix", "infix", "interfix",
            "circumfix", "affix", "combining_form", "phrase", "proverb", "abbrev", "contraction"}  # fmt: skip
BAD_TAGS = {"vulgar", "offensive", "derogatory", "slur", "pejorative", "ethnic"}
ABBREV_TAGS = {"abbreviation", "acronym", "initialism"}
RARE_TAGS = {"archaic", "obsolete", "dated", "rare", "dialectal", "misspelling", "nonstandard", "proscribed",
             "historical", "nonce-word"}  # fmt: skip
FORM_SKIP_TAGS = {"table-tags", "inflection-template", "class", "romanization", "error-unrecognized-form"}
ALT_FORM_TAGS = {"alternative", "archaic", "obsolete", "dialectal", "misspelling", "nonstandard",
                 "pronunciation-spelling", "rare"}  # fmt: skip
WORD_MIN, WORD_MAX = 3, 8

SIZE = 7  # crossword grids are 7x7
# Patterns of black squares (#), symmetric under a half turn, 8-15 black squares. Every run is 3-7
# cells, so every white cell is in an across and a down word. Each fills from the rare Hungarian
# headwords (Hungarian has far fewer of them than English); more open grids do not.
PATTERNS = [
    ("...#...", "...#...", ".......", "##...##", ".......", "...#...", "...#..."),
    ("###...#", "##.....", ".......", "...#...", ".......", ".....##", "#...###"),
    ("###....", "##.....", "#......", "...#...", "......#", ".....##", "....###"),
    ("#...###", "....###", ".......", "...#...", ".......", "###....", "###...#"),
    ("....###", ".....##", "......#", "...#...", "#......", "##.....", "###...."),
    ("#....##", ".....##", ".....##", "...#...", "##.....", "##.....", "##....#"),
    ("###...#", "###....", ".......", "...#...", ".......", "....###", "#...###"),
    ("###...#", "##.....", "#......", "...#...", "......#", ".....##", "#...###"),
    ("....###", ".....##", ".....##", "...#...", "##.....", "##.....", "###...."),
    ("#...###", ".....##", "......#", "...#...", "#......", "##.....", "###...#"),
    ("###....", "###....", "#......", "...#...", "......#", "....###", "....###"),
]


@dataclass
class Word:
    lemma: bool = False  # has a sense of its own, not as an abbreviation
    clean: bool = False  # ... that is not archaic, rare or dialectal
    inflected: bool = False  # an inflected form of another word
    alt: bool = False  # another spelling or a misspelling of a word
    bad: bool = False  # some sense is vulgar, offensive or a slur
    named: bool = False  # mostly a proper noun: "debby" is Debby before it is "like a debutante"
    score: int = 0  # senses and translations: the frequency proxy
    glosses: list[str] = field(default_factory=list)


@dataclass
class Lexicon:
    words: dict[str, Word]  # every accepted word of 3-8 letters


def _letters(lang: str) -> re.Pattern:
    return re.compile(f"[{ALPHABETS[lang]}]{{{WORD_MIN},{WORD_MAX}}}")


def read_lexicon(lines: Iterable[str | bytes], lang: str) -> Lexicon:
    """Wiktextract JSON lines -> the words of 3-8 letters in the language's alphabet."""
    ok = _letters(lang).fullmatch
    code = SOURCES[lang].lang_code
    words: dict[str, Word] = {}
    names: set[str] = set()
    for line in lines:
        record = json.loads(line)
        word, pos = record.get("word", ""), record.get("pos")
        if record.get("lang_code") != code:
            continue
        if pos == "name":
            if ok(word.lower()) and word != word.lower():
                names.add(word.lower())
            continue
        if pos in SKIP_POS:
            continue
        for form in record.get("forms", []):
            text, tags = form.get("form", ""), set(form.get("tags", []))
            if ok(text) and not FORM_SKIP_TAGS & tags:
                entry = words.setdefault(text, Word())
                if tags & ALT_FORM_TAGS:
                    entry.alt = True
                else:
                    entry.inflected = True
        if not ok(word):
            continue
        entry = words.setdefault(word, Word())
        entry.score += len(record.get("senses", [])) + len(record.get("translations", []))
        for sense in record.get("senses", []):
            tags = set(sense.get("tags", []))
            if tags & BAD_TAGS:
                entry.bad = True
            if "alt_of" in sense or {"alt-of", "misspelling"} & tags:
                entry.alt = True
            elif "form_of" in sense or "form-of" in tags:
                entry.inflected = True
            elif not (ABBREV_TAGS | {"no-gloss"}) & tags:
                entry.lemma = True
                if not tags & RARE_TAGS:
                    entry.clean = True
                    gloss = (sense.get("glosses") or [""])[0]  # a subsense lists its parent first
                    if gloss and gloss not in entry.glosses and len(entry.glosses) < 4:
                        entry.glosses.append(gloss)
    for name in names & words.keys():
        words[name].named = words[name].score < NAME_SCORE
    return Lexicon({w: e for w, e in words.items() if e.lemma or e.inflected or e.alt})


def read_ranks(lines: Iterable[str], lang: str) -> dict[str, int]:
    """FrequencyWords lines ("word count", most frequent first) -> word -> rank."""
    ok = _letters(lang).fullmatch
    ranks: dict[str, int] = {}
    for i, line in enumerate(lines):
        word = line.split(" ", 1)[0].strip().lower()
        if ok(word):
            ranks.setdefault(word, i)
    return ranks


def proxy_ranks(lex: Lexicon) -> dict[str, int]:
    """Without a frequency list: words with more senses and translations rank first."""
    order = sorted((w for w, e in lex.words.items() if e.lemma), key=lambda w: (-lex.words[w].score, w))
    return {w: i for i, w in enumerate(order)}


def _good(lex: Lexicon, word: str) -> bool:
    """A word to ask for: a plain, current headword."""
    e = lex.words.get(word)
    return bool(e and e.clean and not e.bad and not e.named)


def by_rank(ranks: dict[str, int]) -> list[str]:
    return sorted(ranks, key=lambda w: (ranks[w], w))


def answers5(lex: Lexicon, ranks: dict[str, int], lang: str) -> list[str]:
    """Common five-letter headwords, most frequent first."""
    least = ANSWER_SCORE[lang]
    found = [w for w in by_rank(ranks) if len(w) == 5 and _good(lex, w) and lex.words[w].score >= least]
    return found[:ANSWERS_MAX]


def valid5(lex: Lexicon, answers: list[str]) -> list[str]:
    return sorted({w for w in lex.words if len(w) == 5} | set(answers))


# crosswords

_PAREN = re.compile(r"\s*\([^()]*\)")
# Glosses that describe grammar ("possessive of él") rather than meaning.
_GRAMMAR = re.compile(
    r"\b(singular|plural|possessive|participle|inflection|tense|person|form|spelling) of\b|script letter", re.I
)


def clue(word: str, glosses: list[str]) -> str | None:
    """The first short gloss that does not give the word away."""
    for gloss in glosses:
        text = " ".join(_PAREN.sub("", gloss).split(";")[0].split()).rstrip(".:,").strip()
        if len(text) < 3 or len(text) > CLUE_MAX or not text[0].isalpha() or ":" in text:
            continue
        if word in text.lower() or _GRAMMAR.search(text):
            continue
        return text[0].upper() + text[1:]
    return None


@dataclass(frozen=True)
class Slot:
    cells: tuple[int, ...]
    across: bool


def slots(pattern: tuple[str, ...]) -> list[Slot]:
    """Runs of two or more white cells, across then down."""
    n = len(pattern)
    out = []
    for across in (True, False):
        for line in range(n):
            run: list[int] = []
            for k in range(n + 1):
                r, c = (line, k) if across else (k, line)
                if k < n and pattern[r][c] != "#":
                    run.append(r * n + c)
                    continue
                if len(run) >= 2:
                    out.append(Slot(tuple(run), across))
                run = []
    return out


def _mask(bits: list[int], size: int) -> int:
    out = bytearray((size + 7) // 8)
    for i in bits:
        out[i >> 3] |= 1 << (i & 7)
    return int.from_bytes(out, "little")


class Filler:
    """Backtracking fill of a square pattern, most constrained slot first.

    The words of one length are the bits of an int, one int per (length, position, letter), so a
    slot's candidates are the AND of a few ints. Lower bits are tried first: the seeds in their
    order, then the pool in its order. A word is used once per grid; `exclude` keeps words out of
    later grids too."""

    def __init__(self, pool: Iterable[str], seeds: Iterable[str] = ()):
        self.seeds = list(dict.fromkeys(seeds))
        self.words: dict[int, list[str]] = {}
        for w in dict.fromkeys([*self.seeds, *pool]):
            self.words.setdefault(len(w), []).append(w)
        self.bit: dict[str, int] = {}
        self.all: dict[int, int] = {}
        self.index: dict[tuple[int, int, str], int] = {}
        for n, ws in self.words.items():
            at: dict[tuple[int, int, str], list[int]] = {}
            for i, w in enumerate(ws):
                self.bit[w] = i
                for p, ch in enumerate(w):
                    at.setdefault((n, p, ch), []).append(i)
            self.index.update({k: _mask(v, len(ws)) for k, v in at.items()})
            self.all[n] = (1 << len(ws)) - 1
        self.excluded: dict[int, int] = dict.fromkeys(self.words, 0)

    def exclude(self, words: Iterable[str]) -> None:
        for w in words:
            if w in self.bit:
                self.excluded[len(w)] |= 1 << self.bit[w]

    def clear(self) -> None:
        """Let every word back in."""
        self.excluded = dict.fromkeys(self.words, 0)

    def _mask(self, slot: Slot, grid: list[str], taken: dict[int, int]) -> int:
        n = len(slot.cells)
        m = self.all.get(n, 0) & ~taken.get(n, 0)
        for p, c in enumerate(slot.cells):
            if grid[c] and m:
                m &= self.index.get((n, p, grid[c]), 0)
        return m

    def _fill(self, pattern: tuple[str, ...], fixed: list[tuple[Slot, str]], budget: int) -> list[str] | None:
        size = len(pattern)
        grid = [""] * (size * size)
        taken = dict(self.excluded)
        todo = [s for s in slots(pattern) if s not in {slot for slot, _ in fixed}]
        for slot, w in fixed:
            for c, ch in zip(slot.cells, w, strict=True):
                if grid[c] and grid[c] != ch:
                    return None
                grid[c] = ch
            taken[len(w)] |= 1 << self.bit[w]
        nodes = 0

        def step() -> bool:
            nonlocal nodes
            if not todo:
                return True
            nodes += 1
            if nodes > budget:
                return False
            best = None
            for s in todo:
                m = self._mask(s, grid, taken)
                if not m:
                    return False
                count = m.bit_count()
                if best is None or count < best[0]:
                    best = (count, s, m)
            _, slot, m = best
            n = len(slot.cells)
            todo.remove(slot)
            for _ in range(BRANCH):
                if not m:
                    break
                low = m & -m
                m ^= low
                word = self.words[n][low.bit_length() - 1]
                saved = [grid[c] for c in slot.cells]
                for c, ch in zip(slot.cells, word, strict=True):
                    grid[c] = ch
                taken[n] |= low
                if step():
                    return True
                taken[n] ^= low
                for c, ch in zip(slot.cells, saved, strict=True):
                    grid[c] = ch
            todo.append(slot)
            return False

        if not step():
            return None
        return ["".join(grid[r * size + c] or "." for c in range(size)) for r in range(size)]

    def fill(self, pattern: tuple[str, ...], rng: random.Random) -> list[str] | None:
        """The filled rows, or None. Seeds go in first, in their order, at most SEEDS_PER_GRID:
        each takes the first free slot of its length (tried in random order) where the whole grid
        still fills, or is left out."""
        found = slots(pattern)
        fixed: list[tuple[Slot, str]] = []
        rows = None
        for w in self.seeds:
            if len(fixed) == SEEDS_PER_GRID:
                break
            if w not in self.bit or self.excluded[len(w)] >> self.bit[w] & 1:
                continue
            used = {slot for slot, _ in fixed}
            free = [s for s in found if len(s.cells) == len(w) and s not in used]
            rng.shuffle(free)
            for slot in free:
                if (got := self._fill(pattern, [*fixed, (slot, w)], SEED_NODES)) is not None:
                    fixed.append((slot, w))
                    rows = got
                    break
        return rows or self._fill(pattern, [], FILL_NODES)


def numbered(rows: list[str]) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
    """(number, word) for the across and down entries, numbered in reading order."""
    n = len(rows)
    pattern = tuple(r.replace(".", "#") for r in rows)
    found = slots(pattern)
    starts = sorted({s.cells[0] for s in found})
    number = {cell: i + 1 for i, cell in enumerate(starts)}
    word = {s: "".join(rows[c // n][c % n] for c in s.cells) for s in found}
    across = sorted((number[s.cells[0]], word[s]) for s in found if s.across)
    down = sorted((number[s.cells[0]], word[s]) for s in found if not s.across)
    return across, down


# Glosses that only name a form of another word: "third-person singular possessive of kép".
_FORM_OF = re.compile(
    r"\b(?:singular|plural|possessive|participle|inflection|tense|person|form|spelling|conditional|imperative|"
    r"subjunctive|indicative|accusative|dative|nominative|case|superlative|comparative|past|present|future|"
    r"definite|indefinite)\s+of\s+\S+\W*$",
    re.I,
)


def _headword(lex: Lexicon, word: str) -> bool:
    """A plain, current headword with a meaning of its own: `orrom` (my nose) is only a possessive."""
    if not _good(lex, word):
        return False
    glosses = [_PAREN.sub("", g).strip() for g in lex.words[word].glosses]
    return not glosses or any(g and not _FORM_OF.search(g) and not _GRAMMAR.search(g) for g in glosses)


def grid_pool(lex: Lexicon, ranks: dict[str, int], lang: str) -> list[str]:
    """Headwords of 3-7 letters, rare first: past the RARE_FROM most frequent by rank; then the
    ones the frequency list does not have, with at least UNRANKED_SCORE senses and translations
    (English Wiktionary is full of one-sense oddities); then the common ones, which the filler
    reaches only where nothing rarer fits. A three-letter word is rare at any rank but must be
    ranked: rare ones are too few to fill a grid, unranked ones are mostly letters and codes."""
    ranked = [w for w in by_rank(ranks) if WORD_MIN <= len(w) <= SIZE and _headword(lex, w)]
    rare = [w for w in ranked if len(w) == WORD_MIN or ranks[w] >= RARE_FROM]
    common = [w for w in ranked if len(w) > WORD_MIN and ranks[w] < RARE_FROM]
    least = UNRANKED_SCORE[lang]
    unranked = sorted(
        (w for w in lex.words if w not in ranks and WORD_MIN < len(w) <= SIZE and lex.words[w].score >= least
         and _headword(lex, w)),
        key=lambda w: (-lex.words[w].score, w),
    )  # fmt: skip
    return rare + unranked + common


def _shuffled(pool: list[str], rng: random.Random) -> list[str]:
    """The pool's order loosened: a word moves up or down by about its own position, so the
    rare, unranked and common parts mostly keep their order."""
    keys = {w: i * (0.5 + rng.random()) for i, w in enumerate(pool)}
    return sorted(pool, key=keys.__getitem__)


def seed_words(seeds: Iterable[str], lang: str) -> tuple[list[str], list[str]]:
    """Seeds that can go in a grid (lowercase letters of the language, 3-7 of them), in order,
    and the ones that cannot."""
    ok = re.compile(f"[{ALPHABETS[lang]}]{{{WORD_MIN},{SIZE}}}").fullmatch
    good, bad = [], []
    for raw in seeds:
        w = unicodedata.normalize("NFC", raw.strip()).lower()
        if not w:
            continue
        (good if ok(w) else bad).append(w)
    return list(dict.fromkeys(good)), list(dict.fromkeys(bad))


def grids(
    lex: Lexicon,
    ranks: dict[str, int],
    lang: str,
    first: int,
    count: int,
    recent: set[str],
    seeds: dict[str, str | None] | None = None,
) -> dict:
    """`count` filled grids numbered from `first`, without clues and without the `recent` answers.

    `seeds` (word -> source title or None, in priority order) go in first: each grid takes the
    pattern that places the most of them, the rest fill from the rare pool. A seed's entries keep
    `"seed": true` and its `src`. No answer repeats within the batch. The same inputs give the same
    grids. Returns {"grids", "placed", "unplaced", "skipped"}."""
    seeds = seeds or {}
    good, skipped = seed_words(seeds, lang)
    source = {unicodedata.normalize("NFC", k.strip()).lower(): v for k, v in seeds.items()}
    skipped += [w for w in good if w in recent]
    left = [w for w in good if w not in recent]
    rng = random.Random(f"x4-grid-{lang}")
    filler = Filler(_shuffled(grid_pool(lex, ranks, lang), rng), left)
    filler.exclude(recent)
    out, placed = [], []
    for number in range(first, first + count):
        filler.seeds = [w for w in left if w not in placed]
        best = None
        for k in range(len(PATTERNS)):
            rng = random.Random(f"x4-grid-{lang}-{number}-{k}")
            rows = filler.fill(PATTERNS[(number + k) % len(PATTERNS)], rng)
            if not rows:
                continue
            across, down = numbered(rows)
            got = sum(w in filler.seeds for _, w in across + down)
            if best is None or got > best[0]:
                best = (got, rows, across, down)
            if not filler.seeds or got == len(filler.seeds):
                break
        if best is None:
            raise ValueError(f"cannot fill grid {lang}-{number:04d}: too few {lang} words besides the recent answers")
        _, rows, across, down = best
        answers = [w for _, w in across + down]
        filler.exclude(answers)
        placed += [w for w in answers if w in filler.seeds]

        def entry(n: int, w: str) -> dict:
            e: dict = {"n": n, "answer": w}
            if w in filler.seeds:
                e["seed"] = True
                if source.get(w):
                    e["src"] = source[w]
            return e

        out.append({
            "id": f"{lang}-{number:04d}",
            "size": SIZE,
            "rows": rows,
            "across": [entry(n, w) for n, w in across],
            "down": [entry(n, w) for n, w in down],
        })  # fmt: skip
    return {"grids": out, "placed": placed, "unplaced": [w for w in left if w not in placed], "skipped": skipped}


def crosswords(lex: Lexicon, ranks: dict[str, int], lang: str, count: int = CROSSWORDS) -> list[dict]:
    """`count` crosswords of rare words clued by their Wiktionary glosses, the same every run for
    the same inputs. Answers repeat only once the pool runs dry."""
    clues = {w: c for w in grid_pool(lex, ranks, lang) if (c := clue(w, lex.words[w].glosses))}
    rng = random.Random(f"x4-crossword-{lang}")
    filler = Filler(_shuffled(list(clues), rng))
    out = []
    for i in range(count):
        rows = None
        for attempt in range(2):
            for k in range(len(PATTERNS)):
                rows = filler.fill(PATTERNS[(i + k) % len(PATTERNS)], rng)
                if rows:
                    break
            if rows or attempt:
                break
            filler.clear()
        if not rows:
            raise ValueError(f"cannot fill crossword {i + 1} for {lang}: the word list is too small")
        across, down = numbered(rows)
        filler.exclude(w for _, w in across + down)
        out.append({
            "id": f"{lang}-{i + 1:04d}",
            "size": SIZE,
            "rows": rows,
            "across": [{"n": n, "clue": clues[w]} for n, w in across],
            "down": [{"n": n, "clue": clues[w]} for n, w in down],
        })  # fmt: skip
    return out


# files


@dataclass(frozen=True)
class Built:
    upload: Upload
    count: int
    unit: str


def _write_lines(path: Path, lines: Iterable[str]) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = list(lines)
    path.write_bytes("".join(f"{line}\n" for line in lines).encode())
    return len(lines)


def build_from(lex: Lexicon, ranks: dict[str, int], lang: str, out_dir: Path, count: int = CROSSWORDS) -> list[Built]:
    """Write the word lists and, unless `count` is 0, the crosswords for `lang` into `out_dir`."""
    answers = answers5(lex, ranks, lang)
    files = [
        ("answers5.txt", answers, "words"),
        ("valid5.txt", valid5(lex, answers), "words"),
    ]
    out = []
    for name, lines, unit in files:
        n = _write_lines(out_dir / lang / name, lines)
        out.append(Built(Upload(out_dir / lang / name, f"/x4/words/{lang}/{name}"), n, unit))
    if not count:
        return out
    puzzles = crosswords(lex, ranks, lang, count)
    path = out_dir / f"{lang}.jsonl"
    n = _write_lines(path, (json.dumps(p, ensure_ascii=False, separators=(",", ":")) for p in puzzles))
    out.append(Built(Upload(path, f"/x4/crosswords/{lang}.jsonl"), n, "crosswords"))
    return out


def _load(path: Path) -> Lexicon:
    data = json.loads(gzip.decompress(path.read_bytes()))
    return Lexicon({w: Word(**e) for w, e in data["words"].items()})


def _lexicon(dump: Path, lang: str, cache: Path) -> Lexicon:
    """The lexicon of a dump, cached next to it: parsing the English dump takes minutes."""
    stamp = dump.stat()
    path = cache / f"{lang}-{stamp.st_size}-{int(stamp.st_mtime)}.lexicon{LEXICON_VERSION}.json.gz"
    if path.exists():
        return _load(path)
    lex = read_lexicon(_lines(dump), lang)
    path.parent.mkdir(parents=True, exist_ok=True)
    for old in path.parent.glob(f"{lang}-*.lexicon*.json.gz"):
        old.unlink()
    data = {"words": {w: e.__dict__ for w, e in lex.words.items()}}
    path.write_bytes(gzip.compress(json.dumps(data, ensure_ascii=False).encode()))
    return lex


def _ranks(lex: Lexicon, lang: str, workdir: Path, fetch: bool) -> dict[str, int]:
    try:
        if fetch:
            freq = download(FREQ_URL.format(lang=lang), workdir)
        else:
            freq = workdir / "downloads" / FREQ_URL.format(lang=lang).rsplit("/", 1)[1]
        return read_ranks(freq.read_text(encoding="utf-8").splitlines(), lang)
    except Exception as e:  # noqa: BLE001 - any failure to fetch means: use the proxy
        log.warning("no frequency list (%s); ranking by Wiktionary senses and translations", e)
        return proxy_ranks(lex)


def _check_lang(lang: str) -> None:
    if lang not in ALPHABETS:
        raise ValueError(f"no word lists for {lang!r}; choose from {', '.join(ALPHABETS)}")


def build_words(lang: str, workdir: Path, count: int = CROSSWORDS) -> list[Built]:
    """Download (cached) Wiktionary and the frequency list, then build `lang`'s files."""
    _check_lang(lang)
    lex = _lexicon(download(SOURCES[lang].url, workdir), lang, workdir / "words")
    return build_from(lex, _ranks(lex, lang, workdir, fetch=True), lang, workdir / "words", count)


def cached(lang: str, workdir: Path) -> tuple[Lexicon, dict[str, int]]:
    """The lexicon and frequency ranks `build_words` left in `workdir`, without downloading."""
    _check_lang(lang)
    pattern = f"{lang}-*.lexicon{LEXICON_VERSION}.json.gz"
    found = sorted((workdir / "words").glob(pattern), key=lambda p: p.stat().st_mtime)
    if not found:
        raise ValueError(f"no {lang} lexicon in {workdir / 'words'}: run x4 words install {lang} once")
    lex = _load(found[-1])
    return lex, _ranks(lex, lang, workdir, fetch=False)
