import json
from datetime import UTC, date, datetime
from pathlib import Path

import httpx
import pytest

from x4 import football
from x4.errors import InvalidData, X4Error

FIXTURES = Path(__file__).parent / "fixtures" / "football"
TODAY = date(2026, 11, 9)
NOW = datetime(2026, 11, 9, 13, 0, tzinfo=UTC).timestamp()


def at(utc: str) -> int:
    return int(datetime.fromisoformat(utc).timestamp())


def write_cfg(h, key="abc123", clubs=(397, 402), competitions=("PL", "CL", "SA")):
    h.football_cfg.write_text(f"clubs: {list(clubs)}\ncompetitions: [{', '.join(competitions)}]\n")
    if key:
        h.football_key.write_text(key + "\n")


class Api:
    """football-data.org from recorded answers; SA is outside the free tier."""

    def __init__(self, limited: int = 0):
        self.calls: list[httpx.Request] = []
        self.limited = limited  # answer 429 this many times first

    def __call__(self, request: httpx.Request) -> httpx.Response:
        self.calls.append(request)
        if self.limited:
            self.limited -= 1
            return httpx.Response(429, json={"message": "You reached your request limit."}, headers={"X-RequestCounter-Reset": "7"})
        path = request.url.path.removeprefix("/v4")
        names = {"/teams/397/matches": "team-397", "/teams/402/matches": "team-402", "/competitions/PL/standings": "standings-PL", "/competitions/CL/standings": "standings-CL"}
        if path in names:
            return httpx.Response(200, content=(FIXTURES / f"{names[path]}.json").read_bytes(), headers={"X-Requests-Available-Minute": "5"})
        if path.startswith("/teams/"):
            return httpx.Response(404, json={"message": "The resource you are looking for does not exist.", "errorCode": 404})
        return httpx.Response(403, content=(FIXTURES / "restricted.json").read_bytes())

    def client(self) -> httpx.Client:
        return httpx.Client(transport=httpx.MockTransport(self))


def run(h, api: Api, slept: list | None = None) -> dict:
    sleep = (slept.append if slept is not None else lambda s: None)
    return football.fetch(h, api.client(), sleep, TODAY, NOW)


def test_fetch_writes_football_json(h):
    write_cfg(h)
    api = Api()
    out = run(h, api)
    assert out == {
        "file": str(h.football),
        "matches": 5,
        "tables": 2,
        "warnings": [
            (
                "competition SA: 403 The resource you are looking for is restricted and apparently not within your "
                "permissions. Please check your subscription; left out"
            )
        ],
    }
    assert api.calls[0].headers["X-Auth-Token"] == "abc123"
    assert dict(api.calls[0].url.params) == {"dateFrom": "2026-11-02", "dateTo": "2026-11-23"}
    data = json.loads(h.football.read_text())
    assert data["fetched"] == int(NOW)
    assert data["matches"] == [
        {"at": at("2026-11-03T20:00:00Z"), "comp": "PL", "home": "Arsenal", "away": "Brighton Hove", "score": [1, 1]},
        {"at": at("2026-11-08T15:00:00Z"), "comp": "PL", "home": "Brighton Hove", "away": "Brentford", "score": [2, 1]},
        {"at": at("2026-11-09T12:30:00Z"), "comp": "PL", "home": "Brighton Hove", "away": "Liverpool", "score": [0, 1], "live": True},
        {"at": at("2026-11-21T17:30:00Z"), "comp": "PL", "home": "Liverpool", "away": "Brentford"},
        {"at": at("2026-11-22T15:00:00Z"), "comp": "PL", "home": "Everton", "away": "Brighton Hove"},
    ]  # the derby once, the postponed match left out
    pl, cl = data["tables"]
    assert pl == {
        "comp": "PL",
        "name": "Premier League",
        "rows": [
            {"pos": 1, "team": "Arsenal", "played": 11, "gd": 15, "points": 26},
            {"pos": 6, "team": "Brighton Hove", "played": 11, "gd": 4, "points": 18, "mine": True},
            {"pos": 12, "team": "Brentford", "played": 11, "gd": -2, "points": 14, "mine": True},
            {"pos": 14, "team": "Everton", "played": 11, "gd": -4, "points": 12},
        ],
    }  # the TOTAL table, not HOME or AWAY
    assert cl["comp"] == "CL" and cl["name"] == "UEFA Champions League" and len(cl["rows"]) == 36
    assert all(len(r["team"].encode()) <= 24 for r in cl["rows"])
    assert cl["rows"][-1] == {"pos": 36, "team": "Barça", "played": 4, "gd": -3, "points": 3}


def test_unknown_club_is_a_warning(h):
    write_cfg(h, clubs=(397, 99999), competitions=("PL",))
    out = run(h, Api())
    assert out["matches"] == 4 and out["tables"] == 1
    assert out["warnings"] == ["club 99999: 404 The resource you are looking for does not exist; left out"]


def test_rate_limit_waits_and_retries(h):
    write_cfg(h, clubs=(397,), competitions=())
    slept: list[float] = []
    out = run(h, Api(limited=2), slept)
    assert slept == [7, 7] and out["matches"] == 4


def test_nothing_answered_is_an_error_and_keeps_the_file(h):
    write_cfg(h, clubs=(99999,), competitions=("SA",))
    h.football.write_text('{"old":true}')
    with pytest.raises(X4Error) as e:
        run(h, Api())
    assert "answered no call" in e.value.message
    assert h.football.read_text() == '{"old":true}'


def test_no_key(h):
    with pytest.raises(InvalidData):
        football.fetch(h)
    write_cfg(h, key=None)
    assert not football.has_key(h)
    assert football.before_issue(h) == []


def test_the_key_stays_out_of_football_yaml(h):
    h.football_cfg.write_text("key: abc\nclubs: [397]\ncompetitions: [PL]\n")
    with pytest.raises(InvalidData) as e:
        football.load(h)
    assert "football.key" in e.value.details[0]


def test_in_the_cloud_the_proxy_adds_the_key(h, monkeypatch):
    write_cfg(h, key="from-the-file")
    monkeypatch.setenv("X4_FOOTBALL_KEY", "proxy")
    api = Api()
    run(h, api)
    assert "X-Auth-Token" not in api.calls[0].headers


def test_invalid_config(h):
    h.football_cfg.write_text("clubs: [Brighton]\ncompetitions: PL\n")
    with pytest.raises(InvalidData) as e:
        football.load(h)
    assert len(e.value.details) == 2


def test_before_issue_turns_failures_into_warnings(h, monkeypatch):
    write_cfg(h)

    def down(*a, **kw):
        raise httpx.ConnectError("no network")

    monkeypatch.setattr(football, "fetch", down)
    assert football.before_issue(h) == ["football: no network; football.json not updated"]


def test_cap_keeps_the_matches_nearest_now(h):
    write_cfg(h, clubs=(397,), competitions=())
    many = [
        {"id": i, "utcDate": f"2026-11-{2 + i // 3:02d}T{8 + i % 3 * 4:02d}:00:00Z", "status": "TIMED",
         "competition": {"code": "PD"}, "homeTeam": {"shortName": "A"}, "awayTeam": {"name": "B"}}
        for i in range(60)
    ]  # fmt: skip
    client = httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(200, json={"matches": many})))
    football.fetch(h, client, lambda s: None, TODAY, NOW)
    kept = json.loads(h.football.read_text())["matches"]
    assert len(kept) == 40 and kept == sorted(kept, key=lambda m: m["at"])
    assert kept[0]["at"] > at("2026-11-02T08:00:00Z") and kept[0]["comp"] == "PD" and kept[0]["away"] == "B"


def test_trim_cuts_on_a_character():
    assert football.trim("Wolverhampton Wanderers FC", 24) == "Wolverhampton Wanderers"
    assert football.trim("é" * 25, 24) == "é" * 12
    assert football.trim("Brentford", 24) == "Brentford"


def test_example_config_is_valid(h):
    example = Path(__file__).parents[1] / "examples" / "home" / "football.yaml"
    h.football_cfg.write_text(example.read_text())
    cfg = football.load(h)
    assert cfg == {"key": None, "clubs": [], "competitions": ["PL"], "names": {}}


def test_names_override_and_fc_trimmed():
    from x4.football import _team

    assert _team({"id": 397, "shortName": "Brighton Hove"}, {397: "Brighton"}) == "Brighton"
    assert _team({"id": 559, "shortName": "Sevilla FC"}, {}) == "Sevilla"
    assert _team({"id": 81, "shortName": "Barça"}, {}) == "Barça"


def test_names_must_map_ids_to_text(h):
    h.football_cfg.write_text("clubs: [397]\ncompetitions: [PL]\nnames: {Brighton: 397}\n")
    with pytest.raises(InvalidData):
        football.load(h)
