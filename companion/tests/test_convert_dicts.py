import os
from pathlib import Path

import pytest
from test_convert_stardict import read_idx, read_ifo, read_syn

from x4 import dicts

FIXTURE = Path(__file__).parent / "fixtures" / "convert" / "kaikki.jsonl"


def test_english_from_fixture(tmp_path: Path) -> None:
    paths = dicts.build_from_file(FIXTURE, "en", tmp_path)
    assert [p.suffix for p in paths] == [".ifo", ".idx", ".dict", ".syn"]
    ifo = read_ifo(tmp_path / "english.ifo")
    assert ifo["sametypesequence"] == "h"
    assert "kaikki.org" in ifo["description"] and "CC BY-SA 4.0" in ifo["description"]

    idx = read_idx(tmp_path / "english.idx")
    words = [w for w, _, _ in idx]
    assert words == ["color", "run"]  # names and pure form-of entries are not headwords
    data = (tmp_path / "english.dict").read_bytes()
    run = next(data[o : o + s].decode() for w, o, s in idx if w == "run")
    assert "<i>verb</i>" in run and "<i>noun</i>" in run
    assert "(intransitive) To move swiftly on foot." in run

    syn = {alias: words[i] for alias, i in read_syn(tmp_path / "english.syn")}
    assert syn["ran"] == "run"
    assert syn["running"] == "run"
    assert syn["colour"] == "color"
    assert syn["colors"] == "color"
    assert "en-verb" not in syn


def test_hungarian_from_fixture(tmp_path: Path) -> None:
    dicts.build_from_file(FIXTURE, "hu", tmp_path)
    words = [w for w, _, _ in read_idx(tmp_path / "magyar.idx")]
    assert words == ["fa", "élet"]
    syn = {alias: words[i] for alias, i in read_syn(tmp_path / "magyar.syn")}
    assert syn["fák"] == "fa"
    assert syn["életek"] == "élet"
    assert syn["Élet"] == "élet"  # the device folds ASCII case only
    assert syn["Életek"] == "élet"
    assert not any(" " in alias for alias in syn)


def test_unknown_language(tmp_path: Path) -> None:
    with pytest.raises(ValueError):
        dicts.build_dictionary("xx", tmp_path)


@pytest.mark.skipif(not os.environ.get("X4_NETWORK_TESTS"), reason="set X4_NETWORK_TESTS=1 to download sources")
def test_build_hungarian_live(tmp_path: Path) -> None:
    uploads = dicts.build_dictionary("hu", tmp_path)
    assert {u.remote.rsplit("/", 1)[0] for u in uploads} == {"/dictionaries/hu"}
    assert int(read_ifo(tmp_path / "dictionaries" / "hu" / "magyar.ifo")["wordcount"]) > 10_000
