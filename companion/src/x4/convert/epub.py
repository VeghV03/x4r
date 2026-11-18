"""Deterministic EPUB 3 writing, and in-place image optimization of existing EPUBs."""

import posixpath
import re
import uuid
import zipfile
from dataclasses import dataclass, field
from html import escape
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit

from lxml import etree

from .images import optimize_image

_ZIP_TIME = (1980, 1, 1, 0, 0, 0)
_CSS = """body { margin: 0; }
h1, h2, h3 { text-align: left; }
figure { margin: 1em 0; text-align: center; }
img { max-width: 100%; }
figcaption { font-style: italic; }
blockquote { margin: 1em 1.5em; }
"""


@dataclass
class Chapter:
    title: str
    body: str  # well-formed XHTML for inside <body>
    in_toc: bool = True


@dataclass
class Book:
    title: str
    chapters: list[Chapter]
    author: str = ""
    language: str = "en"
    date: str = ""  # ISO date, optional
    source: str = ""  # URL or file the book came from
    images: dict[str, tuple[bytes, str]] = field(default_factory=dict)  # name -> (data, media type)


def guess_language(text: str) -> str:
    """'hu' when the Hungarian-only letters ő and ű show up, else 'en' (sets device hyphenation)."""
    letters = sum(ch.isalpha() for ch in text) or 1
    return "hu" if sum(ch in "őűŐŰ" for ch in text) / letters > 0.002 else "en"


def write_zip(path: Path, entries: list[tuple[str, bytes]]) -> None:
    """EPUB-shaped zip with fixed timestamps: same entries in, same bytes out."""
    with zipfile.ZipFile(path, "w") as zf:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=_ZIP_TIME)
            info.external_attr = 0o644 << 16
            if name == "mimetype":
                info.compress_type = zipfile.ZIP_STORED
            else:
                info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, data, compresslevel=9 if info.compress_type == zipfile.ZIP_DEFLATED else None)


def _xhtml(title: str, body: str, language: str) -> bytes:
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE html>\n'
        f'<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" '
        f'xml:lang="{language}" lang="{language}">\n'
        f'<head><meta charset="utf-8"/><title>{escape(title)}</title>'
        '<link rel="stylesheet" type="text/css" href="style.css"/></head>\n'
        f"<body>\n{body}\n</body>\n</html>\n"
    ).encode()


def write_epub(book: Book, path: Path) -> None:
    identifier = "urn:uuid:" + str(uuid.uuid5(uuid.NAMESPACE_URL, book.source or book.title))
    modified = (book.date[:10] if re.match(r"\d{4}-\d{2}-\d{2}", book.date) else "2000-01-01") + "T00:00:00Z"
    files = [f"chapter-{i + 1:03d}.xhtml" for i in range(len(book.chapters))]
    toc = [(name, ch.title) for name, ch in zip(files, book.chapters, strict=True) if ch.in_toc and ch.title]
    if not toc:
        toc = [(files[0], book.title)]

    manifest = [
        '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
        '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
        '<item id="css" href="style.css" media-type="text/css"/>',
    ]
    manifest += [f'<item id="c{i + 1}" href="{n}" media-type="application/xhtml+xml"/>' for i, n in enumerate(files)]
    manifest += [
        f'<item id="img{i + 1}" href="images/{quote(n)}" media-type="{mt}"/>'
        for i, (n, (_, mt)) in enumerate(book.images.items())
    ]
    spine = "".join(f'<itemref idref="c{i + 1}"/>' for i in range(len(files)))
    meta = [
        f'<dc:identifier id="bookid">{identifier}</dc:identifier>',
        f"<dc:title>{escape(book.title)}</dc:title>",
        f"<dc:language>{book.language}</dc:language>",
        f'<meta property="dcterms:modified">{modified}</meta>',
    ]
    if book.author:
        meta.append(f"<dc:creator>{escape(book.author)}</dc:creator>")
    if book.date:
        meta.append(f"<dc:date>{escape(book.date)}</dc:date>")
    if book.source:
        meta.append(f"<dc:source>{escape(book.source)}</dc:source>")
    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n' + "\n".join(meta) + "\n</metadata>\n"
        "<manifest>\n" + "\n".join(manifest) + "\n</manifest>\n"
        f'<spine toc="ncx">{spine}</spine>\n</package>\n'
    )
    nav_items = "".join(f'<li><a href="{n}">{escape(t)}</a></li>' for n, t in toc)
    nav = _xhtml(book.title, f'<nav epub:type="toc" id="toc"><ol>{nav_items}</ol></nav>', book.language)
    points = "".join(
        f'<navPoint id="p{i + 1}" playOrder="{i + 1}"><navLabel><text>{escape(t)}</text></navLabel>'
        f'<content src="{n}"/></navPoint>'
        for i, (n, t) in enumerate(toc)
    )
    ncx = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
        f'<head><meta name="dtb:uid" content="{identifier}"/></head>'
        f"<docTitle><text>{escape(book.title)}</text></docTitle><navMap>{points}</navMap></ncx>\n"
    )
    container = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        '<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
        "</rootfiles></container>\n"
    )
    entries = [
        ("mimetype", b"application/epub+zip"),
        ("META-INF/container.xml", container.encode()),
        ("OEBPS/content.opf", opf.encode()),
        ("OEBPS/nav.xhtml", nav),
        ("OEBPS/toc.ncx", ncx.encode()),
        ("OEBPS/style.css", _CSS.encode()),
    ]
    entries += [
        (f"OEBPS/{n}", _xhtml(ch.title or book.title, ch.body, book.language))
        for n, ch in zip(files, book.chapters, strict=True)
    ]
    entries += [(f"OEBPS/images/{n}", data) for n, (data, _) in book.images.items()]
    write_zip(path, entries)


# --- Optimizing an existing EPUB -------------------------------------------------------------

_OPF_NS = "http://www.idpf.org/2007/opf"
_CONTAINER_NS = "urn:oasis:names:tc:opendocument:xmlns:container"
_IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".tif", ".tiff", ".svg"}
_TEXT_EXTS = {".xhtml", ".html", ".htm", ".xml", ".css", ".svg", ".ncx", ".opf"}
_ATTR_REF = re.compile(rb"""((?:src|href|xlink:href|poster)\s*=\s*)(["'])(.*?)\2""", re.DOTALL)
_CSS_REF = re.compile(rb"""(url\(\s*)(["']?)([^"')]+)\2(\s*\))""")


def _opf_path(zf: zipfile.ZipFile) -> str:
    root = etree.fromstring(zf.read("META-INF/container.xml"))
    rootfile = root.find(f".//{{{_CONTAINER_NS}}}rootfile")
    if rootfile is None:
        raise ValueError("EPUB has no rootfile in META-INF/container.xml")
    return rootfile.get("full-path", "")


def _rewrite_refs(data: bytes, doc: str, renames: dict[str, str]) -> bytes:
    base = posixpath.dirname(doc)

    def fix(ref: bytes) -> bytes:
        text = ref.decode(errors="ignore")
        parts = urlsplit(text)
        if parts.scheme or parts.netloc or not parts.path:
            return ref
        target = posixpath.normpath(posixpath.join(base, unquote(parts.path)))
        if target not in renames:
            return ref
        new = quote(posixpath.relpath(renames[target], base or "."))
        return (new + (f"#{parts.fragment}" if parts.fragment else "")).encode()

    data = _ATTR_REF.sub(lambda m: m[1] + m[2] + fix(m[3]) + m[2], data)
    return _CSS_REF.sub(lambda m: m[1] + m[2] + fix(m[3]) + m[2] + m[4], data)


def optimize_epub(src: Path, dst: Path) -> int:
    """Copy an EPUB, replacing every manifest image with a device-ready one. Returns images replaced.

    Everything else is copied unchanged. When an image changes format (say WebP -> JPEG) it
    gets the new extension, and the manifest and every reference to it are rewritten.
    """
    with zipfile.ZipFile(src) as zf:
        names = zf.namelist()
        contents = {name: zf.read(name) for name in names}
        opf_name = _opf_path(zf)
    opf_dir = posixpath.dirname(opf_name)
    opf = etree.fromstring(contents[opf_name])
    renames: dict[str, str] = {}
    replaced = 0
    opf_changed = False
    taken = set(names)
    for item in opf.iter(f"{{{_OPF_NS}}}item"):
        href = item.get("href", "")
        media = item.get("media-type", "")
        name = posixpath.normpath(posixpath.join(opf_dir, unquote(href)))
        ext = posixpath.splitext(name)[1].lower()
        if name not in contents or not (media.startswith("image/") or ext in _IMAGE_EXTS):
            continue
        result = optimize_image(contents[name])
        if result is None:
            continue
        if result.data != contents[name]:
            replaced += 1
        same_ext = ext == f".{result.ext}" or (ext == ".jpeg" and result.ext == "jpg")
        new_name = name
        if not same_ext:
            stem = posixpath.splitext(name)[0]
            new_name = f"{stem}.{result.ext}"
            if new_name in taken:
                new_name = f"{stem}-x4.{result.ext}"
            taken.add(new_name)
            renames[name] = new_name
            new_href = posixpath.splitext(href)[0] + posixpath.splitext(new_name)[1]
            item.set("href", new_href)
            opf_changed = True
        if media != result.media_type:
            item.set("media-type", result.media_type)
            opf_changed = True
        contents[name] = result.data
    if renames:
        for name in names:
            is_text = posixpath.splitext(name)[1].lower() in _TEXT_EXTS and name != opf_name
            if is_text and any(posixpath.basename(old).encode() in contents[name] for old in renames):
                contents[name] = _rewrite_refs(contents[name], name, renames)
    if opf_changed:
        contents[opf_name] = etree.tostring(opf, xml_declaration=True, encoding="utf-8")
    order = ["mimetype"] + [n for n in names if n != "mimetype"]
    contents.setdefault("mimetype", b"application/epub+zip")
    write_zip(dst, [(renames.get(n, n), contents[n]) for n in order])
    return replaced
