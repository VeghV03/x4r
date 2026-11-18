"""StarDict 2.4.2 writer: .ifo, .idx, .dict and an optional .syn.

Entries are sorted the way StarDict (and the X4 firmware's binary search) expects:
g_ascii_strcasecmp first, then strcmp. Offsets are 32-bit big-endian.
"""

import re
import struct
from collections.abc import Iterable
from pathlib import Path

_ASCII_LOWER = bytes.maketrans(b"ABCDEFGHIJKLMNOPQRSTUVWXYZ", b"abcdefghijklmnopqrstuvwxyz")
_HTML_TAG = re.compile(r"</?[a-zA-Z][^>]*>")
MAX_WORD_BYTES = 255  # StarDict headwords are < 256 bytes; the device reads into a 256-byte buffer.


def sort_key(word: bytes) -> tuple[bytes, bytes]:
    """StarDict order: ASCII-only case folding, ties broken by plain byte order."""
    return word.translate(_ASCII_LOWER), word


def _clean_word(word: str) -> bytes | None:
    data = " ".join(word.split()).encode()
    if not data or b"\0" in data or len(data) > MAX_WORD_BYTES:
        return None
    return data


def _ifo_value(value: str) -> str:
    return " ".join(value.split())


def write_stardict(
    entries: Iterable[tuple[str, str]],
    out_dir: Path,
    name: str,
    bookname: str,
    *,
    synonyms: Iterable[tuple[str, str]] = (),
    description: str = "",
    html: bool | None = None,
) -> list[Path]:
    """Write `<name>.ifo/.idx/.dict` (and `.syn` when synonyms map to known headwords).

    `entries` are (headword, definition); a headword may repeat. `synonyms` are
    (alternate form, headword). `html=None` picks `sametypesequence=h` when any
    definition contains a tag, plain text (`m`) otherwise.
    """
    words: list[tuple[bytes, bytes]] = []
    for word, definition in entries:
        key = _clean_word(word)
        if key is None or not definition.strip():
            continue
        words.append((key, definition.strip().encode()))
    words.sort(key=lambda item: sort_key(item[0]))
    if html is None:
        html = any(_HTML_TAG.search(d.decode()) for _, d in words)

    out_dir.mkdir(parents=True, exist_ok=True)
    dict_path = out_dir / f"{name}.dict"
    idx_path = out_dir / f"{name}.idx"
    first_index: dict[bytes, int] = {}
    with dict_path.open("wb") as dict_file, idx_path.open("wb") as idx_file:
        offset = 0
        for index, (word, definition) in enumerate(words):
            first_index.setdefault(word, index)
            dict_file.write(definition)
            idx_file.write(word + b"\0" + struct.pack(">II", offset, len(definition)))
            offset += len(definition)
            if offset >= 1 << 32:
                raise ValueError("dictionary exceeds 4 GiB; 32-bit offsets only")

    syn_entries: set[tuple[bytes, int]] = set()
    for alias, target in synonyms:
        alias_key, target_key = _clean_word(alias), _clean_word(target)
        if alias_key is None or target_key is None or alias_key == target_key:
            continue
        if target_key in first_index:
            syn_entries.add((alias_key, first_index[target_key]))
    syn_sorted = sorted(syn_entries, key=lambda item: (sort_key(item[0]), item[1]))

    paths = [out_dir / f"{name}.ifo", idx_path, dict_path]
    syn_path = out_dir / f"{name}.syn"
    if syn_sorted:
        with syn_path.open("wb") as syn_file:
            for alias, index in syn_sorted:
                syn_file.write(alias + b"\0" + struct.pack(">I", index))
        paths.append(syn_path)
    elif syn_path.exists():
        syn_path.unlink()

    ifo = [
        "StarDict's dict ifo file",
        "version=2.4.2",
        f"bookname={_ifo_value(bookname)}",
        f"wordcount={len(words)}",
    ]
    if syn_sorted:
        ifo.append(f"synwordcount={len(syn_sorted)}")
    ifo.append(f"idxfilesize={idx_path.stat().st_size}")
    ifo.append(f"sametypesequence={'h' if html else 'm'}")
    if description:
        ifo.append(f"description={_ifo_value(description)}")
    paths[0].write_text("\n".join(ifo) + "\n", encoding="utf-8")
    return paths
