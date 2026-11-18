"""The day a device record belongs to (docs/DATA.md "The day").

A record carries `day`: days since 1 January 1970 in the X4's zone. One from before the day
(v9 step 7) carries a set instead; it lands on the date of its `at`, or on no day (D25).
"""

from datetime import date, datetime

EPOCH = date(1970, 1, 1)
UNDATED = 3650  # a day below this was counted by reading, before the X4 saw any clock


def of_date(d: date) -> int:
    return (d - EPOCH).days


def to_date(day: int) -> date:
    return date.fromordinal(EPOCH.toordinal() + day)


def today() -> int:
    return of_date(date.today())


def of(record) -> int | None:
    """`day`, else the local date of `at`, else None. Takes a model or a dict."""
    get = record.get if isinstance(record, dict) else lambda k: getattr(record, k, None)
    day = get("day")
    if isinstance(day, int) and not isinstance(day, bool):
        return day
    at = get("at")
    if isinstance(at, (int, float)) and not isinstance(at, bool) and at > 0:
        return of_date(datetime.fromtimestamp(at).date())
    return None


def within(record, days: int, now: int | None = None) -> bool:
    """The record's day is one of the last `days` days, today included."""
    day = of(record)
    now = today() if now is None else now
    return day is not None and now - days < day <= now
