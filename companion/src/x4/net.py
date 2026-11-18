"""The X4's own trips online: Wi-Fi networks, calendars and the forecast.

The X4 fetches by itself (firmware src/net/): the Mac only tells it where. Calendars and the
weather place live in X4_HOME/net.json and go to /x4/net.json with the Mac's time zone, its
LAN address and token once `x4 serve` has run (lan.py), and the data repo with the X4's token
for it once `x4 repo set` has (X4_HOME/repo.json). Wi-Fi networks go straight to the device,
which keeps them obfuscated in /.crosspoint/wifi.json.
"""

import json
import re
from pathlib import Path
from urllib.parse import urlparse

import httpx

from .errors import InvalidData
from .home import Home

REMOTE = "/x4/net.json"
AGENDA = "/x4/agenda.json"
MAX_CALENDARS = 4
MAX_URL = 399  # the firmware keeps 400 bytes per calendar URL
GEOCODE = "https://geocoding-api.open-meteo.com/v1/search"
GITHUB = "https://api.github.com"
# "<owner>/<repo>", as the firmware's repoNameAllowed has it.
REPO_NAME = re.compile(r"[A-Za-z0-9_-][A-Za-z0-9._-]*/[A-Za-z0-9_-][A-Za-z0-9._-]*")
MAX_TOKEN = 255


def local_tz(localtime: Path = Path("/etc/localtime")) -> str:
    """The Mac's zone as a POSIX TZ rule, from the footer of its TZif file: "CET-1CEST,M3.5.0,M10.5.0/3"."""
    try:
        data = localtime.read_bytes()
    except OSError:
        return "UTC0"
    if data[:4] != b"TZif":
        return "UTC0"
    footer = data.rstrip(b"\n").rsplit(b"\n", 1)[-1].decode("ascii", "replace")
    return footer if footer and "\x00" not in footer else "UTC0"


def load(h: Home) -> dict:
    if not h.net.exists():
        return {"calendars": [], "weather": None}
    cfg = json.loads(h.net.read_text())
    cfg.setdefault("calendars", [])
    cfg.setdefault("weather", None)
    return cfg


def save(h: Home, cfg: dict) -> None:
    h.root.mkdir(parents=True, exist_ok=True)
    h.net.write_text(json.dumps(cfg, ensure_ascii=False, indent=2) + "\n")


def body(cfg: dict, tz: str | None = None, mac: dict | None = None, repo: dict | None = None) -> bytes:
    """/x4/net.json: the zone, the calendar URLs, the weather place and where the issue is (the
    Mac on the LAN, the data repo), as the firmware reads them."""
    out: dict = {"tz": tz or local_tz(), "calendars": [{"url": c["url"]} for c in cfg["calendars"]]}
    if cfg.get("weather"):
        w = cfg["weather"]
        out["weather"] = {"lat": round(w["lat"], 4), "lon": round(w["lon"], 4)}
    if mac:
        out["mac"] = mac
    if repo:
        out["repo"] = {"name": repo["name"], "token": repo["token"]}
    return json.dumps(out, ensure_ascii=False, separators=(",", ":")).encode()


def device_body(h: Home) -> bytes:
    """/x4/net.json from X4_HOME/net.json, with `mac` once `x4 serve` has a serve.json and
    `repo` once `x4 repo set` has a repo.json."""
    from .lan import mac

    return body(load(h), mac=mac(h), repo=repo(h))


def repo(h: Home) -> dict | None:
    """X4_HOME/repo.json: the data repo's name and the X4's token for it."""
    from .home import read_json

    cfg = read_json(h.repo, {})
    return cfg if cfg.get("name") and cfg.get("token") else None


def set_repo(h: Home, name: str, token: str, client: httpx.Client | None = None) -> dict:
    """Keep the data repo's name and token, once GitHub has answered for the repo with that
    token. Returns the name alone: the token is never shown."""
    from .home import write_json

    name, token = name.strip(), token.strip()
    if not REPO_NAME.fullmatch(name) or len(name) >= 80:
        raise InvalidData("not a repo's name", [f"{name}: give it as <owner>/<repo>, for example you/x4-data"])
    if not token or len(token) > MAX_TOKEN or not token.isascii() or any(c.isspace() for c in token):
        raise InvalidData("not a token", ["paste the fine-grained token GitHub showed, without spaces"])
    own = client is None
    client = client or httpx.Client(timeout=15)
    try:
        status = client.get(
            f"{GITHUB}/repos/{name}", headers={"Authorization": f"Bearer {token}", "User-Agent": "x4"}
        ).status_code
    finally:
        if own:
            client.close()
    if status != 200:
        why = {401: "GitHub does not know this token", 404: "this token cannot see the repo"}.get(status, f"GitHub answered {status}")
        raise InvalidData("kept nothing", [f"{name}: {why}"])
    h.root.mkdir(parents=True, exist_ok=True)
    write_json(h.repo, {"name": name, "token": token})
    h.repo.chmod(0o600)  # the token reads and writes the data repo
    return {"repo": name}


def forget_repo(h: Home) -> dict:
    h.repo.unlink(missing_ok=True)
    return {"repo": None}


def calendar_url(url: str) -> str:
    """A calendar's iCal address, checked: https (webcal is the same), short enough for the X4."""
    url = url.strip()
    if url.startswith("webcal://"):
        url = "https://" + url[len("webcal://") :]
    p = urlparse(url)
    if p.scheme != "https" or not p.netloc:
        raise InvalidData("not a calendar address", [f"{url}: use the https iCal address of the calendar"])
    if len(url.encode()) > MAX_URL:
        raise InvalidData("calendar address too long", [f"{url}: at most {MAX_URL} bytes"])
    if "calendar.google.com" in p.netloc and "/ical/" not in p.path:
        raise InvalidData(
            "not an iCal address",
            [f"{url}: in Google Calendar, open the calendar's settings and copy the secret address in iCal format"],
        )
    return url


def add_calendar(h: Home, url: str, name: str | None = None) -> dict:
    cfg = load(h)
    url = calendar_url(url)
    if any(c["url"] == url for c in cfg["calendars"]):
        return cfg
    if len(cfg["calendars"]) >= MAX_CALENDARS:
        raise InvalidData("too many calendars", [f"the X4 fetches at most {MAX_CALENDARS}; remove one first"])
    cfg["calendars"].append({"url": url, "name": name or f"Calendar {len(cfg['calendars']) + 1}"})
    save(h, cfg)
    return cfg


def remove_calendar(h: Home, which: str) -> dict:
    """By its number in `x4 calendar`, its name, or its address."""
    cfg = load(h)
    keep = []
    for i, c in enumerate(cfg["calendars"], 1):
        if which in (str(i), c.get("name"), c["url"]):
            continue
        keep.append(c)
    if len(keep) == len(cfg["calendars"]):
        raise InvalidData("no such calendar", [f"{which}: see `x4 calendar` for the list"])
    cfg["calendars"] = keep
    save(h, cfg)
    return cfg


def geocode(place: str, client: httpx.Client | None = None) -> dict:
    """The best match for a place name, from Open-Meteo's geocoder (no key)."""
    own = client is None
    client = client or httpx.Client(timeout=15)
    try:
        r = client.get(GEOCODE, params={"name": place, "count": 1, "language": "en", "format": "json"})
        r.raise_for_status()
        results = r.json().get("results") or []
    finally:
        if own:
            client.close()
    if not results:
        raise InvalidData("place not found", [f"{place}: try a city name"])
    best = results[0]
    label = ", ".join(x for x in (best.get("name"), best.get("country")) if x)
    return {"name": label, "lat": best["latitude"], "lon": best["longitude"]}


def set_weather(h: Home, place: str, client: httpx.Client | None = None) -> dict:
    cfg = load(h)
    cfg["weather"] = geocode(place, client)
    save(h, cfg)
    return cfg
