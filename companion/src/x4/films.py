"""Film's memory: X4_HOME/seen.md, what you have watched, so a pick never repeats one (D40).

One line per film or series, as `Original title (year)`; a series watched only in part names
its seasons one line each, `Original title (year) season 2`. A line without a season covers
the whole series. A leading `- ` is allowed; blank lines and lines starting with `#` are
skipped. Titles match ignoring case and spacing. `taste.md` beside it is for Claude alone.
"""

import re

from .home import Home
from .models import ReadSource

LINE = re.compile(r"^(?:-\s+)?(.+?)\s+\((\d{4})\)(?:\s+season\s+(\d+))?\s*$", re.I)


def _key(title: str) -> str:
    return " ".join(title.casefold().split())


def seen(h: Home) -> tuple[dict[tuple[str, int], set[int] | None], list[str]]:
    """(title, year) -> the seasons seen, None for all of it; and the lines that do not parse."""
    out: dict[tuple[str, int], set[int] | None] = {}
    errors: list[str] = []
    if not h.seen_films.exists():
        return out, errors
    for n, line in enumerate(h.seen_films.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = LINE.match(line)
        if not m:
            errors.append(f"{h.seen_films}:{n}: write 'Original title (year)', or 'Original title (year) season 2' for one season")
            continue
        key = (_key(m.group(1)), int(m.group(2)))
        if m.group(3) is None:
            out[key] = None
        elif (seasons := out.setdefault(key, set())) is not None:
            seasons.add(int(m.group(3)))
    return out, errors


def check(h: Home, reads: list[ReadSource]) -> list[str]:
    """Film picks that seen.md says you watched, and seen.md's own errors."""
    watched, errors = seen(h)
    for item in reads:
        if item.film is None:
            continue
        key = (_key(item.film.title), item.film.year)
        if key not in watched:
            continue
        seasons = watched[key]
        if seasons is None or item.film.season in seasons:
            what = f"{item.film.title} ({item.film.year})" + (f" season {item.film.season}" if item.film.season else "")
            errors.append(f"{h.read}: {item.id}: film: {what} is in {h.seen_films.name}; pick another")
    return errors
