"""Google Fonts by name and `x4 font install`, with GitHub replaced by a dict of URLs."""

import json
from pathlib import Path

import httpx
import pytest

from x4 import ops
from x4.convert import Upload, fonts, gfonts, net
from x4.errors import InvalidData, X4Error

NOTO = fonts.FALLBACK


def _metadata(name: str, entries: list[tuple[str, int, str]]) -> bytes:
    blocks = "".join(
        f'fonts {{\n  name: "{name}"\n  style: "{s}"\n  weight: {w}\n  filename: "{f}"\n  copyright: "x"\n}}\n'
        for s, w, f in entries
    )
    return f'name: "{name}"\nlicense: "OFL"\n{blocks}subsets: "latin-ext"\naxes {{\n  tag: "wght"\n}}\n'.encode()


VARIABLE = [("normal", 400, "Literata[opsz,wght].ttf"), ("italic", 400, "Literata-Italic[opsz,wght].ttf")]
STATIC = [
    ("normal", 400, "NotoSans-Regular.ttf"),
    ("italic", 400, "NotoSans-Italic.ttf"),
    ("normal", 700, "NotoSans-Bold.ttf"),
    ("italic", 700, "NotoSans-BoldItalic.ttf"),
    ("normal", 300, "NotoSans-Light.ttf"),
]


SYMBOLS = ("normal", 400, "NotoSansMath-Regular.ttf")


class Files(dict):
    requests: list[str]

    def __init__(self) -> None:
        super().__init__()
        self.requests = []


@pytest.fixture
def github(monkeypatch, tmp_path):
    """URL -> bytes; anything else is a 404. `github.requests` lists what was fetched."""
    files = Files()

    def fetch(url: str) -> bytes:
        files.requests.append(url)
        if url in files:
            return files[url]
        request = httpx.Request("GET", url)
        raise httpx.HTTPStatusError("404", request=request, response=httpx.Response(404, request=request))

    monkeypatch.setattr(net, "fetch", fetch)
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    return files


@pytest.fixture
def noto(github):
    """Noto Sans served as ofl/notosans, from the firmware's copy, and standing in for the symbols font."""
    github[f"{gfonts.RAW}/ofl/notosans/METADATA.pb"] = _metadata("Noto Sans", STATIC)
    for _, _, f in STATIC[:4]:
        github[f"{gfonts.RAW}/ofl/notosans/{f}"] = (NOTO / f).read_bytes()
    github[f"{gfonts.RAW}/ofl/notosansmath/METADATA.pb"] = _metadata("Noto Sans Math", [SYMBOLS])
    github[f"{gfonts.RAW}/ofl/notosansmath/{SYMBOLS[2]}"] = (NOTO / "NotoSans-Regular.ttf").read_bytes()
    return github


def test_slug() -> None:
    assert [gfonts.slug(n) for n in gfonts.CURATED] == [
        "literata", "charissil", "sourceserif4", "ebgaramond", "bitter", "atkinsonhyperlegible",
    ]  # fmt: skip


def test_pick_styles() -> None:
    name, entries = gfonts.parse_metadata(_metadata("Literata", VARIABLE).decode())
    assert name == "Literata"
    assert gfonts.pick_styles(entries) == {
        "regular": "Literata[opsz,wght].ttf",
        "bold": "Literata[opsz,wght].ttf",
        "italic": "Literata-Italic[opsz,wght].ttf",
        "bolditalic": "Literata-Italic[opsz,wght].ttf",
    }
    assert gfonts.pick_styles(STATIC)["bold"] == "NotoSans-Bold.ttf"
    assert gfonts.pick_styles([("normal", 300, "F-Light.ttf"), ("normal", 900, "F-Black.ttf")]) == {
        "regular": "F-Light.ttf"
    }
    with pytest.raises(X4Error, match="no upright style"):
        gfonts.pick_styles([("italic", 400, "F-Italic.ttf")])


def test_complete_uses_a_variable_file_for_bold(monkeypatch) -> None:
    monkeypatch.setattr(fonts, "_axes", lambda p: {"wght": (200, 400, 900)} if "[" in p.name else {})
    styles = {"regular": Path("F[wght].ttf"), "italic": Path("F-Italic.ttf")}
    assert fonts._complete(styles) == {"regular": Path("F[wght].ttf"), "bold": Path("F[wght].ttf"),
                                       "italic": Path("F-Italic.ttf")}  # fmt: skip


def test_font_sources(tmp_path: Path) -> None:
    font = tmp_path / "A.ttf"
    font.write_bytes(b"")
    assert ops.font_sources([str(font)]) == ([font], None)
    assert ops.font_sources(["Crimson", "Pro"]) == ([], "Crimson Pro")
    with pytest.raises(X4Error, match="B.ttf: no such file"):
        ops.font_sources(["B.ttf"])
    with pytest.raises(X4Error, match="Lora: no such file"):
        ops.font_sources([str(font), "Lora"])


def test_add_google_font(x4, sim, noto, tmp_path) -> None:
    r = x4("font", "add", "Noto", "Sans", "--sizes", "12")
    assert r.exit_code == 0, r.output
    pack = sim.root / "fonts/NotoSans/NotoSans_12.cpfont"
    assert pack.read_bytes()[:8] == b"CPFONT\0\0"
    assert r.output.startswith("/fonts/NotoSans/NotoSans_12.cpfont  ")
    assert not any("Light" in url for url in noto.requests)
    cached = tmp_path / "cache/x4/fonts/ofl/notosans"
    assert sorted(p.name for p in cached.iterdir()) == ["METADATA.pb", *sorted(f for _, _, f in STATIC[:4])]
    assert (tmp_path / "cache/x4/fonts/ofl/notosansmath" / SYMBOLS[2]).exists()

    noto.requests.clear()
    r = x4("--json", "font", "add", "notosans", "--name", "Body", "--sizes", "12")
    assert json.loads(r.output)["family"] == "Body"
    assert (sim.root / "fonts/Body/Body_12.cpfont").exists()
    assert noto.requests == []  # all from the cache


def test_symbols_font_is_the_last_fallback(monkeypatch, noto, tmp_path) -> None:
    calls = []
    monkeypatch.setattr(fonts.subprocess, "run", lambda cmd, **kw: calls.append(cmd) or _Done())
    with pytest.raises(RuntimeError, match="did not write"):
        gfonts.build_google_font("Noto Sans", [12], tmp_path / "cache", tmp_path / "work")
    cmd = calls[0]
    regular = [cmd[i + 1] for i, arg in enumerate(cmd) if arg == "--fallback-regular"]
    assert [Path(p).name for p in regular] == ["NotoSans-Regular.ttf", SYMBOLS[2]]


def test_no_symbols_font_offline(github, tmp_path) -> None:
    assert gfonts.symbols(tmp_path) is None


class _Done:
    returncode = 0
    stderr = ""


def test_unknown_family_suggests_names(x4, github) -> None:
    trees = "https://api.github.com/repos/google/fonts/git/trees"
    github[f"{trees}/main"] = json.dumps({"tree": [{"path": "ofl", "sha": "abc"}, {"path": "OFL.txt"}]}).encode()
    entries = [{"path": p, "type": "tree"} for p in ("crimsonpro", "crimsontext", "lora")]
    github[f"{trees}/abc"] = json.dumps({"tree": entries}).encode()
    github[f"{gfonts.RAW}/ofl/crimsonpro/METADATA.pb"] = _metadata("Crimson Pro", VARIABLE)
    r = x4("font", "add", "Crimsn", "Pro")
    assert r.exit_code == 2
    assert "Crimsn Pro: not a Google Fonts family\ndid you mean Crimson Pro" in r.output
    with pytest.raises(InvalidData) as e:
        gfonts.resolve("Zzzz", ops._cache("fonts"))
    assert e.value.details == ["check the name at fonts.google.com"]


def test_network_errors_name_the_fix(monkeypatch, tmp_path) -> None:
    def down(url: str) -> bytes:
        raise httpx.ConnectError("no route to host")

    monkeypatch.setattr(net, "fetch", down)
    with pytest.raises(X4Error, match="no route to host; check the network connection"):
        gfonts.resolve("Lora", tmp_path)


def test_install(x4, sim, monkeypatch) -> None:
    built: list[str] = []

    def build(name, sizes, cache, workdir, family):
        built.append(name)
        uploads = []
        for size in sizes:
            local = workdir / f"{family}_{size}.cpfont"
            local.write_bytes(b"x" * 1024 * size)
            uploads.append(Upload(local, f"/fonts/{family}/{local.name}"))
        return family, uploads

    monkeypatch.setattr(gfonts, "build_google_font", build)
    r = x4("font", "install")
    assert r.exit_code == 0, r.output
    assert built == list(gfonts.CURATED)
    assert r.output.splitlines()[0] == "Literata  60.0 KB"
    assert len(r.output.splitlines()) == 6
    assert (sim.root / "fonts/SourceSerif4/SourceSerif4_18.cpfont").exists()

    built.clear()
    (sim.root / "fonts/Bitter/Bitter_16.cpfont").unlink()
    r = x4("--json", "font", "install")
    data = json.loads(r.output)
    assert built == ["Bitter"]
    assert [f["skipped"] for f in data["fonts"]] == [True, True, True, True, False, True]
    assert data["fonts"][0] == {"family": "Literata", "remote": "/fonts/Literata/", "bytes": 61440, "skipped": True}
    assert x4("font", "install").output.splitlines()[1] == "CharisSIL  60.0 KB  skipped"

    built.clear()
    x4("font", "install", "--force")
    assert len(built) == 6
