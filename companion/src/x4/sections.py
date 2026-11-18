"""The paper's sections: X4_HOME/sections.md, one line per section in the front page's order.

A line is the section's name, a colon, and what belongs in it. `x4` reads the names and their
order; Claude reads the rest. Blank lines and lines starting with `#` are skipped. A home
without the file has no paper: the Read list goes out as a plain list (docs/DATA.md).
"""

import re
import unicodedata
from collections import Counter
from dataclasses import dataclass

from .errors import InvalidData
from .home import Home
from .models import ReadSource

MAX = 8
NAME_BYTES = 24
ID_BYTES = 16
PER_SECTION = 3  # pieces on a section's screen
LINKS = 8  # saved links that arrive kept in one issue
FORECASTS = 2  # forecasts in one issue
HEAD_CHARS = 30  # one line on the front page
LEAD_CHARS = 60  # two lines under a headline


@dataclass(frozen=True)
class Section:
    id: str
    name: str
    what: str


def section_id(name: str) -> str:
    """The name in lower case, accents dropped, spaces as `-`."""
    plain = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
    return re.sub(r"[^a-z0-9]+", "-", plain.lower()).strip("-")[:ID_BYTES].strip("-")


def load(h: Home) -> list[Section] | None:
    """The sections in order, or None when the home has no sections.md."""
    if not h.sections.exists():
        return None
    name = str(h.sections)
    out: list[Section] = []
    errors: list[str] = []
    for n, line in enumerate(h.sections.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        title, colon, what = line.partition(":")
        title = title.strip()
        if not colon or not title:
            errors.append(f"{name}:{n}: write a section as 'Name: what belongs in it'")
            continue
        if len(title.encode()) > NAME_BYTES:
            errors.append(f"{name}:{n}: '{title}' is {len(title.encode())} bytes; a name is {NAME_BYTES} at most")
        sid = section_id(title)
        if not sid:
            errors.append(f"{name}:{n}: '{title}' has no letters or digits to make an id from")
        elif any(s.id == sid for s in out):
            errors.append(f"{name}:{n}: '{title}' is there twice")
        out.append(Section(sid, title, what.strip()))
    if len(out) > MAX:
        errors.append(f"{name}: {len(out)} sections; the front page holds {MAX}")
    if errors:
        raise InvalidData(f"{name}: invalid sections", errors)
    return out


def check_reads(h: Home, sections: list[Section], reads: list[ReadSource]) -> list[str]:
    """Errors for Read items that do not fit the paper: no section, too many in one, no lines."""
    ids = [s.id for s in sections]
    errors = []
    for item in reads:
        where = f"{h.read}: {item.id}"
        if item.section is None:
            if not item.kept:
                errors.append(f"{where}: give it a section ({', '.join(ids)}), or \"kept\": true for a saved link")
            continue
        if item.section not in ids:
            errors.append(f"{where}: section '{item.section}' is not in {h.sections.name} ({', '.join(ids)})")
        if not item.head:
            errors.append(f"{where}: needs head, its headline for the front page ({HEAD_CHARS} characters)")
        if not item.lead:
            errors.append(f"{where}: needs lead, what it says ({LEAD_CHARS} characters)")
    if (n := sum(1 for i in reads if i.forecast)) > FORECASTS:
        errors.append(f"{h.read}: {n} forecasts; an issue asks {FORECASTS} at most")
    if (links := sum(1 for i in reads if i.kept and i.section is None)) > LINKS:
        errors.append(f"{h.read}: {links} saved links; an issue brings {LINKS}, the rest wait for the next")
    for sid, n in Counter(i.section for i in reads if i.section).items():
        if n > PER_SECTION:
            errors.append(f"{h.read}: {n} pieces in '{sid}'; a section holds {PER_SECTION}")
    return errors


def header(sections: list[Section], day: str) -> dict:
    """Line 1 of the device's /x4/read.jsonl."""
    return {"date": day, "sections": [{"id": s.id, "name": s.name} for s in sections]}
