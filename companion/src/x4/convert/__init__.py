"""Converters that turn books, PDFs, articles, fonts and dictionaries into files the X4 reads."""

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Upload:
    """A local file and the absolute SD card path it goes to."""

    local: Path
    remote: str
    note: str = ""  # how it was converted, for the person or agent who asked
