import io

from PIL import Image, ImageDraw

from x4.convert.images import MAX_HEIGHT, MAX_WIDTH, optimize_image


def encode(img: Image.Image, fmt: str, **kwargs: object) -> bytes:
    buf = io.BytesIO()
    img.save(buf, fmt, **kwargs)
    return buf.getvalue()


def photo(width: int, height: int) -> Image.Image:
    img = Image.linear_gradient("L").resize((width, height)).convert("RGB")
    noise = Image.effect_noise((width, height), 40).convert("RGB")
    return Image.blend(img, noise, 0.4)


def test_large_color_photo_becomes_baseline_gray_jpeg() -> None:
    src = encode(photo(2400, 1600), "JPEG", quality=95, progressive=True)
    result = optimize_image(src)
    assert result is not None and result.ext == "jpg" and result.media_type == "image/jpeg"
    out = Image.open(io.BytesIO(result.data))
    assert out.format == "JPEG" and out.mode == "L"
    assert out.size == (MAX_WIDTH, 320)
    assert not out.info.get("progressive") and not out.info.get("progression")
    assert len(result.data) < len(src)


def test_tall_image_fits_height() -> None:
    result = optimize_image(encode(photo(1000, 4000), "PNG"))
    assert result is not None
    assert Image.open(io.BytesIO(result.data)).size == (200, MAX_HEIGHT)


def test_transparent_line_art_becomes_small_16_gray_png() -> None:
    img = Image.new("RGBA", (1200, 900), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    for x in range(0, 1200, 60):
        draw.line((x, 0, 1200 - x, 900), fill=(0, 0, 255, 255), width=4)
    result = optimize_image(encode(img, "PNG"))
    assert result is not None and result.ext == "png"
    out = Image.open(io.BytesIO(result.data))
    assert out.mode == "P" and out.size == (480, 360)
    assert len(out.getpalette() or []) == 16 * 3
    assert out.convert("L").getpixel((0, 359 // 2)) == 255  # transparency became white paper


def test_device_ready_image_is_kept_byte_for_byte() -> None:
    src = encode(photo(300, 200).convert("L"), "JPEG", quality=80)
    result = optimize_image(src)
    assert result is not None and result.data == src


def test_webp_and_gif_are_converted() -> None:
    for fmt in ("WEBP", "GIF"):
        result = optimize_image(encode(photo(600, 400), fmt))
        assert result is not None and result.ext in ("jpg", "png")


def test_svg_is_rasterized() -> None:
    svg = b'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="50"><rect width="50" height="50"/></svg>'
    result = optimize_image(svg)
    assert result is not None
    assert Image.open(io.BytesIO(result.data)).width == MAX_WIDTH


def test_garbage_is_rejected_and_output_is_deterministic() -> None:
    assert optimize_image(b"not an image") is None
    src = encode(photo(1200, 900), "PNG")
    assert optimize_image(src) == optimize_image(src)
