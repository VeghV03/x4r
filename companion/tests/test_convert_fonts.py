from pathlib import Path

import pytest

from x4.convert.fonts import assign_styles, build_font, detect_style, family_name

GEORGIA = Path("/System/Library/Fonts/Supplemental")
GEORGIA_FILES = [GEORGIA / f"Georgia{s}.ttf" for s in ("", " Bold", " Italic", " Bold Italic")]


@pytest.mark.parametrize(
    ("name", "style"),
    [
        ("Literata-Regular.ttf", "regular"),
        ("Literata-BoldItalic.ttf", "bolditalic"),
        ("Georgia Bold Italic.ttf", "bolditalic"),
        ("NotoSerif-Italic[wght].ttf", "italic"),
        ("Inter_Bold.otf", "bold"),
        ("SourceSerif4-SemiBold.ttf", "bold"),
        ("Bookerly.ttf", "regular"),
    ],
)
def test_detect_style(name: str, style: str) -> None:
    assert detect_style(Path(name))[0] == style


def test_plain_weights_win() -> None:
    files = [Path(n) for n in ("F-Light.ttf", "F-Regular.ttf", "F-SemiBold.ttf", "F-Bold.ttf", "F-Italic.ttf")]
    styles = assign_styles(files)
    assert styles == {"regular": Path("F-Regular.ttf"), "bold": Path("F-Bold.ttf"), "italic": Path("F-Italic.ttf")}
    with pytest.raises(ValueError):
        assign_styles([Path("F-Bold.ttf")])


def test_family_name() -> None:
    assert family_name("Source Serif 4") == "SourceSerif4"
    with pytest.raises(ValueError):
        family_name("../")


@pytest.mark.skipif(not all(p.exists() for p in GEORGIA_FILES), reason="Georgia is not installed")
def test_build_georgia(tmp_path: Path) -> None:
    uploads = build_font(GEORGIA_FILES, "Georgia", [14], tmp_path)
    assert [u.remote for u in uploads] == ["/fonts/Georgia/Georgia_14.cpfont"]
    data = uploads[0].local.read_bytes()
    assert data[:8] == b"CPFONT\0\0"  # FontInstaller::CPFONT_MAGIC
    again = build_font(GEORGIA_FILES, "Georgia", [14], tmp_path / "again")
    assert again[0].local.read_bytes() == data
