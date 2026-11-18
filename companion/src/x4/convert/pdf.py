"""PDF to X4 books: reflow prose into an EPUB, render everything else as XTCH pages."""

import io
import re
import statistics
from collections import Counter
from dataclasses import dataclass, field
from html import escape
from pathlib import Path
from typing import Literal

import pymupdf
from PIL import Image

from . import epub, xtc
from .images import optimize_image

Mode = Literal["reflow", "pages"]

SAMPLE_PAGES = 40
SCANNED_SHARE = 0.5  # sampled pages that are an image without a text layer
MATH_SHARE = 0.02  # characters set in math fonts or math symbols
FIGURE_SHARE = 0.3  # sampled pages dominated by figures, diagrams or ruled tables
FIGURE_COVER = 0.25  # share of a page covered by raster images that makes it a figure page
FIGURE_DRAWINGS = 100  # vector paths that make a page a diagram or table page

_MATH_FONT = re.compile(
    r"CMMI|CMSY|CMEX|CMBSY|MSAM|MSBM|Math|STIX|Symbol|rsfs|esint|txsy|pxsy|eufm|eusm", re.IGNORECASE
)
_TEXT_FLAGS = pymupdf.TEXTFLAGS_DICT & ~pymupdf.TEXT_PRESERVE_IMAGES


@dataclass(frozen=True)
class Verdict:
    mode: Mode
    reason: str


def _is_math_char(ch: str) -> bool:
    cp = ord(ch)
    return (
        0x2200 <= cp <= 0x22FF  # mathematical operators
        or 0x27C0 <= cp <= 0x27EF
        or 0x2980 <= cp <= 0x2AFF
        or 0x1D400 <= cp <= 0x1D7FF  # mathematical alphanumerics
        or ch in "∑∏∫√∞≈≠≤≥±×÷∂∇"
    )


def _sample(doc: pymupdf.Document) -> list[int]:
    n = doc.page_count
    if n <= SAMPLE_PAGES:
        return list(range(n))
    return sorted({round(i * (n - 1) / (SAMPLE_PAGES - 1)) for i in range(SAMPLE_PAGES)})


def _image_cover(page: pymupdf.Page) -> float:
    area = abs(page.rect) or 1
    covered = sum(abs(pymupdf.Rect(info["bbox"]) & page.rect) for info in page.get_image_info())
    return min(1.0, covered / area)


def classify(doc: pymupdf.Document) -> Verdict:
    """Pages for scanned, math-heavy or figure-heavy PDFs, reflow for prose. The reason says why."""
    sample = _sample(doc)
    scanned = figures = chars = math = 0
    for index in sample:
        page = doc[index]
        page_chars = 0
        for block in page.get_text("dict", flags=_TEXT_FLAGS)["blocks"]:
            for line in block.get("lines", []):
                for span in line["spans"]:
                    text = "".join(span["text"].split())
                    page_chars += len(text)
                    if _MATH_FONT.search(span["font"]):
                        math += len(text)
                    else:
                        math += sum(map(_is_math_char, text))
        chars += page_chars
        cover = _image_cover(page)
        if page_chars < 50 and cover > 0.5:
            scanned += 1
        elif cover > FIGURE_COVER or len(page.get_cdrawings()) > FIGURE_DRAWINGS:
            figures += 1
    n = len(sample) or 1
    if scanned >= SCANNED_SHARE * n:
        return Verdict("pages", f"scanned: {scanned} of {n} sampled pages are images without text")
    if chars and math / chars > MATH_SHARE:
        return Verdict("pages", f"math: {100 * math / chars:.1f}% of characters are math")
    if figures >= FIGURE_SHARE * n:
        return Verdict("pages", f"figures: {figures} of {n} sampled pages are mostly figures, diagrams or tables")
    if chars / n < 200:
        return Verdict("pages", f"little text: {chars // n} characters per page")
    return Verdict(
        "reflow",
        f"prose: {chars // n} characters per page, {figures} of {n} pages with figures, "
        f"{100 * math / max(chars, 1):.1f}% math",
    )


def _title(doc: pymupdf.Document, fallback: str) -> str:
    title = (doc.metadata or {}).get("title", "").strip()
    if not title or re.search(r"\.(docx?|pdf|tex|indd)$|^microsoft word|^untitled", title, re.IGNORECASE):
        return fallback
    return title


def _author(doc: pymupdf.Document) -> str:
    return (doc.metadata or {}).get("author", "").strip()


# --- Pages: XTCH ---------------------------------------------------------------------------


def _content_box(doc: pymupdf.Document) -> tuple[float, float, float, float]:
    """Union of inked areas over all pages, as fractions of the page, plus a small margin."""
    x0 = y0 = 1.0
    x1 = y1 = 0.0
    for page in doc:
        pix = page.get_pixmap(matrix=pymupdf.Matrix(0.5, 0.5), colorspace=pymupdf.csGRAY, alpha=False)
        img = Image.frombytes("L", (pix.width, pix.height), pix.samples)
        box = img.point(lambda v: 255 if v < 200 else 0).getbbox()
        if box:
            x0, y0 = min(x0, box[0] / pix.width), min(y0, box[1] / pix.height)
            x1, y1 = max(x1, box[2] / pix.width), max(y1, box[3] / pix.height)
    if x1 <= x0 or y1 <= y0:
        return 0.0, 0.0, 1.0, 1.0
    margin = 0.015
    return max(0.0, x0 - margin), max(0.0, y0 - margin), min(1.0, x1 + margin), min(1.0, y1 + margin)


def render_page(page: pymupdf.Page, box: tuple[float, float, float, float]) -> Image.Image:
    """Crop to the content box, fit into 480x800 portrait, quantize to darkness indexes."""
    r = page.rect
    clip = pymupdf.Rect(
        r.x0 + box[0] * r.width, r.y0 + box[1] * r.height, r.x0 + box[2] * r.width, r.y0 + box[3] * r.height
    )
    zoom = min(xtc.WIDTH / clip.width, xtc.HEIGHT / clip.height)
    pix = page.get_pixmap(matrix=pymupdf.Matrix(zoom, zoom), clip=clip, colorspace=pymupdf.csGRAY, alpha=False)
    rendered = Image.frombytes("L", (pix.width, pix.height), pix.samples)
    canvas = Image.new("L", (xtc.WIDTH, xtc.HEIGHT), 255)
    canvas.paste(rendered, ((xtc.WIDTH - rendered.width) // 2, (xtc.HEIGHT - rendered.height) // 2))
    # Text and line art stay crisp with plain 4-level rounding; pictures need dithering.
    return xtc.quantize(canvas, dither=_image_cover(page) > FIGURE_COVER)


def _chapters(doc: pymupdf.Document) -> list[xtc.Chapter]:
    marks = [(page - 1, title.strip()) for level, title, page, *_ in doc.get_toc() if level <= 2 and page >= 1]
    marks.sort(key=lambda m: m[0])
    chapters = []
    for i, (start, name) in enumerate(marks):
        end = marks[i + 1][0] - 1 if i + 1 < len(marks) else doc.page_count - 1
        chapters.append(xtc.Chapter(name or f"Page {start + 1}", start, max(start, end)))
    return chapters


def pdf_to_xtch(doc: pymupdf.Document, out: Path, title: str) -> int:
    box = _content_box(doc)
    pages = (render_page(page, box) for page in doc)
    return xtc.write_xtch(out, pages, title=title, author=_author(doc), chapters=_chapters(doc))


# --- Reflow: EPUB --------------------------------------------------------------------------

_TERMINAL = tuple('.!?:;…"”’)]')
_PAGE_NUMBER = re.compile(r"^(page\s*)?([0-9]+|[ivxlcdm]+)(\s*(/|of)\s*[0-9]+)?$", re.IGNORECASE)
_HYPHENATED = re.compile(r"\b(\w+-\w+)\b")
_MARGIN = 0.08  # top and bottom share of a page where running headers and footers live
_MAX_CHAPTER_BYTES = 150_000


@dataclass
class _Run:
    text: str
    style: str  # "", "i", "b"


@dataclass
class _Para:
    runs: list[_Run] = field(default_factory=list)
    heading: int = 0  # 0 = paragraph, else heading level
    heading_size: float = 0  # font size of a large-type heading, before levels are assigned
    image: str = ""  # image name for a figure

    @property
    def plain(self) -> str:
        return "".join(r.text for r in self.runs).strip()

    def html(self) -> str:
        if self.image:
            return f'<figure><img src="images/{self.image}" alt=""/></figure>'
        parts, current, buf = [], "", ""
        for run in self.runs + [_Run("", "end")]:
            if run.style != current and buf:
                parts.append(f"<{current}>{escape(buf)}</{current}>" if current else escape(buf))
                buf = ""
            current = run.style
            buf += run.text
        inner = " ".join("".join(parts).split())
        tag = f"h{self.heading}" if self.heading else "p"
        return f"<{tag}>{inner}</{tag}>"


def _span_style(span: dict) -> str:
    font = span["font"].lower()
    if span["flags"] & 2 or "italic" in font or "oblique" in font:
        return "i"
    if span["flags"] & 16 or "bold" in font:
        return "b"
    return ""


def _line_key(text: str) -> str:
    return re.sub(r"\d+", "#", " ".join(text.lower().split()))


def _join(para: _Para, runs: list[_Run], hyphen_words: set[str]) -> None:
    """Append a line to a paragraph, undoing end-of-line hyphenation."""
    if not runs:
        return
    if not para.runs:
        para.runs.extend(runs)
        return
    last = para.runs[-1]
    head = runs[0].text.lstrip()
    tail = last.text.rstrip()
    if tail.endswith("­"):
        last.text = tail[:-1]
    elif tail.endswith("-") and len(tail) > 1 and tail[-2].isalpha() and head[:1].islower():
        before = re.findall(r"\w+$", tail[:-1])
        after = re.findall(r"^\w+", head)
        compound = f"{before[0]}-{after[0]}".lower() if before and after else ""
        last.text = tail if compound in hyphen_words else tail[:-1]
    else:
        last.text = tail + " "
    runs[0] = _Run(head, runs[0].style)
    para.runs.extend(runs)


def _figures(page: pymupdf.Page, decorative: set[int]) -> list[tuple[float, bytes]]:
    figures = []
    for info in page.get_image_info(xrefs=True):
        bbox = pymupdf.Rect(info["bbox"]) & page.rect
        if bbox.width < 60 or bbox.height < 40 or info.get("xref") in decorative:
            continue
        zoom = min(480 / bbox.width, 800 / bbox.height, 3.0)
        pix = page.get_pixmap(matrix=pymupdf.Matrix(zoom, zoom), clip=bbox, colorspace=pymupdf.csGRAY, alpha=False)
        img = Image.frombytes("L", (pix.width, pix.height), pix.samples)
        buf = io.BytesIO()
        img.save(buf, "PNG")
        figures.append((bbox.y0, buf.getvalue()))
    return figures


def pdf_to_epub(doc: pymupdf.Document, out: Path, title: str, source: str = "") -> None:
    pages = [page.get_text("dict", flags=_TEXT_FLAGS) for page in doc]

    # Running headers, footers and page numbers: short lines in the page margins that repeat.
    margin_keys: Counter[str] = Counter()
    all_text = []
    for page, data in zip(doc, pages, strict=True):
        keys = set()
        for block in data["blocks"]:
            for line in block.get("lines", []):
                text = "".join(s["text"] for s in line["spans"]).strip()
                all_text.append(text)
                y = line["bbox"][1]
                if y < page.rect.height * _MARGIN or y > page.rect.height * (1 - _MARGIN):
                    keys.add(_line_key(text))
        margin_keys.update(keys)
    repeats = max(3, round(0.2 * doc.page_count))
    running = {k for k, count in margin_keys.items() if count >= repeats}
    hyphen_words = {w.lower() for text in all_text for w in _HYPHENATED.findall(text)}

    sizes: Counter[float] = Counter()
    for data in pages:
        for block in data["blocks"]:
            for line in block.get("lines", []):
                for span in line["spans"]:
                    sizes[round(span["size"] * 2) / 2] += len(span["text"].strip())
    body = sizes.most_common(1)[0][0] if sizes else 10.0

    xref_pages: Counter[int] = Counter()
    for page in doc:
        xref_pages.update({info.get("xref", 0) for info in page.get_image_info(xrefs=True)})
    decorative = {x for x, count in xref_pages.items() if count >= 3}

    paras: list[_Para] = []
    images: dict[str, tuple[bytes, str]] = {}
    for page, data in zip(doc, pages, strict=True):
        figures = sorted(_figures(page, decorative))
        height = page.rect.height
        for block in data["blocks"]:
            lines = []
            for line in block.get("lines", []):
                text = "".join(s["text"] for s in line["spans"]).strip()
                y = line["bbox"][1]
                in_margin = y < height * _MARGIN or y > height * (1 - _MARGIN)
                if not text or in_margin and (_line_key(text) in running or _PAGE_NUMBER.match(text)):
                    continue
                lines.append(line)
            if not lines:
                continue
            while figures and figures[0][0] <= block["bbox"][1]:
                _add_figure(paras, images, figures.pop(0)[1])
            spans = [s for line in lines for s in line["spans"] if s["text"].strip()]
            plain = " ".join("".join(s["text"] for s in line["spans"]).strip() for line in lines)
            size = statistics.fmean(s["size"] for s in spans) if spans else body
            all_bold = bool(spans) and all(_span_style(s) == "b" for s in spans)
            if len(lines) <= 3 and len(plain) <= 150 and size >= body * 1.15:
                paras.append(_Para([_Run(plain, "")], heading=-1, heading_size=round(size)))
                continue
            if len(lines) == 1 and all_bold and len(plain) <= 80 and not plain.endswith(_TERMINAL[:3]):
                paras.append(_Para([_Run(plain, "")], heading=-1))
                continue
            para = _Para()
            x0 = min(line["bbox"][0] for line in lines)
            for line in lines:
                runs = [_Run(s["text"], _span_style(s)) for s in line["spans"] if s["text"]]
                indent = line["bbox"][0] - x0 > 0.8 * body
                if para.runs and indent and para.plain.endswith(_TERMINAL):
                    paras.append(para)
                    para = _Para()
                _join(para, runs, hyphen_words)
            # A paragraph broken by a page or column continues in lowercase.
            prev = paras[-1] if paras else None
            if (
                prev is not None
                and not prev.heading
                and not prev.image
                and not prev.plain.endswith(_TERMINAL)
                and para.plain[:1].islower()
            ):
                _join(prev, para.runs, hyphen_words)
            else:
                paras.append(para)
        for _, data_png in figures:
            _add_figure(paras, images, data_png)

    # Heading levels: biggest size first; bold run-in headings one level below the smallest size.
    ranked = sorted({p.heading_size for p in paras if p.heading_size}, reverse=True)
    for para in paras:
        if para.heading_size:
            para.heading = min(ranked.index(para.heading_size) + 1, 3)
        elif para.heading:
            para.heading = min(len(ranked) + 1, 4)
    counts = Counter(p.heading for p in paras if p.heading)
    split_level = next((level for level in sorted(counts) if counts[level] >= 2), 0)

    chapters: list[epub.Chapter] = []
    for para in paras:
        starts_chapter = bool(para.heading) and para.heading == split_level
        if not chapters or starts_chapter:
            chapters.append(epub.Chapter(para.plain if starts_chapter else title, ""))
        elif len(chapters[-1].body) > _MAX_CHAPTER_BYTES and not para.heading:
            chapters.append(epub.Chapter(chapters[-1].title, "", in_toc=False))
        chapters[-1].body += para.html() + "\n"
    if not chapters:
        chapters = [epub.Chapter(title, "<p></p>")]
    text = " ".join(all_text)
    book = epub.Book(
        title=title,
        chapters=chapters,
        author=_author(doc),
        language=epub.guess_language(text),
        source=source,
        images=images,
    )
    epub.write_epub(book, out)


def _add_figure(paras: list[_Para], images: dict[str, tuple[bytes, str]], png: bytes) -> None:
    result = optimize_image(png)
    if result is None:
        return
    name = f"figure-{len(images) + 1:03d}.{result.ext}"
    images[name] = (result.data, result.media_type)
    paras.append(_Para(image=name))


# --- Entry point ---------------------------------------------------------------------------


def convert_pdf(path: Path, out_dir: Path, slug: str, mode: Mode | None = None) -> tuple[Path, Verdict]:
    """Convert one PDF. `mode` overrides the classifier. Returns the book file and the verdict."""
    with pymupdf.open(path) as doc:
        verdict = Verdict(mode, "requested") if mode else classify(doc)
        title = _title(doc, path.stem)
        if verdict.mode == "pages":
            out = out_dir / f"{slug}.xtch"
            pdf_to_xtch(doc, out, title)
        else:
            out = out_dir / f"{slug}.epub"
            pdf_to_epub(doc, out, title, source=path.name)
    return out, verdict
