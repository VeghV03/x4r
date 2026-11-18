"""Google Fonts by family name, fetched from the google/fonts repository on GitHub.

A family lives in `<license>/<slug>/` (ofl, apache or ufl; the slug is the name in lowercase
letters and digits) with a METADATA.pb that lists each file's style and weight. Downloads are
kept in the cache folder, so a family is fetched once.
"""

import difflib
import json
import re
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import quote

import httpx

from ..errors import InvalidData, X4Error
from . import Upload, net
from .fonts import STYLES, build_styles, family_name

RAW = "https://raw.githubusercontent.com/google/fonts/main"
TREES = "https://api.github.com/repos/google/fonts/git/trees/main"
LICENSES = ("ofl", "apache", "ufl")
CURATED = ("Literata", "Charis SIL", "Source Serif 4", "EB Garamond", "Bitter", "Atkinson Hyperlegible")
# Fills in the maths and arrows a reading font and Noto Sans lack.
SYMBOLS = "Noto Sans Math"
_TARGETS = {
    "regular": ("normal", 400),
    "bold": ("normal", 700),
    "italic": ("italic", 400),
    "bolditalic": ("italic", 700),
}


@dataclass(frozen=True)
class Family:
    name: str  # as Google Fonts shows it, e.g. "Source Serif 4"
    folder: str  # e.g. "ofl/sourceserif4"
    styles: dict[str, str]  # style -> file name in the folder


def slug(name: str) -> str:
    return re.sub(r"[^a-z0-9]", "", name.lower())


def _get(url: str) -> bytes | None:
    """The body, or None for 404."""
    try:
        return net.fetch(url)
    except httpx.HTTPStatusError as e:
        if e.response.status_code == 404:
            return None
        raise X4Error(f"{url}: HTTP {e.response.status_code}; try again later") from e
    except httpx.HTTPError as e:
        raise X4Error(f"{url}: {e}; check the network connection") from e


def parse_metadata(text: str) -> tuple[str, list[tuple[str, int, str]]]:
    """Family name and (style, weight, filename) per file from a METADATA.pb."""
    name = re.search(r'^name:\s*"([^"]+)"', text, re.M)
    fonts = []
    for block in re.findall(r"^fonts\s*\{(.*?)^\}", text, re.M | re.S):
        style = re.search(r'style:\s*"([^"]+)"', block)
        weight = re.search(r"weight:\s*(\d+)", block)
        filename = re.search(r'filename:\s*"([^"]+)"', block)
        if style and weight and filename:
            fonts.append((style.group(1), int(weight.group(1)), filename.group(1)))
    if not name or not fonts:
        raise X4Error("METADATA.pb lists no family name or fonts")
    return name.group(1), fonts


def pick_styles(fonts: list[tuple[str, int, str]]) -> dict[str, str]:
    """One file per style: the exact weight, else a variable file of the same slope."""
    styles = {}
    for style, (slope, weight) in _TARGETS.items():
        same = [(abs(w - weight), f) for s, w, f in fonts if s == slope]
        exact = [f for d, f in same if d == 0]
        variable = sorted((d, f) for d, f in same if "[" in f)
        if exact or variable:
            styles[style] = exact[0] if exact else variable[0][1]
        elif style == "regular" and same:
            styles[style] = min(same)[1]
    if "regular" not in styles:
        raise X4Error("the family has no upright style")
    return {style: styles[style] for style in STYLES if style in styles}


def _metadata(name: str, cache: Path) -> tuple[str, str] | None:
    """(folder, METADATA.pb text) for a family name, from the cache or GitHub; None if unknown."""
    key = slug(name)
    if not key:
        return None
    for lic in LICENSES:
        path = cache / lic / key / "METADATA.pb"
        if path.exists():
            return f"{lic}/{key}", path.read_text(encoding="utf-8")
    for lic in LICENSES:
        data = _get(f"{RAW}/{lic}/{key}/METADATA.pb")
        if data is not None:
            path = cache / lic / key / "METADATA.pb"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            return f"{lic}/{key}", data.decode("utf-8")
    return None


def _all_slugs(cache: Path) -> list[str]:
    """Every family folder in google/fonts (three GitHub API calls, cached). Empty on failure."""
    path = cache / "families.json"
    if path.exists():
        return json.loads(path.read_text())
    try:
        root = json.loads(net.fetch(TREES))
        slugs = []
        for entry in root["tree"]:
            if entry["path"] in LICENSES:
                tree = json.loads(net.fetch(f"{TREES.rsplit('/', 1)[0]}/{entry['sha']}"))
                slugs += [e["path"] for e in tree["tree"] if e["type"] == "tree"]
    except (httpx.HTTPError, ValueError, KeyError):
        return []
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(sorted(set(slugs))))
    return sorted(set(slugs))


def suggest(name: str, cache: Path) -> list[str]:
    """Up to three Google Fonts names close to `name`."""
    key = slug(name)
    slugs = _all_slugs(cache)
    close = difflib.get_close_matches(key, slugs, n=3, cutoff=0.7)
    close += [s for s in slugs if key and key in s and s not in close]
    names = []
    for s in close[:3]:
        found = _metadata(s, cache)
        names.append(parse_metadata(found[1])[0] if found else s)
    return names


def resolve(name: str, cache: Path) -> Family:
    found = _metadata(name, cache)
    if found is None:
        matches = suggest(name, cache)
        hint = f"did you mean {', '.join(matches)}?" if matches else "check the name at fonts.google.com"
        raise InvalidData(f"{name}: not a Google Fonts family", [hint])
    folder, text = found
    family, fonts = parse_metadata(text)
    return Family(family, folder, pick_styles(fonts))


def download(family: Family, cache: Path) -> dict[str, Path]:
    """The family's style files in the cache, fetched if missing."""
    out = {}
    for style, filename in family.styles.items():
        path = cache / family.folder / filename
        if not path.exists():
            data = _get(f"{RAW}/{family.folder}/{quote(filename)}")
            if data is None:
                raise X4Error(f"{family.folder}/{filename}: listed in METADATA.pb but missing on GitHub")
            part = path.with_name(path.name + ".part")
            part.write_bytes(data)
            part.replace(path)
        out[style] = path
    return out


def symbols(cache: Path) -> Path | None:
    """The symbols font in the cache, fetched if missing; None when it cannot be fetched."""
    try:
        return download(resolve(SYMBOLS, cache), cache)["regular"]
    except X4Error:
        return None


def build_google_font(
    name: str, sizes: list[int], cache: Path, workdir: Path, family: str | None = None
) -> tuple[str, list[Upload]]:
    """Fetch a Google Fonts family and convert it; returns the device family name and the files."""
    found = resolve(name, cache)
    family = family_name(family or found.name)
    return family, build_styles(download(found, cache), family, sizes, workdir, symbols(cache))
