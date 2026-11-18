"""Football from football-data.org (v4): the followed clubs' matches and the tables.

X4_HOME/football.yaml holds the clubs (team ids) and the competitions (codes) and goes to the
data repo; the key stays off it, in X4_HOME/football.key or in X4_FOOTBALL_KEY. In the cloud
run X4_FOOTBALL_KEY is `proxy`: the environment's network secret adds the key to each request
(K16). The result is X4_HOME/football.json, the shape of /x4/football.json in docs/DATA.md. Fetch only:
the X4 never calls the API, and nothing here calls a language model.
"""

import json
import os
import time
from collections.abc import Callable
from datetime import date, datetime, timedelta

import httpx
import yaml

from .errors import InvalidData, X4Error
from .home import Home

API = "https://api.football-data.org/v4"
REMOTE = "/x4/football.json"
PAST, AHEAD = 7, 14  # days of results and fixtures
MAX_MATCHES, MAX_TABLES, MAX_ROWS = 40, 4, 36
TEAM_BYTES, COMP_BYTES, NAME_BYTES = 24, 12, 32
OVER = {"FINISHED", "AWARDED"}
LIVE = {"IN_PLAY", "PAUSED", "LIVE"}
GONE = {"POSTPONED", "CANCELLED", "SUSPENDED"}  # no kick-off to show
RETRIES = 3
PROXY = "proxy"  # the key is added on the way out, by the cloud environment


def trim(text: str, limit: int) -> str:
    """At most `limit` UTF-8 bytes, cut on a character boundary."""
    data = text.strip().encode()
    return data[:limit].decode("utf-8", "ignore").strip() if len(data) > limit else text.strip()


def load(h: Home) -> dict:
    """football.yaml, checked. Without it, or without a key, there is nothing to fetch."""
    if not h.football_cfg.exists():
        return {"key": None, "clubs": [], "competitions": [], "names": {}}
    cfg = yaml.safe_load(h.football_cfg.read_text(encoding="utf-8")) or {}
    if not isinstance(cfg, dict):
        raise InvalidData(f"{h.football_cfg}: invalid", [f"{h.football_cfg}: key, clubs and competitions"])
    clubs, comps = cfg.get("clubs") or [], cfg.get("competitions") or []
    errors = []
    if not isinstance(clubs, list) or not all(isinstance(c, int) and not isinstance(c, bool) for c in clubs):
        errors.append(f"{h.football_cfg}: clubs: a list of football-data.org team ids, e.g. [81, 397]")
    if not isinstance(comps, list) or not all(isinstance(c, str) and c.strip() for c in comps):
        errors.append(f"{h.football_cfg}: competitions: a list of codes, e.g. [PL, PD, SA, CL]")
    names = cfg.get("names") or {}
    if not isinstance(names, dict) or not all(isinstance(k, int) and isinstance(v, str) and v.strip() for k, v in names.items()):
        errors.append(f"{h.football_cfg}: names: team id to the name shown, e.g. {{397: Brighton}}")
    if errors:
        raise InvalidData(f"{h.football_cfg}: invalid", errors)
    if cfg.get("key"):
        raise InvalidData(
            f"{h.football_cfg}: the key does not go here",
            [f"{h.football_cfg}: move the key into {h.football_key}, one line; football.yaml goes to the data repo"],
        )
    return {
        "key": _key(h),
        "clubs": clubs,
        "competitions": [c.strip().upper() for c in comps],
        "names": {k: v.strip() for k, v in names.items()} if isinstance(names, dict) else {},
    }


def _key(h: Home) -> str | None:
    if key := os.environ.get("X4_FOOTBALL_KEY", "").strip():
        return key
    if h.football_key.exists():
        return h.football_key.read_text(encoding="utf-8").strip() or None
    return None


def has_key(h: Home) -> bool:
    try:
        return bool(load(h)["key"])
    except (InvalidData, yaml.YAMLError):
        return True  # let the fetch report it


class Api:
    """GETs within the free tier's 10 calls a minute: waits when the counter says so, and on 429."""

    def __init__(self, key: str, client: httpx.Client, sleep: Callable[[float], None] = time.sleep):
        self.key, self.client, self.sleep = key, client, sleep
        self.wait = 0.0  # seconds to wait before the next call

    def get(self, path: str, params: dict | None = None) -> dict:
        for _ in range(RETRIES):
            if self.wait:
                self.sleep(self.wait)
                self.wait = 0.0
            r = self.client.get(f"{API}{path}", params=params, headers={} if self.key == PROXY else {"X-Auth-Token": self.key})
            reset = _int(r.headers.get("X-RequestCounter-Reset"), 60)
            if r.status_code == 429:
                self.wait = max(reset, 1)
                continue
            if r.headers.get("X-Requests-Available-Minute") == "0":
                self.wait = max(reset, 1)
            if r.status_code != 200:
                raise X4Error(f"{r.status_code} {_message(r)}")
            return r.json()
        raise X4Error("429 too many requests")


def _int(value: str | None, default: int) -> int:
    try:
        return int(value) if value is not None else default
    except ValueError:
        return default


def _message(r: httpx.Response) -> str:
    try:
        return str(r.json().get("message") or r.reason_phrase).rstrip(".")
    except ValueError:
        return r.reason_phrase


def _team(t: dict, names: dict[int, str]) -> str:
    """The name football.yaml gives the team, else its short name without a trailing FC or CF."""
    name = names.get(t.get("id")) or t.get("shortName") or t.get("name") or t.get("tla") or "?"
    for suffix in (" FC", " CF"):
        if name.endswith(suffix) and len(name) > len(suffix):
            name = name[: -len(suffix)]
    return trim(name, TEAM_BYTES)


def _comp(c: dict) -> str:
    code = c.get("code") or ""
    return trim(code or c.get("name") or "?", COMP_BYTES)  # the code: six tabs fit only so (D48)


def _at(utc: str) -> int:
    return int(datetime.fromisoformat(utc).timestamp())


def match(m: dict, names: dict[int, str] | None = None) -> dict | None:
    """A match as football.json lists it, or None for one without a kick-off to show."""
    status = m.get("status", "")
    if status in GONE or not m.get("utcDate"):
        return None
    out = {"at": _at(m["utcDate"]), "comp": _comp(m.get("competition") or {}), "home": _team(m["homeTeam"], names or {}), "away": _team(m["awayTeam"], names or {})}
    full = (m.get("score") or {}).get("fullTime") or {}
    if (status in OVER or status in LIVE) and full.get("home") is not None and full.get("away") is not None:
        out["score"] = [full["home"], full["away"]]
        if status in LIVE:
            out["live"] = True
    return out


def table(data: dict, code: str, clubs: set[int], names: dict[int, str] | None = None) -> dict | None:
    """The TOTAL table of a standings answer; of several (groups), the first with a followed club."""
    totals = [s for s in data.get("standings", []) if s.get("type") == "TOTAL" and s.get("table")]
    if not totals:
        return None
    pick = next((s for s in totals if any(r["team"].get("id") in clubs for r in s["table"])), totals[0])
    rows = []
    for r in pick["table"][:MAX_ROWS]:
        row = {
            "pos": r["position"],
            "team": _team(r["team"], names or {}),
            "played": r.get("playedGames", 0),
            "gd": r.get("goalDifference", 0),
            "points": r.get("points", 0),
        }
        if r["team"].get("id") in clubs:
            row["mine"] = True
        rows.append(row)
    name = (data.get("competition") or {}).get("name") or code
    return {"comp": trim(code, COMP_BYTES), "name": trim(name, NAME_BYTES), "rows": rows}


def fetch(
    h: Home,
    client: httpx.Client | None = None,
    sleep: Callable[[float], None] = time.sleep,
    today: date | None = None,
    now: float | None = None,
) -> dict:
    """Fetch the matches and tables into X4_HOME/football.json. A club or competition the key
    cannot see is a warning; nothing fetched at all is an error and leaves the file as it was."""
    cfg = load(h)
    if not cfg["key"]:
        raise InvalidData(f"{h.football_cfg}: no key", [f"{h.football_cfg}: key: your football-data.org API key"])
    today = today or date.today()
    now = time.time() if now is None else now
    own = client is None
    client = client or httpx.Client(timeout=20)
    api = Api(cfg["key"], client, sleep)
    clubs = set(cfg["clubs"])
    warnings: list[str] = []
    calls = ok = 0
    found: dict = {}
    tables = []
    try:
        span = {"dateFrom": (today - timedelta(days=PAST)).isoformat(), "dateTo": (today + timedelta(days=AHEAD)).isoformat()}
        for club in cfg["clubs"]:
            calls += 1
            try:
                data = api.get(f"/teams/{club}/matches", span)
            except (X4Error, httpx.HTTPError, ValueError) as e:
                warnings.append(f"club {club}: {e}; left out")
                continue
            ok += 1
            for m in data.get("matches", []):
                if (x := match(m, cfg["names"])) is not None:
                    found.setdefault(m.get("id") or (x["at"], x["home"], x["away"]), x)
        for code in cfg["competitions"]:
            if len(tables) == MAX_TABLES:
                warnings.append(f"competition {code}: at most {MAX_TABLES} tables; left out")
                continue
            calls += 1
            try:
                data = api.get(f"/competitions/{code}/standings")
            except (X4Error, httpx.HTTPError, ValueError) as e:
                warnings.append(f"competition {code}: {e}; left out")
                continue
            ok += 1
            if (t := table(data, code, clubs, cfg["names"])) is None:
                warnings.append(f"competition {code}: no table; left out")
            else:
                tables.append(t)
    finally:
        if own:
            client.close()
    if calls and not ok:
        raise X4Error("football-data.org answered no call", warnings)
    matches = sorted(found.values(), key=lambda x: abs(x["at"] - now))[:MAX_MATCHES]  # the nearest to now
    matches.sort(key=lambda x: x["at"])
    body = {"fetched": int(now), "matches": matches, "tables": tables}
    tmp = h.football.with_name(h.football.name + ".tmp")
    tmp.write_text(json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    tmp.replace(h.football)
    return {"file": str(h.football), "matches": len(matches), "tables": len(tables), "warnings": warnings}


def before_issue(h: Home) -> list[str]:
    """The fetch `x4 sync` and `x4 stage` run first, when there is a key. Failures are warnings."""
    if not has_key(h):
        return []
    try:
        return [f"football: {w}" for w in fetch(h)["warnings"]]
    except (X4Error, httpx.HTTPError, OSError, ValueError, yaml.YAMLError) as e:
        return [f"football: {' '.join(str(e).split())}; football.json not updated"]
