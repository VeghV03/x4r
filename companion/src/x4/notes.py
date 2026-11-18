"""Render pulled highlights into one Markdown file per book, in reading order."""

import re
import unicodedata
from pathlib import Path

from .home import Home
from .models import Highlight


def slug(text: str) -> str:
    ascii_text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    return re.sub(r"[^a-z0-9]+", "-", ascii_text.lower()).strip("-") or "untitled"


def _title(items: list[Highlight], book: str) -> str:
    return next((h.title for h in items if h.title), None) or Path(book).stem


def render(book: str, items: list[Highlight]) -> str:
    """One book's notes. Highlights are blockquotes; questions start with **?**. Highlights
    made on a bite or a puzzle follow the book's, under the item they were made on."""

    def order(h: Highlight) -> tuple:
        step = h.item.id if h.item else ""
        return (h.item is not None, step, h.pos.spine, h.pos.start, h.pos.end, h.id)

    items = sorted(items, key=order)
    author = next((h.author for h in items if h.author), None)
    out = [f"# {_title(items, book)}", ""]
    if author:
        out += [author, ""]
    chapter: str | None = None
    for h in items:
        if h.section != chapter and h.section:
            out += [f"## {h.section}", ""]
        chapter = h.section or chapter
        mark = "**?** " if h.kind == "question" else ""
        paragraphs = [p.strip() for p in h.text.split("\n\n") if p.strip()]
        quote = "\n>\n".join("> " + p.replace("\n", "\n> ") for p in paragraphs) or ">"
        out += [quote.replace("> ", f"> {mark}", 1), ""]
    return "\n".join(out).rstrip() + "\n"


def note_paths(h: Home, books: list[str], titles: dict[str, str]) -> dict[str, Path]:
    """Book key -> notes file. Same-title books get the book key's stem appended."""
    paths: dict[str, Path] = {}
    used: set[str] = set()
    for book in sorted(books):
        name = slug(titles.get(book) or Path(book).stem)
        if name in used:
            name = f"{name}-{slug(Path(book).stem)}"
        used.add(name)
        paths[book] = h.notes / f"{name}.md"
    return paths


def render_all(h: Home, highlights: list[Highlight], made: list[Highlight] = ()) -> list[Path]:
    """Rewrite every book's notes file. A book in `made` (every highlight ever pulled) whose
    highlights were all deleted loses its file. Returns the files changed or removed."""
    by_book: dict[str, list[Highlight]] = {}
    for item in highlights:
        by_book.setdefault(item.book, []).append(item)
    titles = {b: _title(items, b) for b, items in by_book.items()}
    changed = []
    paths = note_paths(h, list(by_book), titles)
    for book, path in paths.items():
        text = render(book, by_book[book])
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8")
            changed.append(path)
    gone: dict[str, list[Highlight]] = {}
    for item in made:
        if item.book not in by_book:
            gone.setdefault(item.book, []).append(item)
    kept = set(paths.values())
    for path in note_paths(h, list(gone), {b: _title(items, b) for b, items in gone.items()}).values():
        if path not in kept and path.exists():
            path.unlink()
            changed.append(path)
    return changed
