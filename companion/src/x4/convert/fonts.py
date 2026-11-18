"""`x4 font add`: TTF/OTF files to `.cpfont` packs via the firmware's own fontconvert_sdcard.py.

Output follows firmware/docs/sd-card-fonts.md: `/fonts/<Family>/<Family>_<size>.cpfont`, one
multi-style file per size, family names limited to letters, digits, '-' and '_'
(FontInstaller::isValidFamilyName). Glyphs missing from the font come from the
firmware's bundled Noto Sans, as in build-sd-fonts.py, then from a symbols font (Noto Sans Math) when given.
"""

import re
import subprocess
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

from . import Upload

FIRMWARE = Path(__file__).resolve().parents[4] / "firmware"
SCRIPTS = FIRMWARE / "lib/EpdFont/scripts"
FONTCONVERT = SCRIPTS / "fontconvert_sdcard.py"
FALLBACK = FIRMWARE / "lib/EpdFont/builtinFonts/source/NotoSans"
# Latin plus what notes and science texts use: Greek (Δ, β), super/subscripts, number forms, arrows and maths
# operators (≈, ≥, ±), long arrows and brackets (⟶, ⟨⟩), shapes (●, ■). Codepoints no font has are dropped.
INTERVALS = "latin-ext,greek,(0x2070-0x209F),(0x2150-0x218F),(0x2190-0x21FF),(0x2200-0x22FF),(0x25A0-0x25FF),(0x27E6-0x27FF)"
STYLES = ("regular", "bold", "italic", "bolditalic")
_WEIGHTS = {"regular": 400, "bold": 700, "italic": 400, "bolditalic": 700}
_OTHER_WEIGHTS = {"thin", "hairline", "extralight", "ultralight", "light", "book", "medium", "semi", "demi",
                  "extra", "ultra", "black", "heavy", "condensed", "narrow", "expanded", "display"}  # fmt: skip


def _tokens(stem: str) -> list[str]:
    stem = re.sub(r"\[.*?\]", "", stem)  # variable font axis tags: Font[wght]
    stem = re.sub(r"([a-z])([A-Z])", r"\1 \2", stem)
    return [t.lower() for t in re.split(r"[\s_\-.]+", stem) if t]


def detect_style(path: Path) -> tuple[str, int]:
    """Style from the file name, and how far it is from a plain weight (lower is a better match)."""
    tokens = _tokens(path.stem)
    joined = "".join(tokens)
    italic = any(t in ("italic", "oblique", "it") for t in tokens) or "italic" in joined
    bold = "bold" in tokens or joined.endswith(("bold", "bolditalic", "boldoblique"))
    style = ("bold" if bold else "") + ("italic" if italic else "") or "regular"
    return style, sum(t in _OTHER_WEIGHTS for t in tokens)


def assign_styles(font_files: list[Path]) -> dict[str, Path]:
    """Pick one file per style; 'Bold' beats 'SemiBold', 'Regular' beats 'Light'."""
    best: dict[str, tuple[int, str, Path]] = {}
    for path in font_files:
        style, distance = detect_style(path)
        candidate = (distance, path.name, path)
        if style not in best or candidate < best[style]:
            best[style] = candidate
    if "regular" not in best:
        raise ValueError("no regular font among " + ", ".join(p.name for p in font_files))
    return {style: best[style][2] for style in STYLES if style in best}


def _axes(path: Path) -> dict[str, tuple[float, float, float]]:
    font = TTFont(path)
    return {a.axisTag: (a.minValue, a.defaultValue, a.maxValue) for a in font["fvar"].axes} if "fvar" in font else {}


def _static(path: Path, style: str, workdir: Path, size: int | None = None) -> Path:
    """freetype-py renders only a variable font's default instance, so pin each style's axes.

    Sizes are points at 150 DPI, the physical size on the X4, so an optical size axis is pinned
    to the point size being rendered.
    """
    axes = _axes(path)
    if not axes:
        return path
    pins = {tag: default for tag, (_, default, _) in axes.items()}
    if "wght" in axes:
        lo, _, hi = axes["wght"]
        pins["wght"] = min(max(_WEIGHTS[style], lo), hi)
    if "italic" in style and "ital" in axes:
        pins["ital"] = axes["ital"][2]
    if "italic" in style and "slnt" in axes:
        pins["slnt"] = axes["slnt"][0]
    if size is not None and "opsz" in axes:
        lo, _, hi = axes["opsz"]
        pins["opsz"] = min(max(size, lo), hi)
    out = workdir / "instances" / f"{path.stem}-{style}{'' if size is None else f'-{size}'}.ttf"
    out.parent.mkdir(parents=True, exist_ok=True)
    font = instantiateVariableFont(TTFont(path), pins, updateFontNames=False)
    font.save(out)
    return out


def _complete(styles: dict[str, Path]) -> dict[str, Path]:
    """A variable file whose weight axis reaches bold also covers the bold style."""
    styles = dict(styles)
    for bold, base in (("bold", "regular"), ("bolditalic", "italic")):
        if bold not in styles and base in styles:
            wght = _axes(styles[base]).get("wght")
            if wght and wght[2] >= _WEIGHTS[bold]:
                styles[bold] = styles[base]
    return {style: styles[style] for style in STYLES if style in styles}


def font_family(path: Path) -> str:
    """The family name a font file declares, typographic family first."""
    names = TTFont(path)["name"]
    family = names.getDebugName(16) or names.getDebugName(1)
    if not family:
        raise ValueError(f"{path.name} declares no family name; pass --name")
    return family


def family_name(name: str) -> str:
    family = re.sub(r"[^A-Za-z0-9_-]+", "", name.replace(" ", ""))
    if not family:
        raise ValueError(f"font family name {name!r} has no letters or digits")
    return family


def build_font(
    font_files: list[Path], name: str, sizes: list[int], workdir: Path, symbols: Path | None = None
) -> list[Upload]:
    """Convert a font family to one `.cpfont` per size; returns the files and their SD paths."""
    return build_styles(assign_styles([Path(p) for p in font_files]), name, sizes, workdir, symbols)


def build_styles(
    styles: dict[str, Path], name: str, sizes: list[int], workdir: Path, symbols: Path | None = None
) -> list[Upload]:
    """Convert one file per style (regular, bold, italic, bolditalic) to one `.cpfont` per size.

    `symbols` is the last fallback for every style: maths and arrows neither font has (≻, ∼, ⇒).
    """
    if not FONTCONVERT.exists():
        raise FileNotFoundError(f"firmware font converter not found at {FONTCONVERT}")
    family = family_name(name)
    styles = _complete({style: Path(p).resolve() for style, p in styles.items()})  # the converter runs in SCRIPTS
    out_dir = workdir / "fonts" / family
    out_dir.mkdir(parents=True, exist_ok=True)
    optical = any("opsz" in _axes(path) for path in styles.values())
    for group in [[size] for size in sizes] if optical else [sizes]:
        cmd = [sys.executable, str(FONTCONVERT)]
        for style, path in styles.items():
            cmd += [f"--{style}", str(_static(path, style, workdir, group[0] if optical else None))]
            fallback = FALLBACK / f"NotoSans-{'BoldItalic' if style == 'bolditalic' else style.capitalize()}.ttf"
            if fallback.exists():
                cmd += [f"--fallback-{style}", str(fallback)]
            if symbols:
                cmd += [f"--fallback-{style}", str(Path(symbols).resolve())]
        cmd += ["--intervals", INTERVALS, "--sizes", ",".join(str(s) for s in group), "--name", family]
        cmd += ["--output-dir", str(out_dir) + "/"]
        result = subprocess.run(cmd, cwd=SCRIPTS, capture_output=True, text=True, check=False)
        if result.returncode != 0:
            raise RuntimeError(f"fontconvert_sdcard.py failed:\n{result.stderr.strip()}")
    uploads = []
    for size in sizes:
        local = out_dir / f"{family}_{size}.cpfont"
        if not local.exists():
            raise RuntimeError(f"fontconvert_sdcard.py did not write {local.name}")
        uploads.append(Upload(local, f"/fonts/{family}/{local.name}"))
    return uploads
