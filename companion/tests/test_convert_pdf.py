import io
import re
import zipfile
from pathlib import Path

import pymupdf
import pytest
from PIL import Image
from test_convert_xtc import parse

from x4.convert import pdf
from x4.convert.books import prepare_book

BODY = (
    "Reading on paper rewards patience, and the habit of slow attention grows with practice. "
    "Each page asks for a little more focus than the one before it, and the reader learns to wait. "
)


def text_lines(page: pymupdf.Page, lines: list[str], y: float, size: float = 11) -> float:
    for line in lines:
        page.insert_text((72, y), line, fontsize=size)
        y += size * 1.4
    return y


def wrap(text: str, width: int = 80) -> list[str]:
    lines, current = [], ""
    for word in text.split():
        if len(current) + len(word) + 1 > width:
            lines.append(current)
            current = word
        else:
            current = f"{current} {word}".strip()
    return lines + [current]


def make_text_pdf(path: Path) -> None:
    doc = pymupdf.open()
    for number in range(1, 7):
        page = doc.new_page(width=595, height=842)
        page.insert_text((72, 40), "Slow Reading - A Field Guide", fontsize=9)
        page.insert_text((290, 815), str(number), fontsize=9)
        y = 100
        if number == 6:
            y = text_lines(page, ["continues on the next page without a break."], 80) + 14
        if number in (1, 4):
            page.insert_text((72, y), f"Chapter {1 if number == 1 else 2}", fontsize=22)
            y += 50
        if number == 2:
            y = text_lines(page, ["A well-known habit helps. This sentence has an extra-",
                                  "ordinary word, and a well-", "known phrase that keeps its hyphen."], y)  # fmt: skip
            y += 14
            img = Image.linear_gradient("L").resize((900, 600))
            buf = io.BytesIO()
            img.convert("RGB").save(buf, "JPEG")
            page.insert_image(pymupdf.Rect(72, y, 372, y + 200), stream=buf.getvalue())
            y += 220
        for _ in range(4):
            y = text_lines(page, wrap(BODY * 3), y) + 14
        if number == 5:
            text_lines(page, ["This paragraph runs to the end of the page and its sentence"], 790)
    doc.set_toc([[1, "Chapter 1", 1], [1, "Chapter 2", 4]])
    doc.save(path)


def make_figure_pdf(path: Path) -> None:
    doc = pymupdf.open()
    for number in range(3):
        page = doc.new_page(width=595, height=842)
        page.insert_text((72, 60), f"Figure {number + 1}", fontsize=11)
        for i in range(160):
            page.draw_line((72 + i * 2.8, 100), (523 - i * 2, 700))
    doc.set_toc([[1, "Figures", 1], [1, "More figures", 3]])
    doc.save(path)


def make_scanned_pdf(path: Path) -> None:
    doc = pymupdf.open()
    img = Image.effect_noise((850, 1100), 60).convert("RGB")
    buf = io.BytesIO()
    img.save(buf, "JPEG")
    for _ in range(2):
        doc.new_page(width=595, height=842).insert_image(pymupdf.Rect(0, 0, 595, 842), stream=buf.getvalue())
    doc.save(path)


def make_math_pdf(path: Path) -> None:
    doc = pymupdf.open()
    page = doc.new_page(width=595, height=842)
    y = text_lines(page, wrap(BODY * 2), 80)
    for i in range(40):
        page.insert_text((72, y + i * 14), "a b c d e f g h i j", fontsize=11, fontname="Symbol")
    doc.save(path)


@pytest.fixture
def pdfs(tmp_path: Path) -> dict[str, Path]:
    makers = {"text": make_text_pdf, "figure": make_figure_pdf, "scanned": make_scanned_pdf, "math": make_math_pdf}
    paths = {}
    for name, make in makers.items():
        paths[name] = tmp_path / f"{name}.pdf"
        make(paths[name])
    return paths


def test_classifier(pdfs: dict[str, Path]) -> None:
    verdicts = {name: pdf.classify(pymupdf.open(path)) for name, path in pdfs.items()}
    assert verdicts["text"].mode == "reflow" and verdicts["text"].reason.startswith("prose")
    assert verdicts["figure"].mode == "pages" and verdicts["figure"].reason.startswith("figures")
    assert verdicts["scanned"].mode == "pages" and verdicts["scanned"].reason.startswith("scanned")
    assert verdicts["math"].mode == "pages" and verdicts["math"].reason.startswith("math")


def test_reflow_to_clean_epub(pdfs: dict[str, Path], tmp_path: Path) -> None:
    uploads = prepare_book(str(pdfs["text"]), tmp_path / "out")
    assert [u.remote for u in uploads] == ["/books/text.epub"]
    with zipfile.ZipFile(uploads[0].local) as zf:
        chapters = sorted(n for n in zf.namelist() if n.startswith("OEBPS/chapter-"))
        html = "".join(zf.read(n).decode() for n in chapters)
        figures = [n for n in zf.namelist() if n.startswith("OEBPS/images/")]
        assert len(figures) == 1
        assert Image.open(io.BytesIO(zf.read(figures[0]))).width <= 480
    assert len(chapters) == 2
    assert "<h1>Chapter 1</h1>" in html and "<h1>Chapter 2</h1>" in html
    assert "Slow Reading - A Field Guide" not in html  # running header
    assert not re.search(r"<p>\d+</p>", html)  # page numbers
    assert "extraordinary word" in html
    assert "a well-known phrase" in html  # a hyphen that belongs to the word stays
    assert "its sentence continues on the next page" in html  # paragraph joined across pages
    assert '<img src="images/' in html


def test_override_suffix_renders_pages(pdfs: dict[str, Path], tmp_path: Path) -> None:
    uploads = prepare_book(f"{pdfs['text']}#pages", tmp_path / "out")
    assert [u.remote for u in uploads] == ["/books/text.xtch"]
    book = parse(uploads[0].local)
    assert book.page_count == 6
    assert book.chapters == [("Chapter 1", 0, 2), ("Chapter 2", 3, 5)]
    assert all((w, h) == (480, 800) for w, h, _ in book.pages)


def test_figure_pdf_becomes_xtch(pdfs: dict[str, Path], tmp_path: Path) -> None:
    uploads = prepare_book(str(pdfs["figure"]), tmp_path / "out")
    assert uploads[0].remote == "/books/figure.xtch"
    book = parse(uploads[0].local)
    assert book.title == "figure"
    assert book.chapters == [("Figures", 0, 1), ("More figures", 2, 2)]
    again = prepare_book(str(pdfs["figure"]), tmp_path / "again")
    assert again[0].local.read_bytes() == uploads[0].local.read_bytes()


def test_reflow_override_and_bad_mode(pdfs: dict[str, Path], tmp_path: Path) -> None:
    assert prepare_book(f"{pdfs['figure']}#reflow", tmp_path / "out")[0].remote == "/books/figure.epub"
    with pytest.raises(ValueError):
        prepare_book(f"{pdfs['figure']}#sideways", tmp_path / "out")
