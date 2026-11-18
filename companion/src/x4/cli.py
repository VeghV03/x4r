"""The `x4` command. Text by default, one JSON object with --json. Exit 0 ok, 1 device/IO, 2 invalid data."""

import json
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

import click

from . import ops
from .device import connect
from .errors import X4Error
from .home import home
from .sim import BUTTONS


@dataclass
class Opts:
    json: bool = False
    port: str | None = None


def _set_json(ctx: click.Context, _param, value: bool) -> None:
    if value:
        ctx.find_root().ensure_object(Opts).json = True


json_flag = click.option(
    "--json", is_flag=True, expose_value=False, callback=_set_json, help="Print one JSON object."
)


class Group(click.Group):
    """Turns X4Error and OSError into a message and an exit code."""

    def invoke(self, ctx: click.Context):
        try:
            return super().invoke(ctx)
        except X4Error as e:
            _fail(ctx, e.to_json(), str(e), e.exit_code)
        except OSError as e:
            _fail(ctx, {"code": "io", "message": str(e)}, str(e), 1)
        except ValueError as e:  # a file in X4_HOME that does not parse or validate
            _fail(ctx, {"code": "invalid", "message": str(e)}, str(e), 1)

    def command(self, *args, **kwargs):
        wrap = super().command(*args, **kwargs)
        return lambda f: wrap(json_flag(f))

    def group(self, *args, **kwargs):
        kwargs.setdefault("cls", Group)
        return super().group(*args, **kwargs)


def _fail(ctx: click.Context, err: dict, text: str, code: int):
    if ctx.find_root().ensure_object(Opts).json:
        click.echo(json.dumps({"error": err}, ensure_ascii=False))
    else:
        click.echo(f"error: {text}", err=True)
    sys.exit(code)


def emit(data: dict, text: Callable[[dict], str]) -> None:
    if click.get_current_context().find_root().ensure_object(Opts).json:
        click.echo(json.dumps(data, ensure_ascii=False))
    else:
        click.echo(text(data))


def device():
    return connect(click.get_current_context().find_root().ensure_object(Opts).port)


def _volts(d: dict) -> str:
    return f" {d['battery_mv'] / 1000:.2f} V" if d.get("battery_mv") else ""


def _day(d: dict) -> str:
    from . import days

    day = d.get("day")
    # A day the X4 counted before it ever saw a clock is a small number, no date.
    when = days.to_date(day).isoformat() if isinstance(day, int) and day > days.UNDATED else f"day {day}"
    issue = d.get("issue")
    return when + ("" if issue is None or issue == when else f"  issue {issue}")


def _minutes(d: dict) -> str:
    m = d.get("minutes")
    if not m:
        return ""
    return "\nlast 7 days: " + "  ".join(f"{k} {v} min" for k, v in m.items())


def _heap(d: dict) -> str:
    if "heap_free" not in d:
        return ""
    kb = lambda n: f"{n / 1024:.0f} KB"  # noqa: E731
    lows = "".join(f"\n  low {kb(x['min'])} after {x['at']}" for x in d.get("heap_low", []))
    return f"\nheap {kb(d['heap_free'])} free, {kb(d['heap_min'])} lowest, {kb(d['heap_block'])} largest block{lows}"


def _size(n: int | None) -> str:
    if n is None:
        return "?"
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return str(n)


@click.group(cls=Group)
@click.option("--json", is_flag=True, help="Print one JSON object.")
@click.option("--port", envvar="X4_PORT", help="Serial port, or sim:<dir>. Default: autodetect.")
@click.pass_context
def main(ctx: click.Context, json: bool, port: str | None) -> None:
    """Talk to an X4 running x4r firmware."""
    opts = ctx.ensure_object(Opts)
    opts.json = opts.json or json
    opts.port = port


@main.command()
def status() -> None:
    """Battery, free space, the day and its issue, retention, the last 7 days' minutes per section, records not yet pulled."""
    with device() as dev:
        data = ops.status(dev, home())
    emit(
        data,
        lambda d: (
            f"{d['fw']}  battery {d['battery']}%{_volts(d)}"
            f"{' charging' if d['charging'] else ''}  "
            f"SD {_size(d['sd_free'])} free of {_size(d['sd_total'])}\n"
            f"{_day(d)}{_retention(d)}{_minutes(d)}\n"
            f"not pulled: {d['new_highlights']} highlights, {d['new_reviews']} reviews"
            f"{_heap(d)}"
            f"{_net_status(d)}"
        ),
    )


def _net_status(d: dict) -> str:
    net = d.get("net")
    if not net:
        return ""
    from datetime import datetime

    text = "\nwifi " + (", ".join(net.get("networks", [])) or "none")
    if net.get("fetched"):
        text += "  fetched " + datetime.fromtimestamp(net["fetched"]).strftime("%Y-%m-%d %H:%M")
    if net.get("error"):
        text += f"  error: {net['error']}"
    return text


def _retention(d: dict) -> str:
    r = d.get("retention")
    return "" if r is None else f"  retention {round(r * 100)}%"


def _pull_text(d: dict) -> str:
    lines = []
    for name in ops.LOG_NAMES:
        if name in d:
            r = d[name]
            extra = f", {r['unreadable']} unreadable (kept in pulled/{name}.bad)" if r["unreadable"] else ""
            lines.append(f"{name}: {r['new']} new{extra}")
    if "stats" in d:
        lines.append("stats: saved" if d["stats"] else "stats: none on device")
    lines += [f"notes: {p}" for p in d.get("notes", [])]
    return "\n".join(lines)


@main.command()
@click.argument("what", type=click.Choice([*ops.PULLABLE, "all"]), default="all")
def pull(what: str) -> None:
    """Archive device records into X4_HOME/pulled, delete them on the device, update notes."""
    with device() as dev:
        data = ops.pull(dev, home(), what)
    emit(data, _pull_text)


@main.group()
def push() -> None:
    """Validate, then upload a deck, glossary or book."""


@push.command("deck")
@click.argument("file", type=click.Path(exists=True, dir_okay=False, path_type=Path))
def push_deck(file: Path) -> None:
    """Upload a deck JSONL to /x4/decks/<deck-id>.jsonl."""
    with device() as dev:
        data = ops.push_deck(dev, home(), file)
    emit(data, lambda d: f"{d['deck']}: {d['cards']} cards -> {d['remote']}")


@push.command("glossary")
@click.argument("book_key")
@click.argument("file", type=click.Path(exists=True, dir_okay=False, path_type=Path))
def push_glossary(book_key: str, file: Path) -> None:
    """Build a StarDict glossary from a term->definition JSON and upload it for BOOK_KEY."""
    with device() as dev:
        data = ops.push_glossary(dev, home(), book_key, file)
    emit(data, lambda d: f"{d['book']}: {d['terms']} terms -> {d['remote']}")


def _uploaded(d: dict) -> str:
    lines = [f"{u['remote']}  {_size(u['bytes'])}" + (f"  {u['note']}" if u.get("note") else "") for u in d["uploaded"]]
    return "\n".join(lines) or "nothing to upload"


@push.command("book")
@click.argument("src")
@click.option("--to", default="/books/", show_default=True, help="SD folder for the book.")
def push_book(src: str, to: str) -> None:
    """Convert an .epub, .pdf or URL and upload it.

    A PDF is reflowed as text or kept as pages, whichever suits it; the output says which and
    why. Add #reflow or #pages to the path to choose.
    """
    with device() as dev:
        data = ops.push_book(dev, src, to, home())
    emit(data, _uploaded)


def _sync_text(d: dict) -> str:
    lines = [_pull_text(d["pulled"])]
    lines += [f"pushed {r}" for r in d["pushed"]]
    lines += [f"deleted {r}" for r in d["deleted"]]
    lines.append(f"unchanged: {d['unchanged']}")
    lines += [f"kept for the writer: {b}" for b in d.get("books", [])]
    lines += [f"inbox: removed {u}" for u in d["inbox_removed"]]
    lines += [f"warning: {w}" for w in d["warnings"]]
    return "\n".join(lines)


@main.command()
def sync() -> None:
    """Pull everything, then push what changed in X4_HOME since the last push.

    The Read list (read.jsonl) is fetched, converted and uploaded to /books/read/."""
    with device() as dev:
        data = ops.sync(dev, home())
    emit(data, _sync_text)


def _football_text(d: dict) -> str:
    lines = [f"{d['file']}: {d['matches']} matches, {d['tables']} tables"]
    return "\n".join(lines + [f"warning: {w}" for w in d["warnings"]])


@main.command()
def football() -> None:
    """Fetch the followed clubs' matches and tables from football-data.org into football.json.

    The key, the clubs (team ids) and the competitions (codes) are in X4_HOME/football.yaml."""
    from .football import fetch

    emit(fetch(home()), _football_text)


def _stage_text(d: dict) -> str:
    lines = [f"staged {d['issue']} in {d['outbox']}"]
    lines += [f"  {f['path']}  {_size(f['size'])}" for f in d["files"]]
    lines += [f"kept for the writer: {b}" for b in d.get("books", [])]
    lines += [f"inbox: removed {u}" for u in d["inbox_removed"]]
    lines += [f"warning: {w}" for w in d["warnings"]]
    return "\n".join(lines)


@main.command()
def stage() -> None:
    """The issue into X4_HOME/outbox/, for the X4 to take over Wi-Fi. No device needed.

    The stream, the Read list and its articles, and football.json; x4 serve hands them out."""
    emit(ops.stage(home()), _stage_text)


@main.group(invoke_without_command=True)
@click.option("--port", type=click.IntRange(1, 65535), help="Saved in X4_HOME/serve.json. Default 8744.")
@click.option("--host", help="Listen on this address instead of the Mac's LAN IPv4.")
@click.pass_context
def serve(ctx: click.Context, port: int | None, host: str | None) -> None:
    """Serve the outbox to the X4 on the LAN, until stopped."""
    if ctx.invoked_subcommand is not None:
        return
    from . import lan

    def ready(d: dict) -> None:
        emit(d, lambda d: f"serving {d['outbox']} on {d['url']}")

    lan.serve(home(), port, host, ready)


@main.command()
@click.option("--port", type=click.IntRange(1, 65535), help="Default 8745.")
@click.option("--no-open", is_flag=True, help="Do not open the browser.")
def dashboard(port: int | None, no_open: bool) -> None:
    """A page on this Mac for what the paper covers: sections, interests, sources, fields, football, film."""
    from . import dashboard as page

    page.serve(home(), port, not no_open, lambda d: emit(d, lambda d: f"{d['url']}  ({d['home']})"))


@serve.command("install")
@click.option("--port", type=click.IntRange(1, 65535), help="Saved in X4_HOME/serve.json. Default 8744.")
def serve_install(port: int | None) -> None:
    """Run x4 serve at login (launchd, kept alive) and start it now."""
    from . import lan

    emit(lan.install(home(), port), lambda d: f"installed {d['plist']}; log {d['log']}")


@serve.command("uninstall")
def serve_uninstall() -> None:
    """Stop x4 serve and remove it from login."""
    from . import lan

    emit(lan.uninstall(), lambda d: f"removed {d['plist']}" if d["removed"] else "not installed")


@main.command()
def context() -> None:
    """JSON with new highlights, open questions, deleted highlights, review, read, play and puzzle results, books, decks, stream, the day's issue and the pieces of the last 14 days."""
    from .context import context as build

    click.echo(json.dumps(build(home()), ensure_ascii=False))


def _feed_text(d: dict) -> str:
    lines = [f"news: {len(d['news'])} stories" + (f" since {d['since'][:16].replace('T', ' ')}" if d["since"] else "")]
    for s in d["news"]:
        lines.append(f"  {s['k']}  {s['title']}" + (f"  [{', '.join(s['topics'])}]" if s.get("topics") else ""))
        lines += [f"       {p['kind']}: {p['title']}" for p in s.get("primary", [])]
    lines.append(f"follow: {len(d['follow'])} new")
    lines += [f"  {e['title']}  ({e['source']})" for e in d["follow"]]
    lines.append(f"inbox: {len(d['inbox'])} links")
    lines += [f"  {u}" for u in d["inbox"]]
    lines.append("interests: " + (f"{len(d['interests'].split())} words" if d["interests"] else "none"))
    lines += [f"warning: {w}" for w in d["warnings"]]
    return "\n".join(lines)


@main.command()
@click.option("--topic", help="Only this topic: hungary, us, eu, palestine, china, or a feed's own.")
def feed(topic: str | None) -> None:
    """Candidates for the Read list: news, new feed entries, inbox links, interests."""
    from .feed import feed as gather

    emit(gather(home(), topic), _feed_text)


def _net_text(d: dict) -> str:
    from datetime import datetime

    net = d.get("net", d)
    lines = ["wifi: " + (", ".join(net.get("networks", [])) or "none")]
    if net.get("fetching"):
        lines.append("fetching")
    if net.get("fetched"):
        lines.append("fetched " + datetime.fromtimestamp(net["fetched"]).strftime("%Y-%m-%d %H:%M"))
    if net.get("error"):
        lines.append(f"last error: {net['error']}")
    agenda = net.get("agenda") or {}
    for w in agenda.get("weather", []):
        lines.append(f"  {w['date']}  code {w['code']}  {w['high']}° / {w['low']}°")
    for e in agenda.get("events", []):
        when = datetime.fromtimestamp(e["start"])
        lines.append(f"  {when:%a %d}  {'all day' if e.get('all_day') else f'{when:%H:%M}'}  {e['title']}")
    return "\n".join(lines)


def _push_net_now(data: dict) -> dict:
    """Send net.json to the X4 when it is plugged in; otherwise it goes with the next sync."""
    from .errors import DeviceError

    try:
        dev = device()
    except DeviceError:
        return data | {"pushed": False}
    with dev:
        ops.push_net(dev, home())
    return data | {"pushed": True}


def _pushed(d: dict) -> str:
    return "sent to the X4" if d["pushed"] else "the X4 is not connected: it goes with the next x4 sync"


@main.group(invoke_without_command=True)
@click.pass_context
def wifi(ctx: click.Context) -> None:
    """Wi-Fi networks the X4 joins to fetch its calendars and the weather."""
    if ctx.invoked_subcommand is None:
        with device() as dev:
            emit(ops.net_command(dev, {}), _net_text)


@wifi.command("add")
@click.argument("ssid")
@click.option("--password", help="Asked for when left out; empty for an open network.")
def wifi_add(ssid: str, password: str | None) -> None:
    """Save a network on the X4 (at most 8; the one that answered last is tried first)."""
    if password is None:
        password = click.prompt("Password", hide_input=True, confirmation_prompt=True, default="", show_default=False)
    with device() as dev:
        emit(ops.net_command(dev, {"wifi": {"ssid": ssid, "password": password}}), _net_text)


@wifi.command("forget")
@click.argument("ssid")
def wifi_forget(ssid: str) -> None:
    """Remove a saved network from the X4."""
    with device() as dev:
        emit(ops.net_command(dev, {"forget": ssid}), _net_text)


def _calendars_text(d: dict) -> str:
    lines = [f"{i}. {c.get('name', '')}  {c['url'][:60]}{'...' if len(c['url']) > 60 else ''}" for i, c in enumerate(d["calendars"], 1)]
    if d.get("weather"):
        lines.append(f"weather: {d['weather']['name']}")
    if "pushed" in d:
        lines.append(_pushed(d))
    return "\n".join(lines) or "no calendars"


@main.group(invoke_without_command=True)
@click.pass_context
def calendar(ctx: click.Context) -> None:
    """Calendars the X4 fetches over Wi-Fi: their secret iCal addresses."""
    if ctx.invoked_subcommand is None:
        from . import net

        emit(net.load(home()), _calendars_text)


@calendar.command("add")
@click.argument("url", required=False)
@click.option("--name", help="A name to tell it apart in this list.")
def calendar_add(url: str | None, name: str | None) -> None:
    """Add a calendar by its iCal address (Google: Settings > the calendar > Secret address in iCal format).

    Asked for when left out, so the secret address need not sit in shell history."""
    from . import net

    if url is None:
        url = click.prompt("Secret iCal address")
    emit(_push_net_now(net.add_calendar(home(), url, name)), _calendars_text)


@calendar.command("rm")
@click.argument("which")
def calendar_rm(which: str) -> None:
    """Remove a calendar by its number, name or address."""
    from . import net

    emit(_push_net_now(net.remove_calendar(home(), which)), _calendars_text)


def _repo_text(d: dict) -> str:
    return (d["repo"] or "no data repo") + (f"\n{_pushed(d)}" if "pushed" in d else "")


@main.group(invoke_without_command=True)
@click.pass_context
def repo(ctx: click.Context) -> None:
    """The data repo on GitHub the X4 takes its issue from and sends its records to."""
    if ctx.invoked_subcommand is None:
        from . import net

        emit({"repo": (net.repo(home()) or {}).get("name")}, _repo_text)


@repo.command("set")
@click.argument("name")
def repo_set(name: str) -> None:
    """Keep the repo's NAME (<owner>/<repo>) and the X4's token for it.

    The token is asked for and never shown: a fine-grained one, for this repo alone, with
    Contents set to read and write."""
    from . import net

    token = click.prompt("Token", hide_input=True)
    emit(_push_net_now(net.set_repo(home(), name, token)), _repo_text)


@repo.command("rm")
def repo_rm() -> None:
    """Forget the repo and its token, here and on the X4."""
    from . import net

    emit(_push_net_now(net.forget_repo(home())), _repo_text)


@main.command()
@click.argument("place", nargs=-1, required=True)
def weather(place: tuple[str, ...]) -> None:
    """Set where the X4's forecast is for, by place name (Open-Meteo, no key)."""
    from . import net

    emit(_push_net_now(net.set_weather(home(), " ".join(place))), _calendars_text)


@main.command()
def fetch() -> None:
    """Have the X4 go online now: clock, calendars and weather; prints what it got."""
    with device() as dev:
        data = ops.fetch(dev, home())
    emit(data, _net_text)


@main.group()
def rss() -> None:
    """Feeds from X4_HOME/feeds.yaml."""


@rss.command("sync")
def rss_sync() -> None:
    """Fetch feeds, bundle new articles as EPUB, upload them."""
    with device() as dev:
        data = ops.rss_sync(dev, home())
    emit(data, _uploaded)


@main.group()
def font() -> None:
    """Font packs."""


def _sizes(_ctx, _param, value: str) -> list[int]:
    try:
        return [int(s) for s in value.split(",") if s.strip()]
    except ValueError:
        raise click.BadParameter("use comma-separated point sizes, e.g. 12,14,16,18") from None


sizes_option = click.option(
    "--sizes", default="12,14,16,18", show_default=True, callback=_sizes, help="Point sizes, as the built-in fonts."
)


def _font_line(f: dict) -> str:
    return f"{f['family']}  {_size(f['bytes'])}" + ("  skipped" if f.get("skipped") else "")


@font.command("install")
@click.option("--force", is_flag=True, help="Rebuild families already on the device.")
@sizes_option
def font_install(force: bool, sizes: list[int]) -> None:
    """Install Literata, Charis SIL, Source Serif 4, EB Garamond, Bitter and Atkinson Hyperlegible."""
    text = not click.get_current_context().find_root().ensure_object(Opts).json
    with device() as dev:
        data = ops.font_install(dev, force, sizes, (lambda f: click.echo(_font_line(f))) if text else None)
    if not text:
        emit(data, str)


@font.command("add")
@click.argument("source", nargs=-1, required=True)
@click.option("--name", help="Family name on the device. Default: the font's own.")
@sizes_option
def font_add(source: tuple[str, ...], name: str | None, sizes: list[int]) -> None:
    """Convert TTF/OTF files, or a Google Fonts family by name, and upload the packs."""
    with device() as dev:
        data = ops.font_add(dev, list(source), name, sizes)
    emit(data, _uploaded)


@main.group("dict")
def dict_() -> None:
    """Dictionaries."""


@dict_.command("install")
@click.argument("lang", type=click.Choice(["en", "hu"]))
def dict_install(lang: str) -> None:
    """Build and upload the English or Hungarian dictionary."""
    with device() as dev:
        data = ops.dict_install(dev, lang)
    emit(data, _uploaded)


@main.group()
def words() -> None:
    """Word lists and crosswords for the word games."""


def _built(f: dict) -> str:
    if "kept" in f:
        return f"{f['remote']}  kept: {f['kept']}"
    return f"{f['remote']}  {f['count']} {f['unit']}  {_size(f['bytes'])}"


@words.command("install")
@click.argument("lang", type=click.Choice(["en", "hu"]))
def words_install(lang: str) -> None:
    """Build and upload the English or Hungarian word lists and crosswords.

    Crosswords in X4_HOME/crosswords/<lang>.jsonl are kept; x4 sync pushes them."""
    with device() as dev:
        data = ops.words_install(dev, home(), lang)
    emit(data, lambda d: "\n".join(_built(f) for f in d["files"]))


def _grids_text(d: dict) -> str:
    out = []
    for g in d["grids"]:
        out += [g["id"], *(f"  {row}" for row in g["rows"])]
        for key in ("across", "down"):
            out.append(f"  {key}: " + "  ".join(f"{e['n']} {e['answer']}{'*' if e.get('seed') else ''}" for e in g[key]))
    if d["placed"] or d["unplaced"] or d["skipped"]:
        out.append(f"seeds placed (*): {len(d['placed'])}")
    if d["unplaced"]:
        out.append(f"seeds not placed: {', '.join(d['unplaced'])}")
    if d["skipped"]:
        out.append(f"seeds skipped: {', '.join(d['skipped'])}")
    return "\n".join([*out, f"saved {d['file']}", f"clue them into {d['append_to']}"])


@words.command("grids")
@click.argument("lang", type=click.Choice(["en", "hu"]))
@click.option("--count", default=6, show_default=True, type=click.IntRange(1, 100), help="How many grids.")
@click.option("--words", "word_list", default="", help="Seed words to place first, comma-separated, best first.")
@click.option(
    "--words-file",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
    help="Seed words, one per line: word, or word<TAB>source title. After --words.",
)
def words_grids(lang: str, count: int, word_list: str, words_file: Path | None) -> None:
    """Filled 7x7 grids without clues, numbered after X4_HOME/crosswords/<lang>.jsonl.

    Seed words go in first; the rest is rare words. Leaves out the answers of its last 30 puzzles.
    Needs the lexicon x4 words install built."""
    seeds: dict[str, str | None] = {w.strip(): None for w in word_list.split(",") if w.strip()}
    for word, src in (ops.seed_file(words_file) if words_file else {}).items():
        seeds.setdefault(word, src)
    emit(ops.words_grids(home(), lang, count, seeds), _grids_text)


@main.group()
def chess() -> None:
    """Puzzles for chess tactics."""


@chess.command("install")
def chess_install() -> None:
    """Build and upload the chess puzzles from the Lichess database."""
    with device() as dev:
        data = ops.chess_install(dev)
    emit(data, lambda d: "\n".join(_built(f) for f in d["files"]))


@main.command()
@click.argument("book_key")
@click.option("--words", default=2500, show_default=True, type=click.IntRange(50, 20000), help="How many words.")
@click.option("--progress", type=click.FloatRange(0, 1), help="Where to end, 0 to 1. Default: the last pull.")
def excerpt(book_key: str, words: int, progress: float | None) -> None:
    """The last words read in a book on the X4, ending where the reader stopped. For recaps.
    Reads the copy in X4_HOME/books/, so it needs the X4 only the first time."""
    from .previously import excerpt as build

    h = home()
    if h.book(book_key).exists():
        data = build(None, h, book_key, words, progress)
    else:
        with device() as dev:
            data = build(dev, h, book_key, words, progress)
    emit(data, lambda d: f"{d['title']}  {d['progress']:.1%}\n\n{d['text']}")


@main.command()
@click.argument("read_id")
def article(read_id: str) -> None:
    """The text of a Read list article as the X4 has it. A predict question quotes from it."""
    from .readlist import article as build

    emit(build(home(), read_id), lambda d: f"{d['title']}  {d['words']} words\n\n{d['text']}")


def _forecasts_text(d: dict) -> str:
    def lines(title: str, rows: list[dict]) -> list[str]:
        return [f"{title}:"] + [f"  {r['id']}  {r.get('p', '-')}%  by {r['by']}  {r['text']}" for r in rows] if rows else []

    out = lines("to settle", d["due"]) + lines("to tell", d["tell"]) + lines("open", d["open"])
    out.append(f"{d['settled']} settled, {d['void']} void, {d['waiting']} not yet answered")
    if "brier" in d:
        out.append(f"score {d['brier']} (0 best; 0.25 is always saying 50)")
    return "\n".join(out)


@main.command()
def forecasts() -> None:
    """Forecasts: those past their day to settle, those settled to tell, those still open."""
    from datetime import date

    from .forecasts import listing

    emit(listing(home(), date.today()), _forecasts_text)


@main.command()
@click.argument("forecast_id")
@click.argument("outcome", type=click.Choice(["yes", "no", "void"]))
@click.argument("source")
def settle(forecast_id: str, outcome: str, source: str) -> None:
    """Record what became of a forecast: it happened (yes), did not (no), or cannot be told
    (void). SOURCE is where that is settled, a URL."""
    from datetime import date

    from .forecasts import settle as build

    emit(build(home(), forecast_id, outcome, source, date.today()), lambda d: f"{d['id']}  {outcome}  {d.get('p', '-')}%")


@main.command()
def shelf() -> None:
    """Unread books to pick next: on the card, then in X4_HOME/library."""
    from .nextbook import shelf as build

    with device() as dev:
        data = {"books": build(dev, home())}
    emit(data, lambda d: "\n".join(f"{b['book']}  {b.get('title', '')}" for b in d["books"]))


@main.command("ls")
@click.argument("path", default="/")
def ls_(path: str) -> None:
    """List a folder on the SD card."""
    with device() as dev:
        data = ops.ls(dev, path)
    emit(
        data,
        lambda d: "\n".join(f"{e['name']}/" if e["dir"] else f"{e['name']}  {_size(e['size'])}" for e in d["entries"]),
    )


@main.command()
@click.argument("remote")
@click.argument("local", required=False, type=click.Path(path_type=Path))
def get(remote: str, local: Path | None) -> None:
    """Copy a file from the SD card."""
    with device() as dev:
        data = ops.get(dev, remote, local)
    emit(data, lambda d: f"{d['remote']} -> {d['local']}  {_size(d['bytes'])}")


@main.command()
@click.argument("local", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.argument("remote")
@click.option("--force", is_flag=True, help="Overwrite a file the X4 writes.")
def put(local: Path, remote: str, force: bool) -> None:
    """Copy a file to the SD card. A REMOTE ending in / keeps the file name."""
    with device() as dev:
        data = ops.put(dev, local, remote, force)
    emit(data, lambda d: f"{local} -> {d['remote']}  {_size(d['bytes'])}")


@main.command()
@click.argument("remote")
@click.option("-r", "--recursive", is_flag=True, help="Remove a folder and its contents.")
@click.option("--force", is_flag=True, help="Remove files the X4 writes too.")
def rm(remote: str, recursive: bool, force: bool) -> None:
    """Remove a file or folder on the SD card."""
    with device() as dev:
        data = ops.rm(dev, remote, recursive, force)
    emit(data, lambda d: f"removed {d['removed']}")


@main.command()
@click.argument("out", default="screenshot.png", type=click.Path(dir_okay=False, path_type=Path))
def screenshot(out: Path) -> None:
    """Save the screen as PNG."""
    with device() as dev:
        png, head = ops.screenshot(dev)
    out.write_bytes(png)
    emit({"path": str(out), "width": head["width"], "height": head["height"]}, lambda d: d["path"])


def _screen(d: dict) -> str:
    """`Questions  predict r-1 prompt`: the activity, then the step when one shows, and how far
    its steps are (`1/1`)."""
    step = d.get("step")
    if not step:
        return d["activity"]
    parts = [step.get("kind"), step.get("id"), step["phase"]]
    far = f"  {step['done']}/{step['total']}" if "total" in step else ""
    return f"{d['activity']}  {' '.join(p for p in parts if p)}{far}"


@main.command()
@click.argument("button", type=click.Choice(sorted(BUTTONS)))
@click.option("--long", is_flag=True, help="Long press.")
def press(button: str, long: bool) -> None:
    """Press a button. Prints the activity shown afterwards."""
    with device() as dev:
        data = ops.press(dev, button, long)
    emit(data, _screen)


@main.command()
def screen() -> None:
    """The activity on screen."""
    with device() as dev:
        data = ops.screen(dev)
    emit(data, _screen)


def _press_line(p: dict) -> str:
    up = f"up {p['up']:>4}" if p["up"] else "held   "
    clock = "  idle clock" if p["low_clock"] else ""
    return (
        f"{p['activity']:<12} after {p['idle'] / 1000:>5.1f} s  {up}  panel {p['panel']:>4}  "
        f"done {p['done']:>4}  last {p.get('last', p['done']):>4} ({p.get('updates', 1)})  {p['mode']}{clock}"
    )


@main.command()
def lag() -> None:
    """The last presses on the device, timed from button-down in ms."""
    with device() as dev:
        data = ops.lag(dev)
    def text(d: dict) -> str:
        lines = [_press_line(p) for p in d["presses"]]
        if d.get("marks"):
            lines.append("  ".join(f"{m['tag']} {m['ms']}" for m in d["marks"]))
        return "\n".join(lines) or "no presses yet"

    emit(data, text)


@main.command()
@click.option("--out", default="backups", show_default=True, type=click.Path(file_okay=False, path_type=Path))
def backup(out: Path) -> None:
    """Dump and verify all 16 MB of flash with esptool. Never overwrites."""
    from .flashing import backup as run

    data = run(click.get_current_context().find_root().obj.port, out)
    emit(data, lambda d: f"{d['path']}  sha256 {d['sha256']}")


@main.command()
@click.argument("firmware", type=click.Path(exists=True, dir_okay=False, path_type=Path))
@click.option("--backups", default="backups", show_default=True, type=click.Path(file_okay=False, path_type=Path))
def flash(firmware: Path, backups: Path) -> None:
    """Install an x4r image once a verified backup exists, and check that it booted."""
    from .flashing import flash as run

    data = run(click.get_current_context().find_root().obj.port, firmware, backups)
    emit(data, lambda d: f"{d['fw']} running, flashed via {d['method']}" + (f" to {d['slot']}" if "slot" in d else ""))


@main.command()
def mcp() -> None:
    """Run the MCP server on stdio."""
    from .mcp_server import server

    server.run()


@main.command()
@click.argument("root", type=click.Path(file_okay=False, path_type=Path))
@click.option("--lossy", is_flag=True, help="Lose bytes on the way out, as the X4's port does.")
@click.option("--play", is_flag=True, help="Play the paper as the firmware does: press, screen and screenshot work.")
def sim(root: Path, lossy: bool, play: bool) -> None:
    """Serve a simulated X4 whose SD card is ROOT. Prints the port to use."""
    from .sim import player_path, run_forever

    try:
        player = player_path() if play else None
    except FileNotFoundError as e:
        raise click.ClickException(str(e)) from None
    run_forever(root, lossy, player)
