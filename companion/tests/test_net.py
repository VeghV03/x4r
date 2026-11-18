import json

import httpx
import pytest

from x4 import net, ops
from x4.errors import InvalidData

GOOGLE = "https://calendar.google.com/calendar/ical/me%40example.com/private-0123abcd/basic.ics"


def tzif(footer: bytes) -> bytes:
    return b"TZif2" + b"\x00" * 40 + b"\n" + footer + b"\n"


def test_local_tz_reads_the_tzif_footer(tmp_path):
    f = tmp_path / "localtime"
    f.write_bytes(tzif(b"CET-1CEST,M3.5.0,M10.5.0/3"))
    assert net.local_tz(f) == "CET-1CEST,M3.5.0,M10.5.0/3"
    f.write_bytes(b"not a zone")
    assert net.local_tz(f) == "UTC0"
    assert net.local_tz(tmp_path / "missing") == "UTC0"


def test_calendar_url_checks():
    assert net.calendar_url(" webcal://example.com/a.ics ") == "https://example.com/a.ics"
    assert net.calendar_url(GOOGLE) == GOOGLE
    for bad in ("http://example.com/a.ics", "calendar", "https://calendar.google.com/calendar/u/0/r"):
        with pytest.raises(InvalidData):
            net.calendar_url(bad)
    with pytest.raises(InvalidData):
        net.calendar_url("https://example.com/" + "a" * 400)


def test_add_remove_and_body(h):
    net.add_calendar(h, GOOGLE, "Me")
    net.add_calendar(h, GOOGLE)  # the same address once
    cfg = net.add_calendar(h, "https://example.com/team.ics")
    assert [c["name"] for c in cfg["calendars"]] == ["Me", "Calendar 2"]
    body = json.loads(net.body(cfg, tz="CET-1CEST,M3.5.0,M10.5.0/3"))
    assert body == {"tz": "CET-1CEST,M3.5.0,M10.5.0/3", "calendars": [{"url": GOOGLE}, {"url": "https://example.com/team.ics"}]}
    assert [c["name"] for c in net.remove_calendar(h, "1")["calendars"]] == ["Calendar 2"]
    with pytest.raises(InvalidData):
        net.remove_calendar(h, "9")


def test_at_most_four_calendars(h):
    for i in range(4):
        net.add_calendar(h, f"https://example.com/{i}.ics")
    with pytest.raises(InvalidData):
        net.add_calendar(h, "https://example.com/5.ics")


def test_weather_geocodes(h):
    def answer(request: httpx.Request) -> httpx.Response:
        assert request.url.params["name"] == "Budapest"
        return httpx.Response(200, json={"results": [{"name": "Budapest", "country": "Hungary", "latitude": 47.49812, "longitude": 19.04023}]})

    client = httpx.Client(transport=httpx.MockTransport(answer))
    cfg = net.set_weather(h, "Budapest", client)
    assert cfg["weather"]["name"] == "Budapest, Hungary"
    assert json.loads(net.body(cfg, tz="UTC0"))["weather"] == {"lat": 47.4981, "lon": 19.0402}

    empty = httpx.Client(transport=httpx.MockTransport(lambda r: httpx.Response(200, json={})))
    with pytest.raises(InvalidData):
        net.set_weather(h, "Nowhere", empty)


def test_wifi_commands(x4, sim):
    r = x4("wifi", "add", "Home", "--password", "secret")
    assert r.exit_code == 0 and "wifi: Home" in r.output
    assert sim.networks == {"Home": "secret"}
    assert json.loads(x4("--json", "wifi").output)["net"]["networks"] == ["Home"]
    assert "wifi Home" in x4("status").output
    x4("wifi", "forget", "Home")
    assert sim.networks == {}


def test_calendar_add_pushes_when_connected(x4, sim, h):
    r = x4("calendar", "add", GOOGLE, "--name", "Me")
    assert r.exit_code == 0 and "sent to the X4" in r.output
    on_device = json.loads((sim.root / "x4/net.json").read_text())
    assert on_device["calendars"] == [{"url": GOOGLE}]
    assert on_device["tz"]


def test_fetch(x4, sim, h):
    net.add_calendar(h, GOOGLE)
    r = x4("--json", "fetch")
    assert r.exit_code == 1  # no network saved yet
    x4("wifi", "add", "Home", "--password", "pw")
    data = json.loads(x4("--json", "fetch").output)
    assert data["fetched"] and data["agenda"]["events"][0]["title"] == "Sim event"
    assert "/x4/agenda.json" in ops.DEVICE_OWNED


def test_sync_sends_net_json_when_changed(dev, h):
    net.add_calendar(h, GOOGLE)
    assert net.REMOTE in ops.sync(dev, h)["pushed"]
    assert net.REMOTE not in ops.sync(dev, h)["pushed"]


def test_calendar_add_asks_for_the_address(x4, sim):
    r = x4("calendar", "add", input=GOOGLE + "\n")
    assert r.exit_code == 0, r.output
    assert json.loads((sim.root / "x4/net.json").read_text())["calendars"] == [{"url": GOOGLE}]
