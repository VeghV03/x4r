"""`x4 push book`: one EPUB, PDF or web article in, device-ready files out."""

import logging
from pathlib import Path
from typing import cast

from . import Upload
from .article import prepare_article
from .epub import optimize_epub
from .names import slugify
from .pdf import Mode, convert_pdf

log = logging.getLogger(__name__)

BOOKS = "/books"
PDF_MODES = ("pages", "reflow")


def _split_mode(src: str) -> tuple[Path, Mode | None]:
    """'paper.pdf#pages' -> (paper.pdf, 'pages'). A file whose real name has '#' is left alone."""
    path = Path(src).expanduser()
    if path.exists() or "#" not in src:
        return path, None
    name, _, mode = src.rpartition("#")
    if mode not in PDF_MODES:
        raise ValueError(f"unknown PDF mode #{mode}; use #pages or #reflow")
    return Path(name).expanduser(), cast(Mode, mode)


def prepare_book(src: str, workdir: Path) -> list[Upload]:
    """Convert a .epub, .pdf (optionally `#pages` or `#reflow`) or http(s) URL for the device."""
    workdir.mkdir(parents=True, exist_ok=True)
    if src.startswith(("http://", "https://")):
        return [prepare_article(src, workdir)]
    path, mode = _split_mode(src)
    if not path.is_file():
        raise FileNotFoundError(path)
    slug = slugify(path.stem)
    suffix = path.suffix.lower()
    if suffix == ".epub":
        if mode:
            raise ValueError("#pages and #reflow apply to PDFs only")
        out = workdir / f"{slug}.epub"
        optimize_epub(path, out)
        return [Upload(out, f"{BOOKS}/{out.name}")]
    if suffix == ".pdf":
        out, verdict = convert_pdf(path, workdir, slug, mode)
        log.info("%s: %s", path.name, verdict.reason)
        return [Upload(out, f"{BOOKS}/{out.name}", note=f"{verdict.mode}: {verdict.reason}")]
    raise ValueError(f"unsupported book format {suffix or path.name}; expected .epub, .pdf or a URL")
