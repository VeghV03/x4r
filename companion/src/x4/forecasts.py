"""Forecasts: from asked to settled.

A piece's `forecast` is a statement with a day; the reader says how likely (10, 30, 50, 70 or
90). X4_HOME/forecasts.jsonl is the ledger, written only here: a line when a forecast is sent
and its settlement when Claude runs `x4 settle`. The row picked is not copied in: it is read
from the pulled seen log.
"""

from datetime import date, timedelta

from . import days
from .errors import InvalidData
from .home import Home, read_jsonl
from .models import Seen, jsonl_bytes

ROWS = (10, 30, 50, 70, 90)
HORIZON_DAYS = 42  # a forecast settles within six weeks of being made
# `x4 forecasts` puts numbers on certainty only from these counts (a judgement, not a standard).
SCORE_FROM = 20
ROW_FROM = 10
OUTCOMES = {"yes": True, "no": False, "void": "void"}
TELL_DAYS = 7  # a settled forecast is in the sync's report this long


def ledger(h: Home) -> list[dict]:
    return read_jsonl(h.forecasts) if h.forecasts.exists() else []


def _write(h: Home, rows: list[dict]) -> None:
    h.forecasts.write_bytes(jsonl_bytes(rows))


def picks(h: Home) -> dict[str, dict]:
    """Forecast id -> the row picked on the X4 and the day it was picked on."""
    out = {}
    for s in map(Seen.model_validate, read_jsonl(h.archive("seen"))):
        if s.p is not None:
            out[s.id] = {"p": s.p, "day": days.of(s)}
    return out


def note(h: Home, records: list[dict], today: date) -> None:
    """An issue was sent: its forecasts enter the ledger."""
    rows = ledger(h)
    known = {r["id"]: r for r in rows}
    changed = False
    for r in records:
        if r["kind"] == "forecast" and r["id"] not in known:
            rows.append({"id": r["id"], "text": r["text"], "by": r["by"], "made": today.isoformat()})
            changed = True
    if changed:
        _write(h, rows)


def settle(h: Home, forecast_id: str, outcome: str, source: str, today: date) -> dict:
    """Record what became of a forecast: yes, no, or void when it cannot be told."""
    rows = ledger(h)
    row = next((r for r in rows if r["id"] == forecast_id), None)
    if row is None:
        raise InvalidData(f"{forecast_id} is not a forecast", [f"{h.forecasts}: no forecast with id {forecast_id!r}; see x4 forecasts"])
    row.update({"happened": OUTCOMES[outcome], "resolved": today.isoformat(), "source": source})
    _write(h, rows)
    return row | picks(h).get(forecast_id, {})


def score(settled: list[dict]) -> dict:
    """How certainty matched what happened, once there is enough to say: `brier` (the mean
    squared distance between the row and the outcome, 0 best, 0.25 for always saying 50) from
    SCORE_FROM settled, and per row how often it happened from ROW_FROM."""
    out: dict = {"settled": len(settled)}
    if len(settled) >= SCORE_FROM:
        out["brier"] = round(sum((r["p"] / 100 - r["happened"]) ** 2 for r in settled) / len(settled), 3)
    rows = {}
    for p in ROWS:
        of = [r for r in settled if r["p"] == p]
        if len(of) >= ROW_FROM:
            rows[str(p)] = {"n": len(of), "happened": round(sum(r["happened"] for r in of) / len(of), 2)}
    if rows:
        out["rows"] = rows
    return out


def listing(h: Home, today: date) -> dict:
    """`forecasts` for `x4 context` and `x4 forecasts`: `open` (picked, their day not yet
    come), `due` (past their day, to settle), `tell` (settled yes or no in the last week, for
    the sync's report until the X4 shows outcomes, X59), `void`, and the score."""
    picked = picks(h)
    rows = [r | picked.get(r["id"], {}) for r in ledger(h)]

    def brief(r: dict) -> dict:
        return {k: r[k] for k in ("id", "text", "by", "p", "happened", "source") if r.get(k) is not None}

    answered = [r for r in rows if "p" in r]
    unsettled = [r for r in answered if "happened" not in r]
    told = [r for r in answered if isinstance(r.get("happened"), bool)]
    return {
        "open": [brief(r) for r in unsettled if r["by"] >= today.isoformat()],
        "due": [brief(r) for r in unsettled if r["by"] < today.isoformat()],
        "tell": [brief(r) for r in told if r.get("resolved", "") >= (today - timedelta(days=TELL_DAYS)).isoformat()],
        "void": len([r for r in rows if r.get("happened") == "void"]),
        "waiting": len([r for r in rows if "p" not in r and "happened" not in r]),
        **score(told),
    }


def check(h: Home, name: str, rows: list[dict], today: date) -> list[str]:
    """The issue's forecasts settle within the horizon and not in the past."""
    known = {r["id"]: r for r in ledger(h)}
    errors = []
    for r in rows:
        where = f"{name}: {r.get('id')}"
        if r.get("kind") == "forecast" and isinstance(r.get("by"), str):
            try:
                by = date.fromisoformat(r["by"])
            except ValueError:
                continue  # the model reports it
            made = date.fromisoformat(known[r["id"]]["made"]) if r.get("id") in known else today
            if by < today:
                errors.append(f"{where}: by: {r['by']} has passed; drop the forecast, it was never answered")
            elif by > made + timedelta(days=HORIZON_DAYS):
                errors.append(f"{where}: by: {r['by']} is over {HORIZON_DAYS} days ahead; ask about something that settles sooner")
    return errors
