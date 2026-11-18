import itertools
import struct
from pathlib import Path

from x4.stardict import write_stardict


def read_ifo(path: Path) -> dict[str, str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    assert lines[0] == "StarDict's dict ifo file"
    return dict(line.split("=", 1) for line in lines[1:])


def read_idx(path: Path) -> list[tuple[str, int, int]]:
    data = path.read_bytes()
    out, pos = [], 0
    while pos < len(data):
        end = data.index(b"\0", pos)
        offset, size = struct.unpack(">II", data[end + 1 : end + 9])
        out.append((data[pos:end].decode(), offset, size))
        pos = end + 9
    return out


def read_syn(path: Path) -> list[tuple[str, int]]:
    data = path.read_bytes()
    out, pos = [], 0
    while pos < len(data):
        end = data.index(b"\0", pos)
        (index,) = struct.unpack(">I", data[end + 1 : end + 5])
        out.append((data[pos:end].decode(), index))
        pos = end + 5
    return out


def g_ascii_strcasecmp_then_strcmp(a: str, b: str) -> int:
    """Independent re-implementation of StarDict's stardict_strcmp."""
    x, y = a.encode(), b.encode()
    for ca, cb in zip(x, y, strict=False):
        la = ca + 32 if 65 <= ca <= 90 else ca
        lb = cb + 32 if 65 <= cb <= 90 else cb
        if la != lb:
            return la - lb
    if len(x) != len(y):
        return len(x) - len(y)
    return (x > y) - (x < y)


def test_writer_is_readable_and_sorted(tmp_path: Path) -> None:
    entries = [
        ("zebra", "a striped animal"),
        ("Apple", "a fruit"),
        ("apple", "the same fruit, lowercase"),
        ("_under", "underscore sorts after Z, before a"),
        ("élet", "life"),
        ("ab", "short"),
        ("abc", "longer"),
        ("run", "to move fast"),
        ("run", "an act of running"),
    ]
    paths = write_stardict(entries, tmp_path, "test", "Test Dict", synonyms=[("ran", "run"), ("Élet", "élet")])
    assert [p.name for p in paths] == ["test.ifo", "test.idx", "test.dict", "test.syn"]

    ifo = read_ifo(tmp_path / "test.ifo")
    assert ifo["version"] == "2.4.2"
    assert ifo["wordcount"] == str(len(entries))
    assert ifo["idxfilesize"] == str((tmp_path / "test.idx").stat().st_size)
    assert ifo["synwordcount"] == "2"
    assert ifo["sametypesequence"] == "m"

    idx = read_idx(tmp_path / "test.idx")
    words = [w for w, _, _ in idx]
    for a, b in itertools.pairwise(words):
        assert g_ascii_strcasecmp_then_strcmp(a, b) <= 0, (a, b)
    assert words.index("Apple") < words.index("apple")  # equal ignoring case: strcmp decides
    assert words.index("_under") < words.index("ab")  # '_' (0x5F) < 'a' after ASCII lowering
    assert words[-1] == "élet"  # non-ASCII bytes sort after ASCII

    dict_bytes = (tmp_path / "test.dict").read_bytes()
    definitions = {}
    for word, offset, size in idx:
        definitions.setdefault(word, []).append(dict_bytes[offset : offset + size].decode())
    assert definitions["run"] == ["to move fast", "an act of running"]
    assert definitions["élet"] == ["life"]

    syn = dict(read_syn(tmp_path / "test.syn"))
    assert words[syn["ran"]] == "run"
    assert words[syn["Élet"]] == "élet"


def test_html_detection_and_no_syn(tmp_path: Path) -> None:
    paths = write_stardict([("word", "<b>bold</b> meaning")], tmp_path, "g", "Glossary")
    assert len(paths) == 3
    assert read_ifo(tmp_path / "g.ifo")["sametypesequence"] == "h"
    assert "synwordcount" not in read_ifo(tmp_path / "g.ifo")


def test_deterministic(tmp_path: Path) -> None:
    entries = [("b", "2"), ("a", "1"), ("C", "3")]
    write_stardict(entries, tmp_path / "1", "d", "D")
    write_stardict(entries, tmp_path / "2", "d", "D")
    for suffix in ("ifo", "idx", "dict"):
        assert (tmp_path / "1" / f"d.{suffix}").read_bytes() == (tmp_path / "2" / f"d.{suffix}").read_bytes()
