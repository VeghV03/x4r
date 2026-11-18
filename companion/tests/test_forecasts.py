import json
from datetime import date, timedelta

from records import write_jsonl

from x4 import forecasts
from x4.context import context
from x4.home import read_jsonl

TODAY = date.today()


def day(n: int) -> str:
    return (TODAY + timedelta(days=n)).isoformat()


def forecast(i: str, by: str) -> dict:
    return {"id": i, "kind": "forecast", "text": f"Statement {i}.", "by": by}


def test_a_forecast_settles_within_six_weeks_and_not_in_the_past(h):
    assert forecasts.check(h, "read.jsonl", [forecast("f1", day(42))], TODAY) == []
    for by, said in ((day(43), "over 42 days ahead"), (day(-1), "has passed")):
        [error] = forecasts.check(h, "read.jsonl", [forecast("f1", by)], TODAY)
        assert said in error


def test_a_forecast_goes_from_asked_to_settled_to_told(x4, sim, h):
    forecasts.note(h, [forecast("f1", day(10)), forecast("f2", day(10)), forecast("f3", day(10))], TODAY)
    assert [r["id"] for r in read_jsonl(h.forecasts)] == ["f1", "f2", "f3"]
    assert forecasts.listing(h, TODAY)["waiting"] == 3

    # Answered on the X4: 70 and 30; the third is not seen yet.
    write_jsonl(sim.root / "x4/seen.jsonl", [{"id": "f1", "p": 70, "ms": 5000, "day": 1}, {"id": "f2", "p": 30, "ms": 4000, "day": 1}])
    x4("pull")
    later = TODAY + timedelta(days=11)
    listed = forecasts.listing(h, later)
    assert [(r["id"], r["p"]) for r in listed["due"]] == [("f1", 70), ("f2", 30)]
    assert forecasts.listing(h, TODAY)["open"][0]["id"] == "f1" and listed["waiting"] == 1

    assert json.loads(x4("--json", "settle", "f1", "yes", "https://example.org/a").output)["p"] == 70
    x4("settle", "f2", "void", "https://example.org/b")
    assert x4("settle", "f9", "yes", "https://example.org/c").exit_code == 2
    listed = forecasts.listing(h, later)
    assert listed["due"] == [] and listed["void"] == 1
    assert [r["id"] for r in forecasts.listing(h, TODAY)["tell"]] == ["f1"]
    # The sync's report tells a settled one for a week.
    assert forecasts.listing(h, TODAY + timedelta(days=forecasts.TELL_DAYS + 1))["tell"] == []
    assert context(h)["forecasts"]["settled"] == 1


def test_numbers_wait_for_enough_forecasts(h):
    def settled(n: int, p: int, happened: int) -> list[dict]:
        return [{"p": p, "happened": i < happened} for i in range(n)]

    assert forecasts.score(settled(19, 70, 10)) == {"settled": 19, "rows": {"70": {"n": 19, "happened": 0.53}}}
    few = forecasts.score(settled(9, 70, 9) + settled(9, 30, 0))
    assert few == {"settled": 18}
    out = forecasts.score(settled(10, 90, 9) + settled(10, 10, 1))
    assert out == {"settled": 20, "brier": 0.09, "rows": {"10": {"n": 10, "happened": 0.1}, "90": {"n": 10, "happened": 0.9}}}
