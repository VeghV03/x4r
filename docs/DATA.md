# x4 data on the SD card, v2

Plain JSON Lines wherever a person or Claude reads or writes the data. One record per line,
UTF-8, `\n` line ends. Unknown fields are ignored by every reader, so fields can be added later.

Ownership decides who writes a file. The other side only reads it.

| Path | Owner | What |
|---|---|---|
| `/books/` | host | EPUB and XTC/XTCH books. Any folder layout. |
| `/fonts/` | host | `.cpfont` font packs (CrossPoint format). |
| `/dictionaries/<name>/` | host | StarDict dictionaries (English, Hungarian). |
| `/x4/glossary/<key-hash>/` | host | StarDict glossary for one book, written by Claude. |
| `/x4/decks/<deck-id>.jsonl` | host | Cards. |
| `/x4/stream.jsonl` | host | Bites, resurfaced highlights and book puzzles. |
| `/x4/read.jsonl` | host | The issue: its header, then its pieces by section. |
| `/books/read/` | host | The articles of the issue and of Kept, one EPUB per piece, named `<read id>.epub`. |
| `/x4/words/<lang>/` | host | Word lists for the word games (`en`, `hu`). |
| `/x4/crosswords/<lang>.jsonl` | host | 7x7 crosswords, played in file order. |
| `/x4/chess/puzzles.txt` | host | Chess tactics, sorted by rating. |
| `/x4/threads.jsonl` | host | One line per running news story: the story so far, shown before its next piece. |
| `/x4/previously.jsonl` | host | One recap per book in progress, shown when the book opens after days away. |
| `/x4/next.jsonl` | host | Up to three books to follow a book, listed by title at its end. |
| `/x4/net.json` | host | The calendars and the weather place the X4 fetches over Wi-Fi, the zone, and where the issue is: the Mac on the LAN, or the data repo. |
| `/x4/football.json` | host | Results, fixtures and tables of the followed clubs. |
| `/x4/highlights.jsonl` | device | Highlights, questions and their deletions, append-only. |
| `/x4/reviews.jsonl` | device | One line per answered card, append-only. |
| `/x4/seen.jsonl` | device | One line per stream item shown, append-only. |
| `/x4/play.jsonl` | device | One line per finished game, append-only. |
| `/x4/read-log.jsonl` | device | One line per book or piece closed, append-only. |
| `/x4/kept.jsonl` | device | One line per piece kept or let go, append-only. |
| `/x4/visits.jsonl` | device | One line per stay on a section's screen or Play's, append-only. |
| `/x4/lookups.jsonl` | device | One line per word looked up in the dictionary, append-only. |
| `/x4/stats.json` | device | Current totals, rewritten in place at rest (2 s without a press), before sleep and before the Mac reads a file. |
| `/x4/agenda.json` | device | What the last fetch over Wi-Fi brought: the next days' events and forecast. |
| `/x4/state/` | device | Private scheduler and progress state. The host never reads it. |

The host never edits a device-owned file. To pull an append-only log it renames it first
(`mv /x4/reviews.jsonl /x4/reviews.pulling`), reads the renamed file, then removes it. The
device opens, appends and closes the log for every record, so a record written during a pull
lands in a new file and is picked up next time.

## Keys

- **book key**: the book's path on the SD card with the leading `/` removed, for example
  `books/meditations.epub`. Used in every record that refers to a book.
- **key hash**: FNV-1a 64 of the book key's UTF-8 bytes, as 16 lowercase hex digits. It names a
  book's glossary folder, so the path stays short whatever the book is called, and finds its
  reading totals on the device.
- **deck id**: `[A-Za-z0-9._-]`, at most 32 bytes. It is the deck's file name.
- **card key**: `<deck-id>/<card id>`. Scheduler state is keyed by it. Rewriting a card with
  the same id keeps its history; a new id is a new card. Removing a card from the deck file
  retires it.

## Time: the day

`day` is the local date as a whole number: days since 1 January 1970 in the host's zone.

- With a known clock the day is the date.
- Without one the day stands, and every 20 minutes spent in the reader, in books and pieces,
  adds one to it. Games do not count. A day counted before the X4 ever saw a clock is a small
  number from 1, no date.
- When the clock returns, the day is the date again. If the X4 had counted past it, every
  card's due day moves back by the difference, so no interval grows, and the daily puzzle
  and the bite are not held up while the calendar catches up.
- The scheduler measures in days. Every device record carries `day`. A record from before the
  day (v9 step 7) carries `set` instead; the Mac puts it on the date of its `at`, or on no day.
- The firmware that brought the day moved a card due in n sets to n days, with the last set
  on the day of the last clock it had seen.
- `at` fields hold Unix seconds when the device knows the wall clock, otherwise `null`. It
  knows the clock after `hello` or `time` until power is cut. On battery, sleep cuts power;
  on USB power the clock survives sleep.

## Host-written

### Deck: `/x4/decks/<deck-id>.jsonl`

Line 1 is the header. Every following line is one card.

```json
{"deck":"meditations","title":"Meditations","book":"books/meditations.epub"}
{"id":"c1","type":"flip","front":"What does Marcus mean by the 'inner citadel'?","back":"The part of the mind no outside event can reach.","highlight":"h-1a2b"}
{"id":"c2","type":"choice","prompt":"Which is within our control?","options":["Reputation","Judgement","Health"],"answer":1,"why":"Only our judgements are fully ours."}
{"id":"c3","type":"cloze","text":"The {{impediment}} to action advances action.","options":["impediment","fear","desire"]}
{"id":"c4","type":"explain","prompt":"Explain the dichotomy of control in your own words.","points":["Some things are up to us","Judgement, impulse, desire are","Body, property, reputation are not"]}
```

| Type | Required fields | On device |
|---|---|---|
| `flip` | `front`, `back` | Show front. Confirm shows back. Grade. |
| `choice` | `prompt`, `options` (2–4), `answer` (index) | Pick one. Right or wrong is shown with `why` if present. Graded automatically. |
| `cloze` | `text` with one `{{term}}`, `options` (2–4, includes the term) | Pick the missing term. Graded automatically. |
| `explain` | `prompt`, `points` (1–5) | Think it through. Confirm shows the points. Grade. |

The device asks one of a deck's cards at the end of a chapter of its `book`, or of the article
it names; a deck without `book` after any piece. The host writes no new `flip`
cards: the reader grades those, and a card you grade yourself is answered without thinking.

Optional on every card: `highlight` (id from `highlights.jsonl`), `why`. Text fields are plain
text; a blank line starts a new paragraph. Limits: `id` 32 bytes, each text field 1000 bytes,
1000 cards per deck, 64 decks. Past 64 the device uses the first 64 deck ids in name order and
ignores the rest; an ignored deck keeps its scheduler state until its file is removed.

### Stream: `/x4/stream.jsonl`

Bites, resurfaced highlights and book puzzles, in the order they should appear. The device
shows each item once, in file order. One bite or highlight a day, the first not yet seen,
shows on the day's first opening of a book; the book puzzles are Play's Book puzzles row,
which plays the next one not yet seen. The host replaces the whole file on every sync,
keeping unseen items it still wants shown.

```json
{"id":"s-2026-11-03-1","kind":"bite","title":"Why Marcus wrote to himself","body":"…","book":"books/meditations.epub"}
{"id":"s-2026-11-03-2","kind":"highlight","highlight":"h-1a2b","text":"…","title":"Meditations"}
{"id":"s-2026-11-03-3","kind":"odd","prompt":"Which one does not belong?","options":["…","…","…"],"answer":2,"why":"…","book":"books/meditations.epub"}
```

| Kind | Fields | On device |
|---|---|---|
| `bite` | `title`, `body` (at most 1 screen at the default size), optional `book`, optional `highlight` (the question it answers) | Read. Next. |
| `highlight` | `highlight`, `text`, `title` | The highlight's text and book title, resurfaced. Next. The host copies `text` and `title` in, because pulled highlights leave the device. |
| `riddle` | `prompt`, `answer`, optional `book` | Show the riddle. Confirm shows the answer. Got it / Missed. |
| `order` | `prompt`, `items` (3–6, in the correct order), optional `why`, `book` | Items appear shuffled; the reader puts them in order. Checked automatically. |
| `odd` | `prompt`, `options` (3–5), `answer` (index), `why`, optional `book` | Odd one out: pick it. Checked automatically. |
| `logic` | `prompt` (may span screens), `options` (2–4), `answer`, `why`, optional `book` | A reasoning puzzle from the book. Checked automatically. |

`riddle`, `order`, `odd` and `logic` are the book puzzles. Their results go to `seen.jsonl`.

`body` and `prompt` limit: 4000 bytes; `items[]` and `options[]`: 200 bytes each. `id`
limit: 32 bytes, unique. `x4` refuses any other kind (the set's `article`, `brief`, `letter`,
`game`, `forecast`, `outcome` and `test`), and the X4 skips them. `end` is ignored.

### Threads: `/x4/threads.jsonl`

```json
{"id":"t-budget-2027","title":"A 2027-es költségvetés","body":"…","since":"2026-10-28","updated":"2026-11-10"}
```

One line per running story, replaced whole on every sync: `id` (32 bytes), `title` (200
bytes) and `body`, the story so far: one paragraph of at most 600 bytes, which is one screen.
Any other field (`since`, `updated`) is the host's own; the X4 ignores it. 32 threads at
most; a story that ended leaves the file.

A piece names its thread by `"thread":"<id>"`. Opening the piece shows the thread first when
it is due: the first time after its title or body changed. `x4` refuses a piece whose
`thread` is not in the file; on the X4 such a piece opens without it.

### The issue: `/x4/read.jsonl`

Line 1 is the header. Every following line is one piece, in its section's order. Claude
writes the pieces on the Mac as `~/x4/read.jsonl`, each with a `url` (or a local `file`);
`x4 sync` and `x4 stage` fetch, convert and upload, write the header from `sections.md`, and
write the device copy with `book` instead. A piece whose article cannot be fetched is left
out of the device copy.

```json
{"date":"2026-11-12","sections":[{"id":"hungary","name":"Hungary"},{"id":"world","name":"World"},{"id":"institutions","name":"Institutions"},{"id":"design","name":"Design"},{"id":"field","name":"Field"},{"id":"football","name":"Football"},{"id":"film","name":"Film"}]}
{"id":"r-2026-11-12-1","section":"hungary","kind":"news","head":"…","lead":"…","title":"…","source":"Telex","lang":"hu","minutes":4,"book":"books/read/r-2026-11-12-1.epub",
 "thread":"t-budget-2027","predict":{…},"forecast":{"id":"f-2026-11-12-1","text":"…","by":"2026-12-16"}}
```

| Header field | |
|---|---|
| `date` | The issue's day, `YYYY-MM-DD`. One issue per date. |
| `sections` | The front page's rows above Play, Book and Library, in order: `id` (`[a-z0-9-]`, 16 bytes) and `name` (24 bytes). 8 at most: that is what the front page holds. A section listed with no pieces has its row and no line. `football` is the Football screen, and its pieces are that screen's first tab. |
| `outcomes` | Not built yet (X59). Forecasts settled since the last issue, each one line on top of its section: `section`, `forecast`, `text`, `p`, `happened`, `body`. |

| Piece field | |
|---|---|
| `id` | `r-` id, 32 bytes, unique. Letters, digits, `.`, `_` and `-` only: it is the file name. |
| `kind` | `news`, `follow` (a source you follow), `found` (an article Claude found on your interests; `long` in records before v3), `link` (a link you saved), `translation` (a digest Claude wrote of a foreign-language article), `dossier` (Claude's weekly dossier on a followed club), `field` (the field of the week's primer or primary text), `film` (one of the week's picks), `analysis` (football). `story` is refused. |
| `section` | A section's `id`. A piece without one is in no section and must be `kept`. |
| `head` | The piece's headline for the front page, written by Claude in the piece's language: 30 characters at most, which is one line on the X4 (32 fit on the Mac's drawing; the X4 measures a little wider). Without it the front page cuts `title`. |
| `lead` | What the piece says, in the piece's language: 60 characters at most, which is the two lines the section shows. |
| `title`, `source`, `lang`, `minutes` | The article's own headline and outlet, shown on the section's screen (`minutes`: estimated reading time). Optional on the Mac: `x4 sync` fills them from the article. |
| `url`, `file` | Mac copy only: where the article comes from, one of the two. A `file` is a saved page (`.html`) or Markdown with front matter (`.md`: `title`, `source`, `lang`, `author`, `date`, `translated_from`, `sources`; see companion/README.md). |
| `book` | Device copy only: the article's book key. |
| `thread` | A thread's id in `threads.jsonl` (above). |
| `predict`, `link`, `forecast` | What the piece asks, below. |
| `kept` | `true` on a link saved in `inbox.md`: it arrives in the Library's Kept, in no section. |
| `test` | Mac copy only, on every `news` piece and no other: the editor's test its story passed, `life` (it changes daily life), `power` (it shifts power) or `reach` (everyone is talking about it). `x4` keeps it in `sent.jsonl` and never sends it to the X4. |

A piece is read when a `read-log.jsonl` line says `finished`. A section's line on the front
page is the `head` of its first piece not read. 32 pieces at most.

The pieces of an issue are fixed. On a date whose issue was already sent, `x4` sends the same
pieces again whatever `read.jsonl` on the Mac says, and tells what it left out; only `kept`
pieces can be added. Books, cards, threads and football still go.

The X4 takes a file with a later `date` as a new issue: every piece the new file does not
list and that is not kept leaves, with its article (Kept, under Device-written). A weekly
piece (Field, Film) is listed again, under its id, in every issue of its week; a piece listed
again keeps its article and whether it was read.

#### What a piece asks

Before the article, a piece may ask a question the article answers:

```json
"predict":{"prompt":"Which exercise do most people drop first?","options":["The morning text","The evening review","The cold shower"],"answer":1,"quote":"By Thursday most had given up the evening review."}
```

`predict`: `prompt` (1000 bytes), `options` (2–4, 200 bytes each), `answer` (index) and `quote`
(1000 bytes), the article's own sentence that settles it. It is asked when the piece is
opened, until it is guessed: one press guesses, and nothing is marked. Back from the article,
once a page was turned in it or its end reached, the question returns with the guess and the
answer marked and the quote under them. The X4 keeps the guess in `/x4/state/guess.bin`, so a
sleep in the middle of the article loses nothing. `x4` refuses a `quote` that is not in the
article's text (`x4 article <read id>` prints it; quote marks, dashes, case and spacing are
set aside).

After the article, once it was read, a piece may ask one thing more: a link or a forecast,
never both (`x4` refuses that). A due card follows only a piece with neither.

```json
"link":{"prompt":"Which of these says the opposite?","options":[{"text":"You have power over your mind","ref":"h-1a2b"},{"text":"A Brief History of Tea","ref":"r-2026-10-20-2"},{"text":"Meditations","ref":"books/meditations.epub"}],"answer":0,"why":"…"}
```

`link`: `prompt` (1000 bytes), which names the kind of connection to look for; `options`,
exactly three of `{"text" (200 bytes), "ref"}`; `answer` (index); `why` (1000 bytes). One
press picks, the one that connects is marked, with `why`. `ref` says what the option is: a
highlight's id, a finished piece's id or a book's key. The X4 does not read it; `x4` refuses
a `ref` that is not in `linkable` in `x4 context`, one named twice, or the piece itself.

`forecast`: `id`, `text` (a statement that will be plainly true or false, 1000 bytes) and `by`
(the day it is settled, `YYYY-MM-DD`): the statement, its day, and five rows, 10%, 30%, 50%,
70%, 90%, how likely. The selection starts on 50; one press picks. `by` is at most 42 days
after the forecast was first sent and not in the past; `x4` refuses others, and more than two
an issue. One whose day has passed is not asked.

### Word lists: `/x4/words/<lang>/`

Lowercase UTF-8, one entry per line. Built by `x4 words install <lang>`.

| File | Order | What |
|---|---|---|
| `answers5.txt` | most common first | Common five-letter words: daily and practice answers. The level picks how far into the file the answer comes from. |
| `valid5.txt` | by bytes | Every accepted five-letter guess (includes the answers). |

Files sorted by bytes are binary-searched on the device. Letters are Unicode characters:
Hungarian `sz` is two letters, `ő` is one. The letters are `a`–`z` in English and
`a á b c d e é f g h i í j k l m n o ó ö ő p q r s t u ú ü ű v w x y z` in Hungarian; a word
with any other character is left out.

### Crosswords: `/x4/crosswords/<lang>.jsonl`

```json
{"id":"hu-0042","size":7,"rows":["...láva","..kapál",".lakoma","dér.lop","alifás.","dekás..","akad..."],"across":[{"n":1,"clue":"Kihűlt magma"},…],"down":[…,{"n":6,"clue":"Psziché","src":"Elmélkedések"},…]}
```

`rows` are lowercase, `.` is a black square; clue numbers follow the usual reading order. A
run of one white cell is not an entry. Answers are rare words: first ones the user highlighted
or looked up, then rare words from what they read, the rest from the word list past its most
frequent 3000. Claude writes the clues: `x4 words grids <lang>` makes the grids (seeded with
`--words` or `--words-file`), Claude writes `X4_HOME/crosswords/<lang>.jsonl`, and `x4 sync`
checks it and pushes it. Without a host file, `x4 words install <lang>` pushes 200 generated
puzzles with short Wiktionary glosses (in English) as clues; a host file replaces them, and
`words install` keeps it.

| Field | |
|---|---|
| `id` | `<lang>-<number>`, at most 15 bytes, unique. New puzzles take the next number. |
| `size` | 7. Lines of size 5 written before still play; the device skips any other. |
| `rows` | `size` rows of `size` letters of the language, `.` for a black square. Every white cell is in a word. |
| `across`, `down` | `{"n", "clue"}` in number order, exactly the entries the rows give, plus `src` on a word from the user's reading. On the Mac an entry may keep its `answer`, which must match the grid, and `seed`; the device copy leaves both out. |
| `clue` | One line, at most 96 bytes and three lines on the screen (Noto Sans 14, 394 px), only characters the device font has. Never contains its answer as a word or the start of one (any case, accents kept). |
| `src` | Optional. The title of the book or article the word came from, at most 48 bytes, one line, characters the device font has. At most six different titles per puzzle. A solved puzzle lists each title over its words. |

A line is at most 4095 bytes. The device plays the lines in order and remembers the next one
per language, so puzzles are appended: a new line comes next. Once every line is played, earlier
puzzles come back, picked by the game's seed, until more are appended. A file shorter than where
play got to is a new file and starts from its first line.

### Chess puzzles: `/x4/chess/puzzles.txt`

```
00sHx,1760,q3k1nr/1pp1nQpp/3p4/1P2p3/4P3/B1PP1b2/B5PP/5K2 b k - 0 17,e8d7 a2e6 d7d8 f7f8
```

One puzzle a line, `<id>,<rating>,<fen>,<moves>`, from the
[Lichess puzzle database](https://database.lichess.org/#puzzles) (CC0). Built by
`x4 chess install`.

| Field | |
|---|---|
| `id` | The Lichess puzzle id (lichess.org/training/`<id>`), at most 15 bytes. |
| `rating` | Its Lichess rating. |
| `fen` | The position before the opponent's move, as in the Lichess file. |
| `moves` | UCI moves separated by spaces, 4 to 16 of them, an even count. The first is the opponent's: the device plays it. Then the player's move, the opponent's reply, and so on; the line ends with the player's move. Castling is the king's move (`e1g1`), a promotion ends with the piece (`e7e8q`). |

The file is sorted by rating and holds the same number of puzzles in each of ten bands of 160
points from 800 to 2400, so level 1–10 plays from the band at that tenth of the file. A line is
at most 255 bytes; the device skips lines it cannot read or whose moves are not legal.

The device takes the listed move, or any move that mates. A pawn reaching the last rank becomes
the piece the listed move names when it goes to the same square, else a queen; so an
underpromotion is found by playing the pawn to its square.

### Glossary: `/x4/glossary/<key-hash>/`

A normal StarDict folder (`glossary.ifo`, `.idx`, `.dict`), named by the book's key hash. Lookup while reading a book checks that
book's glossary first, then every folder in `/dictionaries/` in name order.

### Recaps: `/x4/previously.jsonl`

One line per book, written by Claude at sync (`X4_HOME/previously.jsonl`), checked and pushed
by `x4 sync`:

```json
{"book": "books/meditations.epub", "progress": 0.41, "text": "Where the book was going when you stopped."}
```

- `book`: a book key, at most 512 bytes, one line per book, never a Read article.
- `progress`: the book's progress from `x4 context` when the recap was written, 0 to 1.
- `text`: one paragraph, at most 65 words and 600 bytes, so it fits one screen.

`x4 excerpt <book key>` gives the last words before the reader's place (by default 2500, at the
progress of the last pull), found as the device measures progress: by the inflated size of each
spine item. `x4 context` reports each book's recap progress as `previously`.

The device keeps each book's reading totals and when it was last read, by day and wall-clock
time, in `/x4/state/books.bin`, found by key hash (keys up to 479 bytes). Opening a book shows its recap before the page
when the book was last read 3 or more days ago, or 48 hours ago by a clock known both
times, and its progress is still within 1 percent point of the recap's. Confirm or Next goes
on to the page; Back goes home.

### Next books: `/x4/next.jsonl`

Claude writes `X4_HOME/next.jsonl` at sync for books near their end, from `x4 shelf` (unread
books on the card by key, and the user's own EPUBs in `X4_HOME/library` as `library/<file>`):

```json
{"after": "books/meditations.epub", "books": ["books/letters.epub", "library/Seneca.epub"]}
```

- `after`: a book key, one line per book.
- `books`: 1 to 3 keys from `x4 shelf`, best first, never `after` itself.

`x4 sync` checks every pick is there before the card changes, then sends each library pick as a
sample to `books/<title slug>.epub`: the chapters up to 6000 words, later chapters left in the
spine empty (so positions match the whole book) and their images dropped. At the sync after the
sample was read (reading time, pages or progress in `stats.json`), the whole book replaces it at
the same path, and the book's device cache is cleared except `progress.bin`, so the reader keeps
its place. Samples no longer picked and never opened are removed. The device file carries titles:

```json
{"after": "books/meditations.epub", "books": [{"book": "books/letters.epub", "title": "Letters from a Stoic"}]}
```

The end-of-book screen lists the picks on the card by title, in place of the book folder's next
files. `x4 context` reports each book's picks as `next`.

### Online: `/x4/net.json`

Written by `x4 calendar`, `x4 weather`, `x4 repo` and `x4 sync` from `X4_HOME/net.json`,
`serve.json` and `repo.json`:

```json
{"tz":"CET-1CEST,M3.5.0,M10.5.0/3","calendars":[{"url":"https://calendar.google.com/calendar/ical/…/basic.ics"}],"weather":{"lat":47.4981,"lon":19.0402}}
```

- `tz`: the Mac's zone as a POSIX TZ rule, from its `/etc/localtime`. Without it the X4 uses the
  host's last offset, without summer time.
- `calendars`: up to 4 iCal addresses, https, under 400 bytes each. A Google calendar's is the
  secret address in iCal format (the calendar's settings in Google Calendar). Anyone holding
  it can read the calendar, so this file is as private as the Wi-Fi password.
- `weather`: where the forecast is for; Open-Meteo, no key.
- `mac`: where `x4 serve` listens on the LAN, `{"url":"http://192.168.1.23:8744","token":"<32 hex>"}`.
  `x4 sync` writes the Mac's current IPv4 address. See "LAN issue" in [PROTOCOL.md](PROTOCOL.md).
- `repo`: the data repo on GitHub, `{"name":"<owner>/<repo>","token":"<token>"}`, from
  `X4_HOME/repo.json`, which `x4 repo set <name>` writes after asking for the token and trying
  it on GitHub. The token is a fine-grained one that reads and writes the contents of that repo
  and nothing else; `x4` never prints it. The name is two parts of `[A-Za-z0-9._-]`, under 80
  bytes; the token at most 255. With
  `repo` the X4 takes the issue from the repo on any saved network, sends its records there,
  and does not ask the Mac. See "Repo issue" in [PROTOCOL.md](PROTOCOL.md) and "The data repo"
  below. Without `mac` and `repo` the X4 takes no issue over Wi-Fi.

The X4 reads the events overlapping today and the next three days, recurring ones expanded:
daily, weekly (`BYDAY`, `INTERVAL`, `WKST`), monthly (`BYDAY` with or without an ordinal, one
`BYMONTHDAY`) and yearly, with `COUNT`, `UNTIL`, `EXDATE`, edited and cancelled instances
(`RECURRENCE-ID`, `STATUS:CANCELLED`), and times in UTC, floating, as dates, or in any zone the
feed's `VTIMEZONE` blocks describe. Any other rule shows its first occurrence only. At most 32
events are kept, the earliest.

### Football: `/x4/football.json`

Written by `x4 football` from football-data.org; `x4 sync` and `x4 stage` run it first when
there is a key.

```json
{"fetched":1794042000,
 "matches":[{"at":1793970000,"comp":"PL","home":"Brighton","away":"Brentford","score":[2,1]},
            {"at":1794150000,"comp":"CL","home":"Barcelona","away":"Inter"}],
 "tables":[{"comp":"PL","name":"Premier League","rows":[{"pos":1,"team":"Arsenal","played":11,"gd":15,"points":26},
                                                       {"pos":6,"team":"Brighton","played":11,"gd":4,"points":18,"mine":true}]}]}
```

- `matches`: the followed clubs' results of the past 7 days and fixtures of the next 14, by
  `at` (kick-off, unix seconds). `score` is `[home, away]` once the match is over, `[h, a]` with
  `"live":true` while it is played at fetch time, absent before. At most 40; a match of two
  followed clubs appears once.
- `comp`: the competition's code, at most 12 bytes (`PL`, `PD`, `SA`, `CL`). `team`, `home`,
  `away`: the short club name, at most 24 bytes.
- `tables`: up to 4, in the order of `football.yaml`; `rows` up to 36 (the CL league phase).
  `mine` marks a followed club.

`~/x4/football.yaml`, in the data repo:

```yaml
clubs: [81, 90, 397, 402]        # Barcelona, Real Betis, Brighton, Brentford
competitions: [PL, PD, SA, CL]   # tables, in this order
names: {397: Brighton}           # optional: team id to the name shown
```

The key is never in the repo: on the Mac it is `~/x4/football.key`, one line, or
`X4_FOOTBALL_KEY`. In the cloud run `X4_FOOTBALL_KEY` is `proxy`: the environment's network
secret adds the key to each request (K16). `x4` refuses a `football.yaml` with a `key`, and
does not send one to the repo.

A team shows under its `names` entry, else football-data's short name without a trailing FC or CF.

## Device-written

### `/x4/highlights.jsonl`

```json
{"id":"h-1a2b","kind":"highlight","book":"books/meditations.epub","title":"Meditations","author":"Marcus Aurelius","chapter":"Book II","text":"You have power over your mind, not outside events.","pos":{"spine":3,"start":1204,"end":1257},"day":20769,"at":null}
```

`kind` is `highlight` or `question`. A question is a passage you did not understand. Claude
explains it in a bite and may turn it into cards. `text` is at most 4000 bytes; a selection
stops growing there, so it is never cut. The id is a hash of book, `pos` and `text`: the same
passage highlighted again gets the same id.

A highlight deleted on the device leaves a deletion record:

```json
{"id":"h-1a2b","deleted":true,"day":20770,"at":null}
```

The device also blanks the highlight's own lines in place (spaces up to the line end, so
offsets stay valid). `x4 pull` applies records in order: a deletion removes a pulled
highlight, and a later record with the same id brings it back. A deletion for an id the host
never pulled is dropped.

A highlight made on a step (a bite, a resurfaced highlight, a card, a puzzle) names the step
in `item`:

```json
{"id":"h-5e6f","kind":"highlight","book":"books/meditations.epub","item":{"id":"s-2026-11-03-1","kind":"bite","title":"Why Marcus wrote to himself"},"text":"He wrote to remind himself.","pos":{"start":120,"end":147},"day":20769,"at":null}
{"id":"h-7a8b","kind":"question","book":"stream/s-2026-11-03-4","title":"Attention as a budget","item":{"id":"s-2026-11-03-4","kind":"bite","title":"Attention as a budget"},"text":"…","pos":{"start":0,"end":64},"day":20769,"at":null}
```

| Field | |
|---|---|
| `item` | `id`: the stream item id, or `<deck-id>/<card id>` for a card. `kind`: its stream kind or card type. `title`: its title, or prompt (at most 200 bytes). |
| `book` | The step's book key: the item's `book`, a card's deck `book`, a resurfaced highlight's book. A step without one names itself: `stream/<item id>`, with `title` the item's title. |
| `pos` | Byte offsets into the step's text: the fields that can be highlighted (a bite's or highlight's body, a revealed back or points, a why), joined by one byte each. No `spine`, `author` or `chapter`. |

The id also hashes the item id. The device keeps each one's range in `/x4/state/marks.bin` and
shades it when the step comes back; deleting it there logs a deletion as above.

### `/x4/reviews.jsonl`

```json
{"card":"meditations/c2","type":"choice","grade":3,"correct":true,"pick":1,"ms":5400,"day":20769,"at":null}
```

`grade` follows FSRS: 1 Again, 3 Good. `correct` and `pick` are present for `choice` and
`cloze`; `pick` is the option chosen, as its index in the card's `options`. `ms` is the time
from showing the card to the answer.

### `/x4/seen.jsonl`

```json
{"id":"s-2026-11-03-1","day":20769,"at":null}
```

`seen.jsonl` lines for book puzzles also carry `"correct"` (true or false) and `"ms"`; `odd`
and `logic` also `"pick"`, the option chosen, as its index in the item's `options`.

A piece's answers are lines by its Read id: the guess when it is made, and the link's answer
when it is picked. `x4` keeps both.

```json
{"id":"r-2026-11-12-3","opened":false,"predict":{"pick":2,"correct":false,"ms":4100},"day":20771,"at":null}
{"id":"r-2026-11-12-3","opened":true,"link":{"pick":0,"correct":true,"ms":6200},"day":20771,"at":null}
```

`pick` is the index in `options`, `ms` the time from the question to the answer. A forecast's
line is by the forecast's id, with the row picked in percent and its piece:
`{"id":"f-2026-11-12-1","p":70,"read":"r-2026-11-12-1","ms":5200,"day":20771,"at":null}`.

### `/x4/play.jsonl`

```json
{"game":"sudoku","mode":"daily","level":4,"won":true,"ms":412000,"day":20769,"at":null}
{"game":"words","lang":"hu","mode":"practice","level":3,"won":false,"tries":6,"ms":95000,"day":20769,"at":null}
```

| Field | |
|---|---|
| `game` | `sudoku`, `nonogram`, `einstein` (logic grid), `words` (five-letter word), `crossword`, `chess`, `arith`. |
| `mode` | `daily` (counts), `practice` (does not). `set`, a game played as a step of the old daily set, is no longer written. |
| `level` | The level the puzzle was played at, 1–10. |
| `won` | Solved, or the round passed. |
| `lang` | Word games only. |
| `score`, `tries` | Game-specific extras: guesses used (`words`), the puzzle's rating as `score` (`chess`). |

"Daily" means once a day: the day's daily is new each day, by the clock or by the counted
day. `day` is the day when the game ended. Difficulty adapts from every game played: a win moves the level
toward harder, a loss toward easier.

### `/x4/read-log.jsonl`

```json
{"book":"books/read/r-2026-11-03-7.epub","progress":1.0,"finished":true,"ms":980000,"day":20769,"at":null}
```

Written when the reader closes a book once a page of it was shown: a piece under
`/books/read/`, or any other book, which is how the Mac counts Book's minutes. `finished` is
true once the reader reached the end (progress 0.95 or more).

### `/x4/lookups.jsonl`

```json
{"word":"ataraxia","book":"books/meditations.epub","title":"Meditations","day":20769,"at":null}
```

Written when a word looked up in the reader's dictionary is found. `word` is the dictionary's
headword (the selection when it has none), at most 64 bytes; `book` is the book key, so a piece
is `books/read/<read id>.epub`; `title` is the book's title when it has one, at most 200
bytes. `x4 context` lists the words new since its last call, each once, with the title.

### `/x4/visits.jsonl`

Device-written, append-only. One line each time a section's screen is left:

```json
{"section":"world","issue":"2026-11-12","ms":41000,"day":20769,"at":null}
```

`section` is a section's id, or `play`. `ms` is the time on that screen, not counting time
inside a piece or a game, which have their own records. Every stay is written, however short.
Football's tabs are one visit. The front page, the Library and books are not logged here.

### Kept: `/x4/kept.jsonl`

Device-written, append-only. One line each time a piece is kept or let go: a hold of Confirm on a section's screen or in
the Library's Kept, or the first row of the menu inside the article:

```json
{"id":"r-2026-11-12-3","kept":true,"day":20769,"at":null}
```

The last line for an id says whether it is kept. The X4 holds each kept piece's line in
`/x4/state/kept.jsonl`, with the date of the issue it last settled, 32 at most; a hold past
that does nothing. The Library's Kept lists them: the unfinished first, the newest kept on
top, then the finished.

A kept piece is also stored whole, as its issue listed it, in `/x4/state/kept/<id>.json`, so
it still asks and answers once the issue has gone. What a piece was asked and whether it was
read is in `/x4/state/guess.bin`, 48 pieces at most.

The X4 settles an issue when its front page first shows it. The issue's `kept` links are
added, 8 an issue at most (`x4` refuses more). When the date is new, every kept piece that
was finished or let go and that the issue does not list is dropped, and every article under
`/books/read/` that neither the issue lists nor a kept piece needs is removed. So the host
no longer removes files there, over USB or in an issue's `prune`. A link let go stays in the
state as gone while its issue lists it, so it does not arrive twice.

### `/x4/agenda.json`

Rewritten whole after each fetch over Wi-Fi; a source that did not answer keeps what it had.

```json
{"fetched":1794042000,"events":[{"start":1794042000,"end":1794045600,"title":"Coffee","cal":0,"uid":2166136261}],"weather":[{"date":"2026-11-07","code":61,"high":12,"low":4}],"hours":[{"date":"2026-11-07","hour":9,"code":61,"temp":6,"rain":40}]}
```

`start`, `end` and `fetched` are unix seconds; an all-day event has `"all_day":true` and runs
from local midnight. `cal` is the calendar's place in `net.json`. `weather` is seven days:
`code` is the WMO weather code; `high` and `low` are °C. `hours` is the day ahead of the fetch,
in three-hour steps, in local time: `temp` in °C and `rain`, the chance of rain or snow in
percent. The weather screen shows the hours not yet over, then the days. The front page's masthead
shows today's weather and the next event; the sleep screen the rest of today, or tomorrow from 18:00, four at most.

### `/x4/stats.json`

```json
{"day":20769,"retention":0.87,"cards":342,"due":14,
 "books":{"books/meditations.epub":{"read_ms":8400000,"pages":212,"progress":0.41}},
 "play":{"sudoku":{"level":4,"played":31,"won":24}}}
```

- `retention`: mean FSRS retrievability of all non-new cards on the current day.
- `play`: per game, `{"level":4,"played":31,"won":24}` (daily games only).

## On the Mac

### Sent pieces: `X4_HOME/sent.jsonl`

Every piece ever sent, each once, written by `x4` alone:

```json
{"id":"r-2026-11-06-16","kind":"found","title":"…","url":"https://…","sent":"2026-11-06","section":"mind","issue":"2026-11-06","listed":["2026-11-06"],"asked":true}
```

`url` or `file` is how the issue named it, so its text can be had long after it left
(`x4 article <id>`). An issue that names a `url` already here under another id is refused:
the same page is sent again only under its old id. Links that differ only in tracking
parameters, `www.`, a trailing slash or a fragment are the same page. `section` and `issue`
(the date that first listed it) are the piece's; `listed` is every date that did, for a
weekly piece listed again. `asked` is set once the piece asked a `predict` or `link`. `test`
is a news piece's (S9): with the read log it says which of the editor's tests brings the
pieces that get read.

### Forecasts: `X4_HOME/forecasts.jsonl`

The ledger of every forecast, written by `x4` alone:

```json
{"id":"f-2026-11-12-1","text":"The budget passes its final vote.","by":"2026-12-16","made":"2026-11-12","happened":true,"resolved":"2026-12-17","source":"https://…","told":true}
```

A line is added when an issue with the forecast is sent (`made`). `x4 settle <id> yes|no|void
<source>` records what became of it: `happened` (true, false, or `"void"` when it cannot be
told), `resolved` and `source`. `told` is set once an issue with its outcome is sent (X59, not built yet). The
row picked is not copied in; it is read from the pulled `seen` log.

`x4 forecasts`, and `forecasts` in `x4 context`, list `due` (answered, past their day, not
settled), `tell` (settled yes or no, outcome not yet sent), `open`, and the counts `settled`,
`void` and `waiting` (not yet answered). Numbers on certainty wait for enough forecasts:
`brier` from 20 settled (the mean squared distance between the row, as a share, and the
outcome: 0 is best, 0.25 is always saying 50), and under `rows`, for each row with 10 or more
settled, how often it happened.

### Film: `X4_HOME/taste.md`, `X4_HOME/seen.md`

Yours. `taste.md` is what you like in film, in your words; Claude picks by it and `x4` does
not read it. `seen.md` is what you have watched, one line per film or series:

```
Anora (2024)
Black Doves (2024) season 1
```

The original title and the year it came out; a series watched in part names each season seen
on its own line, and a line without a season covers the whole series. A leading `- ` is
allowed; blank lines and `#` lines are skipped. A Film pick in the issue carries
`"film":{"title":"<original title>","year":2026,"season":2}` (season for a series only, and
only on kind `film`, which must have it). `x4 sync` and `x4 stage` refuse a pick that
`seen.md` lists, matching titles ignoring case and spacing, and a line of `seen.md` that is
not in this form. `film` stays on the Mac: the X4's line has no such field.

### The voice: `X4_HOME/voice.md`, `X4_HOME/voice/`

The guide Claude writes every Hungarian text by, drawn from the samples in `voice/samples/`
(nine Telex pieces, D27) and approved by you (D32); `voice/rewrites.md` is what it was approved
on. Yours: Claude reads it before writing and rereads its texts against it before `x4 sync`,
never edits it.

Under the heading `## x4 refuses`, each line is `` - `<text>` <what to write instead> ``.
`x4 sync` and `x4 stage` refuse an issue whose Hungarian texts contain one, ignoring case:
`stream.jsonl` (all but a resurfaced highlight, and names, keys and quotes),
`threads.jsonl`, `previously.jsonl`, the clues of `crosswords/hu.jsonl`, each piece's
`predict` prompt and options, `link` prompt and why and `forecast` text, and the Markdown
articles the issue names with `lang: hu`. A line's leading spaces are skipped, so a list's
`-` is not a dash. Headlines and leads are in the piece's language and are not checked.
Without the file nothing is refused.

### Sections: `X4_HOME/sections.md`

Yours to edit. One line per section: its name, a colon, what belongs
in it. `x4` reads the names and their order; Claude reads the rest. Blank lines and lines
starting with `#` are skipped.

```
Hungary: the day's three biggest stories in Hungarian politics and public life
World: the US, the EU, Palestine and China
Institutions: how rules shape behaviour; essays and research on institutions as mechanisms
```

A section's `id` is its name in lower case, accents dropped, spaces as `-`. `x4` refuses more
than 8 lines, a name twice, a name over 24 bytes. The line named Football is the Football
screen; without it there is no Football row.

### What `x4` reports

- `x4 status` gives the minutes of the last 7 days per section, in the front page's order:
  pieces from the read log by their section, section screens from the visits, `play` from the
  play log and Play's visits, `book` from the read log of the other books, then `kept` (saved
  links, in no section) and `read` (pieces of the old Read list, sent before sections). It
  counts what is still on the X4 too.
- `x4 context` has `day`, `issue` (today's date and whether its issue was sent), `sections`,
  and `pieces`: for each piece an issue listed in the last 14 days, newest first, its section,
  title, issue, whether it was seen (its section was visited while an issue listing it was on
  the X4), opened (the read log has a line for it), finished and kept, and its minutes.

## The data repo

`X4_HOME` is a checkout of one private GitHub repo. It is the writer's whole memory, so the
writer can run anywhere that has the repo: the Mac, or a scheduled run in the cloud. The repo is
the true copy. Whoever writes fetches first and sends after: `x4 pull` and `x4 context` bring
the copy up to the repo, `x4 sync` and `x4 stage` commit what was written and send it, and a
copy that is behind does not send. A home that is not a checkout is left alone.

| Path | Written by | What |
|---|---|---|
| everything under "On the Mac" and the host-written files above | the writer | The memory: `stream.jsonl`, `read.jsonl`, `decks/`, `threads.jsonl`, `previously.jsonl`, `crosswords/`, `glossary/`, `football.json`, `sent.jsonl`, `forecasts.jsonl`, `interests.md`, `fields.md`, `feeds.yaml`, `voice.md`, `voice/`, `taste.md`, `seen.md`, `notes/`, `tracks/`, `dossiers/`, and the cursors `.sync.json`, `.context.json`, `.feed.json`, `.rss.json`. |
| `pulled/`, `stats.json` | the writer | The X4's records once taken, from the cable or from `inbox/`. |
| `books/…` | the Mac | Each EPUB sent to the X4 (`x4 push book`), and each one the X4 has read in (`x4 sync` copies it off the card), at its book key without the leading `books/`: `/books/meditations.epub` is `books/meditations.epub`, `/Novel.epub` is `books/Novel.epub`. A recap reads the book here (K14). Read articles (`books/read/`) are not kept here. |
| `outbox/` | the writer (`x4 stage`) | The issue: `manifest.json` and its files at their device paths (`outbox/x4/stream.jsonl`). |
| `inbox/` | the X4 | What the X4 sent up, one new file per upload. |

Not in the repo (`.gitignore`): `net.json` (calendar addresses), `repo.json` and `serve.json`
(tokens), `football.key`, `cache/` and `library/`.

### `inbox/`

The X4 never changes a file in the repo. Each upload is a new file named by the X4's clock in
UTC, so names sort in the order they were sent:

| File | What |
|---|---|
| `inbox/<YYYYMMDDTHHMMSSZ>-<log>.jsonl` | The lines of one device-written log since the last upload. `<log>` is `highlights`, `reviews`, `seen`, `play`, `read-log`, `lookups`, `kept` or `visits`. |
| `inbox/<YYYYMMDDTHHMMSSZ>-stats.json` | `/x4/stats.json`, when it changed since the last upload. |
| `inbox/<YYYYMMDDTHHMMSSZ>-taken.json` | `{"issue":"<id>","files":["/x4/stream.jsonl",…]}`: the issue the X4 took, as `POST /taken` says it on the LAN. |

`x4 pull` and `x4 context` take the inbox in before anything else, in name order: log files
go into `pulled/` as a cable pull's would (a highlight's deletion removes it, a record seen
before is dropped), the newest `stats.json` replaces the one kept, a `taken` is noted in
`.sync.json`, and the inbox files are removed. What was taken in is sent to the repo at once,
so no other writer takes the same files in. The X4 adds to the inbox at any hour: a writer
whose copy is behind only by inbox files is put on top of them and sends; one that is behind
another writer does not. Git keeps every
version, so a record or an issue removed from the files is still in the repo's history.
