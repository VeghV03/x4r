"""SD card file names."""

import re
import unicodedata

MAX_SLUG = 60


def slugify(text: str, fallback: str = "untitled") -> str:
    """ASCII, lowercase, words joined by '-': 'Az ősz Útja!' -> 'az-osz-utja'."""
    ascii_text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    slug = re.sub(r"[^a-z0-9]+", "-", ascii_text.lower()).strip("-")
    if len(slug) > MAX_SLUG:
        slug = slug[:MAX_SLUG].rsplit("-", 1)[0] or slug[:MAX_SLUG]
    return slug or fallback
