"""English and Hungarian StarDict dictionaries built from Wiktionary (kaikki.org extracts).

Source: English Wiktionary as extracted by wiktextract, published by kaikki.org
(CC BY-SA 4.0 and GFDL, like Wiktionary). "en" is English with English definitions;
"hu" is Hungarian words with English definitions (no Hungarian-language edition is extracted).

Each headword gets one HTML entry: part of speech, then a few short senses. Inflected forms,
alternative spellings and "form of" entries go into the .syn, so `ran`, `házak` or
`colour` find their headword. The device lowercases only ASCII before lookup, so
`Élet` at the start of a sentence gets its own .syn line pointing at `élet`.
"""

import gzip
import json
import logging
import re
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from html import escape
from pathlib import Path

import httpx

from .convert import Upload
from .stardict import write_stardict

log = logging.getLogger(__name__)

LICENSE = "CC BY-SA 4.0 and GFDL (Wiktionary contributors; extracted by wiktextract, kaikki.org)"
MAX_SENSES = 4  # per part of speech
MAX_GLOSS = 200  # characters
SKIP_POS = {"name", "character", "symbol", "punct", "romanization", "prefix", "suffix", "infix", "interfix",
            "circumfix", "affix", "combining_form"}  # fmt: skip
_SKIP_FORM_TAGS = {"table-tags", "inflection-template", "class", "romanization"}
_FORM = re.compile(r"^[^\W\d_](?:[\w'’.-]*\w)?$")  # one word: letters, inner hyphens or apostrophes
_POS_NAMES = {"adj": "adjective", "adv": "adverb", "intj": "interjection", "conj": "conjunction",
              "prep": "preposition", "postp": "postposition", "pron": "pronoun", "det": "determiner",
              "num": "numeral", "abbrev": "abbreviation"}  # fmt: skip


@dataclass(frozen=True)
class Source:
    url: str
    lang_code: str
    name: str
    bookname: str


SOURCES = {
    "en": Source(
        "https://kaikki.org/dictionary/English/kaikki.org-dictionary-English.jsonl.gz",
        "en",
        "english",
        "English (Wiktionary)",
    ),
    "hu": Source(
        "https://kaikki.org/dictionary/Hungarian/kaikki.org-dictionary-Hungarian.jsonl.gz",
        "hu",
        "magyar",
        "Magyar–English (Wiktionary)",
    ),
}


def download(url: str, workdir: Path) -> Path:
    """Download once into `workdir/downloads/`; later builds reuse the file."""
    path = workdir / "downloads" / url.rsplit("/", 1)[1]
    if path.exists():
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    part = path.with_suffix(path.suffix + ".part")
    log.info("downloading %s", url)
    with httpx.stream("GET", url, follow_redirects=True, timeout=60) as response:
        response.raise_for_status()
        with part.open("wb") as f:
            for chunk in response.iter_bytes(1 << 20):
                f.write(chunk)
    part.replace(path)
    return path


def _short(text: str) -> str:
    text = " ".join(text.split())
    if len(text) <= MAX_GLOSS:
        return text
    return text[:MAX_GLOSS].rsplit(" ", 1)[0].rstrip(",;:") + "…"


def _is_form(form: str, word: str) -> bool:
    return form != word and len(form) <= 48 and bool(_FORM.match(form))


def _capitalized(word: str) -> str | None:
    """'élet' -> 'Élet': the device cannot fold non-ASCII case itself."""
    first = word[:1]
    if first and not first.isascii() and first.islower() and first.upper() != first:
        return first.upper() + word[1:]
    return None


def parse_kaikki(lines: Iterable[str | bytes], lang_code: str) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """Wiktextract JSON lines -> (headword, HTML definition) entries and (form, headword) synonyms."""
    senses_by_word: dict[str, list[tuple[str, list[str]]]] = {}
    synonyms: list[tuple[str, str]] = []
    for line in lines:
        record = json.loads(line)
        word = record.get("word", "")
        pos = record.get("pos", "")
        if record.get("lang_code") != lang_code or not word or pos in SKIP_POS or len(word.encode()) > 128:
            continue
        glosses = []
        for sense in record.get("senses", []):
            for link in sense.get("form_of", []) + sense.get("alt_of", []):
                if link.get("word"):
                    synonyms.append((word, link["word"]))
            if "form_of" in sense or "alt_of" in sense or "no-gloss" in sense.get("tags", []):
                continue
            # A subsense lists its parent gloss first; the parent alone is the short definition.
            gloss = _short((sense.get("raw_glosses") or sense.get("glosses") or [""])[0])
            if gloss and gloss not in glosses and len(glosses) < MAX_SENSES:
                glosses.append(gloss)
        if glosses:
            senses_by_word.setdefault(word, []).append((_POS_NAMES.get(pos, pos), glosses))
            for form in record.get("forms", []):
                text = form.get("form", "")
                if not _SKIP_FORM_TAGS & set(form.get("tags", [])) and _is_form(text, word):
                    synonyms.append((text, word))
    entries = []
    for word, groups in senses_by_word.items():
        html = "".join(
            f"<p><i>{escape(pos)}</i></p><ol>{''.join(f'<li>{escape(g)}</li>' for g in glosses)}</ol>"
            for pos, glosses in groups
        )
        entries.append((word, html))
    synonyms = [(form, word) for form, word in synonyms if form not in senses_by_word]  # direct hits win
    caps = [(c, w) for w in senses_by_word if (c := _capitalized(w)) and c not in senses_by_word]
    caps += [(c, w) for f, w in synonyms if (c := _capitalized(f)) and c not in senses_by_word]
    return entries, synonyms + caps


def _lines(path: Path) -> Iterator[bytes]:
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rb") as f:
        yield from f


def build_from_file(path: Path, lang: str, out_dir: Path, url: str = "") -> list[Path]:
    """Build the StarDict for `lang` from a local kaikki JSONL (optionally gzipped)."""
    source = SOURCES[lang]
    entries, synonyms = parse_kaikki(_lines(path), source.lang_code)
    description = f"Source: {url or source.url}. License: {LICENSE}."
    return write_stardict(
        entries, out_dir, source.name, source.bookname, synonyms=synonyms, description=description, html=True
    )


def build_dictionary(lang: str, workdir: Path) -> list[Upload]:
    """Download (cached) and build the `en` or `hu` dictionary; files go to `/dictionaries/<lang>/`."""
    if lang not in SOURCES:
        raise ValueError(f"no dictionary source for {lang!r}; choose from {', '.join(SOURCES)}")
    source = SOURCES[lang]
    paths = build_from_file(download(source.url, workdir), lang, workdir / "dictionaries" / lang)
    return [Upload(p, f"/dictionaries/{lang}/{p.name}") for p in paths]
