"""X4_HOME/settings.yaml: the few choices that are neither sections nor interests. Yours to edit,
or set in `x4 dashboard`.

language: the language Claude writes in (bites, questions, recaps, threads), an ISO 639-1 code.
news_engine: the folder of a news engine that ranks the day's stories (feed.py); none by default.
X4_NEWS_ENGINE overrides it.
"""

import os
from pathlib import Path

import yaml

from .errors import InvalidData
from .home import Home

DEFAULTS = {"language": "en"}
KEYS = ("language", "news_engine")


def load(h: Home) -> dict:
    path = h.settings
    if not path.exists():
        return dict(DEFAULTS)
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except yaml.YAMLError as e:
        raise InvalidData(f"{path}: not YAML", [str(e)]) from e
    if not isinstance(data, dict):
        raise InvalidData(f"{path}: invalid settings", ["write one 'key: value' per line"])
    unknown = sorted(set(data) - set(KEYS))
    if unknown:
        raise InvalidData(f"{path}: invalid settings", [f"unknown: {', '.join(unknown)}; the keys are {', '.join(KEYS)}"])
    lang = data.get("language", DEFAULTS["language"])
    if not (isinstance(lang, str) and len(lang) == 2 and lang.isalpha()):
        raise InvalidData(f"{path}: invalid settings", [f"language: '{lang}' is not a two-letter code (en, hu, de)"])
    return {**DEFAULTS, **{k: v for k, v in data.items() if v not in (None, "")}, "language": lang.lower()}


def save(h: Home, values: dict) -> None:
    """Write the settings, keeping only known keys with a value."""
    data = {k: values[k] for k in KEYS if values.get(k) not in (None, "")}
    text = "# x4 settings (companion/src/x4/settings.py). Yours to edit, or use `x4 dashboard`.\n"
    h.settings.write_text(text + yaml.safe_dump(data, allow_unicode=True, sort_keys=False), encoding="utf-8")
    load(h)  # refuse what was just written if it is invalid


def news_engine(h: Home) -> Path | None:
    """The news engine's folder, or None when none is set up."""
    where = os.environ.get("X4_NEWS_ENGINE") or load(h).get("news_engine")
    return Path(where).expanduser() if where else None
