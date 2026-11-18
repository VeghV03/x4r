"""Image optimization for the X4's on-device decoders.

The firmware decodes JPEG and PNG to 8-bit gray, scales to the text column and
then applies its own 4x4 Bayer dither to 4 levels (ImageBlock.cpp always sets
useDithering). Pre-dithering here would be dithered twice, so images are sent
as clean 8-bit gray at no more than screen size: one gray channel is the fastest
JPEG decode, no downscale pass is needed, and near-white/near-black are snapped
so paper and ink stay free of dither noise (values >= 232 or <= 24 never flip).
"""

import io
from dataclasses import dataclass

import pymupdf
from PIL import Image, ImageFilter, ImageOps

MAX_WIDTH, MAX_HEIGHT = 480, 800
JPEG_QUALITY = 85
GRAPHIC_PAPER_INK = 0.8  # share of pure white/black pixels above which an image is line art
_WHITE, _BLACK = 232, 24
_TONE = [
    0 if v <= _BLACK else 255 if v >= _WHITE else round((v - _BLACK) * 255 / (_WHITE - _BLACK)) for v in range(256)
]


@dataclass(frozen=True)
class OptimizedImage:
    data: bytes
    ext: str  # "jpg" or "png"
    media_type: str


def _open(data: bytes) -> Image.Image | None:
    head = data[:1024].lstrip()
    if head.startswith(b"<?xml") and b"<svg" in head or head.startswith(b"<svg"):
        return _render_svg(data)
    try:
        img = Image.open(io.BytesIO(data))
        img.seek(0)
        img.load()
    except (OSError, ValueError, Image.DecompressionBombError):
        return None
    return img


def _render_svg(data: bytes) -> Image.Image | None:
    try:
        doc = pymupdf.open(stream=data, filetype="svg")
        page = doc[0]
        zoom = max(1.0, MAX_WIDTH / max(page.rect.width, 1))
        pix = page.get_pixmap(matrix=pymupdf.Matrix(zoom, zoom), alpha=False)
        return Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
    except Exception:  # noqa: BLE001 - MuPDF raises its own types for bad SVG
        return None


def to_gray(img: Image.Image) -> Image.Image:
    """Flatten transparency onto white paper and drop color."""
    img = ImageOps.exif_transpose(img)
    if img.mode in ("RGBA", "LA", "PA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        paper = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
        img = Image.alpha_composite(paper, rgba)
    return img.convert("L")


def fit(img: Image.Image, width: int = MAX_WIDTH, height: int = MAX_HEIGHT) -> Image.Image:
    """Downscale to fit inside width x height, never upscale."""
    scale = min(width / img.width, height / img.height)
    if scale >= 1:
        return img
    size = (max(1, round(img.width * scale)), max(1, round(img.height * scale)))
    img = img.resize(size, Image.Resampling.LANCZOS)
    return img.filter(ImageFilter.UnsharpMask(radius=1, percent=50, threshold=2))


def _already_fine(img: Image.Image) -> bool:
    if img.width > MAX_WIDTH or img.height > MAX_HEIGHT:
        return False
    if img.format == "JPEG":
        return img.mode == "L" and not img.info.get("progressive") and not img.info.get("progression")
    if img.format == "PNG":
        return img.mode in ("1", "L") and not img.info.get("interlace")
    return False


def optimize_image(data: bytes) -> OptimizedImage | None:
    """Return a device-ready gray JPEG (photos) or 16-gray PNG (line art), or None if undecodable."""
    img = _open(data)
    if img is None:
        return None
    if _already_fine(img):
        ext = "jpg" if img.format == "JPEG" else "png"
        return OptimizedImage(data, ext, "image/jpeg" if ext == "jpg" else "image/png")
    gray = fit(to_gray(img)).point(_TONE)
    hist = gray.histogram()
    out = io.BytesIO()
    if hist[0] + hist[255] >= GRAPHIC_PAPER_INK * gray.width * gray.height:
        indexes = gray.point([round(v / 17) for v in range(256)]).tobytes()
        indexed = Image.frombytes("P", gray.size, indexes)
        indexed.putpalette([v for i in range(16) for v in (i * 17,) * 3])
        indexed.save(out, "PNG", bits=4, optimize=True)
        return OptimizedImage(out.getvalue(), "png", "image/png")
    gray.save(out, "JPEG", quality=JPEG_QUALITY, optimize=True, progressive=False)
    return OptimizedImage(out.getvalue(), "jpg", "image/jpeg")
