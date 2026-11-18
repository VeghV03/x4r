"""Draws the x4r mark into src/images/X4rLogo120.{h,png}.

White lowercase "x4r" in Literata Bold on a black rounded square, 120 px, 1 bit per pixel.
Run: uv run --with pillow python firmware/scripts/make_x4r_logo.py <Literata[opsz,wght].ttf>
(`x4 font install` caches Literata under ~/.cache/x4/fonts/ofl/literata/).
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

N = 120  # px
S = 8  # supersampling
IMAGES = Path(__file__).resolve().parent.parent / "src" / "images"


def draw(font_path: str) -> Image.Image:
    big = Image.new("L", (N * S, N * S), 255)
    d = ImageDraw.Draw(big)
    d.rounded_rectangle([0, 0, N * S - 1, N * S - 1], radius=26 * S, fill=0)
    font = ImageFont.truetype(font_path, 54 * S)
    font.set_variation_by_axes([36, 700])  # opsz, wght
    left, top, right, bottom = d.textbbox((0, 0), "x4r", font=font)
    d.text(((N * S - (right - left)) / 2 - left, (N * S - (bottom - top)) / 2 - top), "x4r", font=font, fill=255)
    return big.resize((N, N), Image.LANCZOS).point(lambda v: 255 if v >= 128 else 0).convert("1")


def header(im: Image.Image) -> str:
    data = bytearray()
    for y in range(N):
        for xb in range(N // 8):
            byte = 0
            for bit in range(8):
                byte = (byte << 1) | (1 if im.getpixel((xb * 8 + bit, y)) else 0)
            data.append(byte)
    rows = ["    " + " ".join(f"0x{b:02x}," for b in data[i : i + 19]) for i in range(0, len(data), 19)]
    return (
        "#pragma once\n#include <cstdint>\n\n"
        '// The x4r mark: white lowercase "x4r" in Literata Bold on a black rounded square.\n'
        "// Image dimensions: 120x120, 1 bit per pixel, MSB first, 1 = white. scripts/make_x4r_logo.py draws it.\n"
        "static const uint8_t X4rLogo120[] = {\n" + "\n".join(rows) + "\n};\n"
    )


if __name__ == "__main__":
    mark = draw(sys.argv[1])
    mark.save(IMAGES / "X4rLogo120.png")
    (IMAGES / "X4rLogo120.h").write_text(header(mark))
