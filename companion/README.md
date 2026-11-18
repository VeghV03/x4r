# x4

The Mac side of x4r: a CLI and an MCP server that talk to the X4 over USB serial
([docs/PROTOCOL.md](../docs/PROTOCOL.md)) and keep your reading data in `X4_HOME`
([docs/DATA.md](../docs/DATA.md) for record formats).

## Install

```bash
uv tool install --editable ./companion
```

The port is autodetected (Espressif USB, then `/dev/cu.usbmodem*`). Override with `--port` or
`X4_PORT`. `X4_PORT=sim:<dir>` runs a simulated X4 with `<dir>` as its SD card.

Every command prints text, or one JSON object with `--json`. Exit codes: 0 ok, 1 device or IO
error, 2 invalid data (the message names file, line and field).

## Commands

```bash
x4 status                                      # battery, SD space, the day, retention, minutes per section, records not pulled
x4 pull                                        # every device log and stats; or one of them
x4 pull highlights
x4 feed                                        # candidates for the issue: news, feeds, inbox, interests
x4 feed --topic china                          # one topic only; the run is not remembered
x4 push deck ~/x4/decks/meditations.jsonl     # validate, upload to /x4/decks/<deck-id>.jsonl
x4 push glossary books/meditations.epub terms.json   # term -> definition JSON, built as StarDict
x4 push book ~/Downloads/paper.pdf --to /books/papers/
x4 sync                                        # pull all, push what changed, fetch the issue
x4 stage                                       # no X4 on USB: the issue into ~/x4/outbox/ for Wi-Fi
x4 dashboard                                   # a page on this Mac for sections, interests, sources, fields, football, film
x4 serve                                       # serve the outbox on the LAN; --port, --host
x4 serve install                               # run x4 serve at login (launchd); uninstall removes it
x4 football                                    # results, fixtures and tables into ~/x4/football.json
x4 context                                     # JSON summary for writing the next sync
x4 rss sync                                    # feeds in ~/x4/feeds.yaml
x4 font install                                # the six curated families; --force rebuilds
x4 font add Crimson Pro                        # any Google Fonts family, by name
x4 font add Georgia.ttf "Georgia Bold.ttf" --name Georgia    # or TTF/OTF files; sizes 12,14,16,18 pt
x4 dict install hu
x4 words install hu                            # word lists and 200 mini crosswords for the word games
x4 words grids hu --count 6                    # filled grids without clues, for Claude to clue
x4 chess install                               # 4000 Lichess puzzles in ten rating bands
x4 ls /x4
x4 get /x4/stats.json stats.json
x4 put notes.epub /books/
x4 rm /books/old -r
x4 screenshot screen.png
x4 press confirm --long                        # back confirm left right up down power
x4 screen                                      # activity on screen
x4 lag                                         # last presses, timed to the panel update
x4 backup --out backups                        # esptool: dump and verify all 16 MB flash
x4 flash firmware.bin                          # protocol OTA, or esptool once a backup exists
x4 mcp                                         # MCP server on stdio (no backup or flash)
x4 sim /tmp/sd                                 # serve a simulated X4, prints its port
x4 sim /tmp/sd --lossy                         # one whose port loses bytes like the X4's
```

Fonts come from [google/fonts](https://github.com/google/fonts) and are kept in
`~/.cache/x4/fonts` (`$XDG_CACHE_HOME/x4/fonts`). Each family becomes one `.cpfont` per size in
`/fonts/<Family>/` with regular, bold, italic and bold italic, Latin Extended included; glyphs a
font lacks come from Noto Sans. Literata is the reader font on x4r; until it is installed the
device reads in Noto Sans.

`dict install` and `words install` build from the same Wiktionary extract (kaikki.org), kept in
`~/.cache/x4/dicts`. Word games rank words by
[hermitdave/FrequencyWords](https://github.com/hermitdave/FrequencyWords) (OpenSubtitles 2018),
or, offline, by how many senses and translations Wiktionary gives them.

`chess install` streams the [Lichess puzzle database](https://database.lichess.org/#puzzles)
(CC0, about 300 MB, kept in `~/.cache/x4/chess`) and keeps 400 puzzles in each of ten rating
bands from 800 to 2400: popular, often played, settled ratings, 2–4 moves to find.

## Crosswords

English crosswords are Wiktionary glosses, built by `words install`. Hungarian ones are
written by Claude: `x4 words grids hu` fills 5x5 grids from frequent Hungarian headwords (no
names, abbreviations or bare inflected forms; none of the answers of the last 30 puzzles),
numbered after the last id in `crosswords/hu.jsonl`, and saves them to `crosswords/hu.grids.jsonl`.
Claude adds a `clue` to each `answer` and appends the lines to `crosswords/hu.jsonl`. `x4 sync`
checks the file and pushes it when it changed; `words install` then leaves the device's
crosswords alone. It reads the lexicon `words install` cached, so that runs once first.

A clue is one line of text, at most 96 bytes and three lines on the device (Noto Sans 14, 394
px wide), in characters the device font has, and never contains its answer as a word or the
start of one. The device plays new lines in file order, so puzzles are only appended.

Files pushed from outside `X4_HOME` are copied into it, so `x4 sync` always works from there.
`sync` deletes a device deck only if `x4` pushed it and its local file is gone.

## Feed and the issue

`x4 feed` gathers candidates since its last run; Claude picks and writes `read.jsonl`;
`x4 sync` fetches and uploads.

- `news`: only with a news engine set up (`news_engine` in `settings.yaml`, or
  `X4_NEWS_ENGINE`; the engine is not part of this repo and needs Node 22+): snapshot, then
  discover for every day since the last run. Stories are listed by `k`, the number of
  independent sources: a hint for the writer, who chooses. Each source URL is reported once,
  so a known story returns only with new coverage. A story is used up only when a writer who
  could print it saw it: a run after the day's issue went out holds its stories for the next
  issue (two days at most), and so does a story below the first 60. A source with
  `"piece": false` counts in `k` but is never the piece: the government-aligned press, and
  paywalled outlets, which also carry `"paywalled": true`. Those come last in `sources`.
- `primary`, on the top 8 stories: the documents a story rests on, each `{title, url, kind,
  date}`; `kind` is `gazette`, `law`, `court`, `parliament`, `government`, `international`,
  `state-media` or `document`. From the engine: Közlöny items its clusters hold, the Közlöny
  index (`kozlony.mjs`, the 3 newest issues) for acts the coverage cites by number or states in
  its headline (the engine's claim clustering, run by `src/x4/engine/link.mjs`), and its
  watched EU dossiers (`eurlex.mjs`) when a headline names one. From the coverage: links in the
  article body to official domains and PDFs, and EU acts cited as `(EU) 2024/1689`.
- `follow`: new entries from `feeds.yaml`, `max_per_feed` (2) per feed and `max_per_topic`
  (10) per topic, each feed's newest first. A feed's `topic` goes into its entries' `topics`;
  `only_topic: true` keeps only entries whose words name that topic (for mixed feeds).
  [`examples/home/feeds.yaml`](examples/home/feeds.yaml) is a starter list. State media (Xinhua,
  Global Times, People's Daily) are used only as primary sources for official positions,
  never as news or analysis.
- `topics` on every story and entry: `hungary`, `us`, `eu`, `palestine`, `china` by the
  keyword, language and domain rules in `src/x4/topics.py`, plus a feed's own topic.
- `inbox`: links in `inbox.md`. `interests`: `interests.md`.
- `warnings`: what could not be gathered, and the fix. `x4 feed` itself never fails on them.

`read.jsonl` lines are the issue's pieces (docs/DATA.md, "The issue"). They need `id` (`r-…`),
`kind` (`news`, `follow`, `found`, `link`, `translation` for an article Claude digested,
`dossier` for a club dossier, `field`, `film`, `analysis`), `section`, `head` and `lead`, and
`url` or `file`. A `news` piece also needs `test`, the editor's test its story passed
(`life`, `power` or `reach`); it stays on the Mac, in `sent.jsonl`.
A `file` is a saved web page (`.html`, cleaned like any page) or an article in Markdown
(`.md`), converted as written:

```markdown
---
title: A tűzifa-rendelet
source: LátóTér
lang: hu
author: Claude
date: 2026-11-03
translated_from: {url: "https://www.thepaper.cn/…", lang: zh, source: 澎湃新闻}
sources:
  - {title: 345/2026. (X. 30.) Korm. rendelet, url: "https://magyarkozlony.hu/…"}
---
Headings, *emphasis*, lists, > quotes, tables and ![images](map.png) by path or URL; [1]
points at the first of `sources`, which end the article as a numbered list.
```

`sync` fills `title`, `source`, `lang` and `minutes` (230 words a minute) when left out,
uploads `/books/read/<id>.epub` when it changed and writes `/x4/read.jsonl` with its header;
the X4 removes the articles no longer listed or kept. An article that fails to fetch is left
out with a warning. Links uploaded as `link` items leave `inbox.md`.

`dossier` items are Claude's weekly club dossiers, Markdown files like any article Claude
writes.

## Football

`x4 football` reads `~/x4/football.yaml` (start from
[`examples/home/football.yaml`](examples/home/football.yaml)) and the key in `~/x4/football.key` (a free
one from football-data.org) and
writes `football.json` (docs/DATA.md): each club's matches from 7 days back to 14 ahead, and
each competition's table, the followed clubs marked `mine`. It keeps to the free tier's 10
calls a minute, waiting when the API says so. A club or competition the key cannot see is a
warning. `x4 sync` and `x4 stage` run it first when the file has a key; a failure there is a
warning and the last `football.json` goes.

## Issues over the LAN

When the X4 is not plugged in, `x4 stage` builds the issue the way `x4 sync` would (football
first, the issue fetched and converted)
and writes it to `~/x4/outbox/`: the files at their device paths and `manifest.json`
(docs/PROTOCOL.md, "LAN issue"). It refuses an issue over the limits (48 files, 4 MB each, 16
MB in all) and replaces the outbox whole. Link items leave `inbox.md`, and the feed moves on.

`x4 serve` serves the outbox on the Mac's LAN address, port 8744, to requests carrying the
token in `~/x4/serve.json` (made on first use). When the X4 reports what it took, those files
count as pushed, so the next `x4 sync` skips them. `x4 sync` writes the address and token into
`/x4/net.json` and empties the outbox. `x4 serve install` adds a launchd agent
(`~/Library/LaunchAgents/x4r.serve.plist`, log in `~/Library/Logs/x4-serve.log`)
that runs it at login and again when the Mac's address changes.

## X4_HOME (default `~/x4`)

```
notes/<title>.md          highlights per book, by chapter; rewritten on every pull
decks/<deck-id>.jsonl     cards
stream.jsonl              bites, resurfaced highlights, book puzzles
read.jsonl                the issue's pieces, with url or file each
glossary/<book key, / as __>.json   term -> definition
crosswords/<lang>.jsonl   crosswords Claude clued, pushed by sync
crosswords/<lang>.grids.jsonl   the last x4 words grids, waiting for clues
feeds.yaml                RSS feeds: {url, name, topic, only_topic}; start from examples/home/feeds.yaml
inbox.md                  saved links, one per line; # lines are comments
interests.md              what you want more of, in your words
pulled/highlights.jsonl   everything pulled, deduplicated; *.bad holds unreadable lines
pulled/reviews.jsonl
pulled/seen.jsonl         stream items the device has shown, with book puzzle results
pulled/play.jsonl         finished games
pulled/read-log.jsonl     books and pieces finished or left
cache/                    fetched articles as EPUB, by URL; safe to delete
stats.json                last pulled /x4/stats.json
football.yaml             clubs and competitions; start from examples/home/football.yaml
football.key              the football-data.org key, one line; never sent to the data repo
football.json             the last x4 football
outbox/                   the staged issue: files at device paths and manifest.json
serve.json                x4 serve's port and token
.sync.json                what was last pushed, by CRC
.context.json             what `x4 context` already reported
.feed.json                when `x4 feed` last ran and what it reported
```

Stream items of kind `highlight` need only `{"id","kind":"highlight","highlight":"<id>"}`:
`sync` and `stage` copy `text` and `title` in from `pulled/highlights.jsonl`, since pulled
highlights leave the device. The file in `X4_HOME` stays as written.

A question highlight counts as answered once a card or stream item has `"highlight": "<its id>"`.

## Tests

```bash
uv run --directory companion pytest
```
