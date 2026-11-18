---
name: x4-sync
description: Write the next issue of the Xteink X4's newspaper and sync the device. Pull highlights, review, play and reading results; write cards; choose up to three original articles for each section in sections.md, with a short headline and two lines each; write bites, book puzzles, track lessons, match analyses, club dossiers, crosswords, glossaries and recaps; then push everything. Unplugged, stage the issue for the X4 to take over Wi-Fi. Use when the user says sync, plugs in the X4, or asks for a new issue, cards, articles, lessons or dossiers.
---

# x4 sync

The X4 is the user's newspaper and their book. Every sync you write one issue: for each
section, up to three articles worth their time, chosen by you and written by other people.
An issue is finite and it leaves: tomorrow's replaces it, and what the user did not read or
keep is gone. So nothing goes in to fill a slot. Everything goes through the `x4` CLI (or the
`x4` MCP tools, the same operations). The data contract is `docs/DATA.md`. The CLI validates every file before pushing, and its errors say
exactly what to fix.

## 1. Read the state

```bash
x4 status --json
```

When it finds no X4 on USB, this is an unplugged run, as the cloud run always is: skip
`x4 pull`, write everything below except glossaries (step 7) and next books (step 9), which
wait for the cable, and finish with `x4 stage` (step 10). `x4 context` takes in what the X4
sent up over Wi-Fi, so the records are as fresh as its last fetch.

When `context.issue` says today's issue was sent already (the afternoon run), skip step 4:
the pieces are fixed for the day. Everything else is written as usual.

```bash
x4 pull --json
```

```bash
x4 context --json
```

```bash
x4 feed --json
```

`context` has new highlights and questions, words looked up in the dictionary since the last
sync (`lookups`, each with its book's title), deleted highlights, card results (weak cards
first), books with progress, unseen bites and puzzles, book-puzzle results, play levels and
results, which articles were finished or left (`read`), `interests.md` and pending inbox
links. `feed` has the candidates: `news` (stories since the last sync, ranked by `k`, the
number of independent sources, with `topics` and, for the top stories, `primary` documents),
`follow` (new posts from the user's feeds, with `topics`), `inbox` (links they saved) and
`interests`. Both report the same new items until `x4 sync` succeeds; run them again freely.
`x4 feed --topic china --json` narrows to one topic. Work from these, the files in `$X4_HOME`
(default `~/x4`) and web search.

`settings.yaml`, `interests.md`, `sections.md`, `taste.md` and `seen.md` are the user's:
follow them, never edit them. `context.settings.language` is the writing language: everything
you write is in it unless this skill or a line in `sections.md` says otherwise. `voice.md`,
when there is one, is the voice of what you write in it: read it now, before writing
anything, and write by it.

## 2. Tracks: `~/x4/tracks/<slug>.md`

A track is something the user wants to get good at, listed under `## Tracks` in
`interests.md`. Each has a file you keep:
a syllabus of 20–40 lessons in a sensible order, each lesson one line, done ones checked with
the day they ran. Write the file the first time a track appears; revise it when results
show a gap.

Every sync, for each track, take the next 1–2 lessons:

- a **bite** per lesson (step 5), titled with the lesson, teaching one idea with one concrete
  example;
- 3–6 **cards** in the deck `track-<slug>`, from the lesson and from earlier lessons the
  results show are weak;
- a **book puzzle** or two built on it (`logic`, `order`, `odd`, `riddle`);
- now and then a piece that goes deeper (a classic essay, a paper, a chapter), in the section
  it belongs to (step 4).

For a track built on a document (a thesis, a paper, a book on the device), read the source
its line names first and ground every item in it: its argument, hypotheses, design,
measures, the literature it cites, its limits, and the questions a critic would ask. Mix
recall with reasoning: what would change if, which result would refute, which design choice
guards against what. Cards about a book on the device set `"book"` to its book key.

## 3. Cards: `~/x4/decks/<deck-id>.jsonl`

One deck per book (`deck-id` = a short slug of the title), one per track. Append new cards;
never change the `id` of an existing card. The device asks one due card of a deck at the end of
a chapter of its `book`; a deck without `book` (a track) after any Read article. One question,
so each card must be worth the stop.

For every new highlight, write one card, two when it holds two ideas. For every weak card (2+ lapses, or wrong twice in a
row), rewrite it in place with the same `id` from a different angle, and add one simpler card
for the idea it depends on. A card's `wrong` in `context.reviews.cards` lists the options
picked when it was missed (their index in `options`): that option is the misreading to write
the new angle against. A puzzle's `pick` in `context.puzzles.results` is the same. `context.deleted` lists highlights deleted on the device: delete
every card and stream item whose `highlight` is one of them (`x4 pull` already updated `notes/`).
A highlight or question with `item` was made on a bite or a puzzle: treat it like a book highlight,
attributed to its `book` (cards go in that book's deck), or to the item when `book` is `stream/<id>`.

- `cloze` for a precise statement in the user's own highlighted words. Blank the one term that
  carries the meaning. Options: the term plus 2–3 terms from the same book a careless reader
  could confuse with it.
- `choice` for distinctions and applications. The prompt is a situation or a question, never
  "Which of the following". Distractors are plausible misreadings, same length and grammar as
  the answer. Add `why` in one sentence.
- `explain` for the big ideas and every `question` highlight. Points: 2–4 short phrases.
- No `flip`: the reader grades it, and a card you grade yourself is answered without thinking.
  A definition, name or cause becomes a `cloze` or a `choice` between near misses.
  `context.decks.<id>.flip` lists the flip cards left: rewrite each in place, same `id`, as
  `choice` or `cloze`, ten a sync at most, the ones in `reviews.weak` first.

One idea per card, grounded in the book, track or highlight (`"highlight": "<id>"`). Short: the
front fits two lines. Plain text: no Markdown, labels or emoji.

## 4. The issue: `~/x4/read.jsonl` and `~/x4/articles/`

One issue a day. Read `~/x4/sections.md`: one line per section, in the front page's order,
its name and what belongs in it. For each section write up to three pieces, the best first:
the front page shows the first one the user has not read. Three when three are worth
reading, fewer when they are not, none when nothing is. A thin section is right; a filled one
is wrong.

A piece is an original article, whole, in its own language, linked by `url`:

```json
{"id":"r-2026-11-12-1","section":"news","kind":"news","url":"https://…","head":"Budget passes after all-night vote","lead":"The deficit target holds; two ministries lose a fifth."}
```

- `section`: the section's name in lower case, accents dropped, spaces as `-` (`x4` names
  the valid ones when you get it wrong).
- `head`: the headline for the front page, 30 characters at most, in the piece's language.
  It says what happened or what the piece shows, in the plainest words. Never a teaser, a
  question or a pun. The article keeps its own headline on the section's screen.
- `lead`: what the piece says, 60 characters at most, in the piece's language: the one
  sentence you would tell someone deciding whether to read it. It adds to the headline,
  never repeats it.
- `kind`: `news` (a news section), `found` (an article you found), `follow` (from the user's
  feeds), `field`, `analysis`, `dossier`, `translation`, `film`.

`x4` refuses a section with more than three pieces, a piece without `head` or `lead`, and
either one too long.

What each section takes, beyond its line in `sections.md`:

- **News sections** (a line that asks for the day's stories): the day's biggest stories in
  what the line names. With a news engine set up, they come ranked in `feed.news` by `k`,
  the number of independent sources; without one, rank the stories in `feed.follow` and on
  the web by how many independent outlets carry them. For each, the single best article on
  it that can be read whole without paying: the fullest account from the outlet that did the
  reporting, before a wire rewrite. When the story rests on a document (a law, a ruling, a
  statement) and the document is short and readable, the document itself may be the piece.
  Never two pieces on one story; never the same outlet three times in a section. A source
  with `"piece": false` (a paywalled outlet, or one the engine counts but never shows) counts
  in `k` and is never the piece; when a story has no other source, find the piece on the
  web or leave the story out.
- **A piece in a language the user does not read** (any but the writing language and
  English): a scholarly essay or good reporting in English by `url` when there is one. Else a
  digest you write, kind `translation`, at most 450 words: what it reports and argues, its
  facts and figures, a few short key lines translated and quoted, the original linked. Never
  a full translation: that republishes someone else's article. State media only as the
  record of an official position, named as such. Where accounts conflict, say who claims
  what.
- **Every other section**: articles you find on the internet, fresh every sync, that fit its
  line: essays, research written for readers, practitioner pieces with a worked case. Prefer
  the piece with an argument or a finding over the piece with an opinion. No paywalled
  pages, never the same site twice in an issue. The best 2 to 4 new posts from `feed.follow`
  go where they belong.
- **Field**, two pieces, every calendar week: the field of the week, which is how the user
  reads outside their topics. `~/x4/fields.md` is the user's list, one field a line, in
  their order; never edit it except to mark a line used. When this ISO week has no field
  yet, take the first line without a week and append it: `- Immunology (2026-w47)`. A week
  that passed is over whether it was read or not. Find, fresh:
  - a **primer**: one article that explains the field's central idea to an outsider, by
    someone who works in it, 10 to 15 minutes;
  - a **primary text**: the field in its own voice, a founding paper, a classic chapter or
    lecture, a ruling, free to read online in full and about 30 minutes at most. When the
    founding work is long, take a shorter text by the same author, not a summary of it.
  Both are kind `field`. List both again, under the same ids, in every issue of the week:
  a piece listed again stays on the X4 with what was read of it.
- **Football**: an analysis of each followed club's match (below) in the issue after it,
  once; the club dossiers the Football line asks for (below) once a week, listed again
  through the week under the same ids. When that makes more than three, the analyses go
  first and a dossier left out comes back the next day. Beside them, when a slot is free, an article you find on how
  a team plays or how a club is run. Results, fixtures and tables come from `x4 football`,
  which `x4 sync` and `x4 stage` run; never write them yourself.
- **Film**, up to three picks a week, the week turning on Thursday with the premieres.
  Nothing goes in to fill it: three when three fit `taste.md`, fewer, or none. The
  candidates are what arrived since the last Thursday in the cinemas and on the services the
  Film line names, from the sources it names; without sources, from the week's premiere
  listings for the user's country that you find on the web.
  Choose by `taste.md`; never what `seen.md` lists (`x4` refuses it). Each pick is a piece you
  write in `~/x4/articles/<id>.md`, about 150 words, in the voice: what it is (who made it,
  who is in it, what it is about, no spoilers past the premise), why it fits `taste.md`,
  naming what in it does, and where and from when. Front matter as for a digest, with
  `source: "x4 · film"` and the pages you used under `sources`. The Read list line is kind
  `film`, section `film`, `head` the title as it runs where the user lives, `lead` where and
  from when ("In cinemas from 13 November", "Netflix, season 3"), and `film`: the original
  title, the year, and `season` for a series. List the week's picks again, under the same
  ids, in every issue until the next Thursday.

  ```json
  {"id":"r-2026-11-13-17","section":"film","kind":"film","file":"articles/r-2026-11-13-17.md","head":"…","lead":"In cinemas from 12 November","film":{"title":"<original title>","year":2026}}
  ```

### What a piece may carry

Most pieces carry nothing. A piece may ask one question before it is read, and after it one
thing more: a link or a forecast, never both (`x4` refuses that). A due card follows only a
piece that has neither.

- `predict`, before the article, when it has a main finding or claim the reader can hold a
  view on first: `prompt` asks which way it goes ("Does it raise or lower…?"), two to four
  `options` that someone who has not read it could each believe, `answer`, and `quote`, the
  article's own sentence that settles it, copied from `x4 article <read id>` (`x4` refuses a
  quote that is not there). The prompt and options are in the writing language; the quote
  stays in the article's language. Never a detail or a number for its own sake, and no predict for an
  article without a claim (a profile, a chronicle). Vary where the right option stands. The
  X4 asks it when the piece is opened and shows the answer only once the piece was read;
  `context.predict` has how the guesses went.
- `link`, after it, when something the user read earlier truly stands in a relation to it,
  never to fill the slot: `prompt` names the relation ("Which one contradicts this?", "Which
  one describes the same mechanism?"), three `options` from
  `context.linkable` (each `{"text", "ref"}`: an article or book by its title, a highlight
  by its own words cut short), `answer`, and `why` in a sentence or two that names both
  sides. Reach for the oldest thing that fits, and across fields before within one. The two
  others must be things someone could take for the answer by topic alone. `x4` refuses a
  `ref` that is not in `linkable`. `context.link` has how they went.
- `forecast`, after it, on a piece of a running story, two an issue at most:
  `{"id":"f-<date>-<n>","text":"…","by":"YYYY-MM-DD"}`. `text` is a plain
  statement about what happens next (a vote, a ruling, a deadline, a number that will be
  published) that will be clearly true or false on `by`, and that you know a source will
  settle; `by` within six weeks, most within two (`x4` refuses later). Nothing already
  decided, nothing that turns on a definition. The user says how likely on five rows.
- `thread`, on a piece of a story that ran before: `"thread":"t-<slug>"`, the id of a line
  in `~/x4/threads.jsonl`, `{"id","title","body","since","updated"}`. A story becomes a
  thread the second time it is among an issue's pieces (compare with `context.read` and the
  threads already in the file), not on its first day. `body` is the story so far up to but
  not including today's news: who, what is at stake, what happened in order, in plain
  words, one paragraph, 600 bytes at most (`x4` refuses more). The X4 shows it before
  the piece, once each time its text changed, so change the text only when the story moved.
  Drop a thread that had no news for two weeks; 32 at most.

```json
{"id":"r-2026-11-12-1","section":"news","kind":"news","url":"https://…","head":"…","lead":"…",
 "thread":"t-budget-2027",
 "predict":{"prompt":"…","options":["…","…","…"],"answer":1,"quote":"…"},
 "forecast":{"id":"f-2026-11-12-1","text":"…","by":"2026-12-16"}}
```

A link the user saved (`feed.inbox`) is no section's piece. It arrives already kept:

```json
{"id":"r-2026-11-12-20","kind":"link","url":"https://…","kept":true}
```

Articles and digests you write are Markdown files in `~/x4/articles/<id>.md` with front
matter, listed by `file` in place of `url`:

```markdown
---
title: "…"
source: "x4 · digest"
lang: hu
date: 2026-11-12
translated_from: {url: "https://…", lang: zh, source: "澎湃新闻"}
sources:
  - {title: "…", url: "https://…"}
---
```

### Match analysis: `~/x4/articles/<id>.md`

The followed clubs are the ones in `~/x4/football.yaml`; `~/x4/football.json`, which
`x4 football` writes, has their results. For each match one of them played since the last
issue, write one piece, in the language the Football line asks for (else the writing
language), about 400 words (`x4` refuses more than 450):
what decided it and how each side played, from the match reports, the managers' words and
the numbers others published. Numbers only as someone published them: Opta Analyst,
the clubs' and leagues' own match centres, the reports you read; each with its source in the
sentence ("11 shots to 4, by the Premier League's match centre"). Never a number of your own,
never one from FotMob or another site that forbids reuse, and no expected goals unless a
source you cite gives them. When nobody published numbers, say how the match went without
them. Every claim has its owner, as in a dossier. Kind `analysis`, `source: "x4 · analysis"`,
every report under `sources`, in the issue after the match and no other:

```json
{"id":"r-2026-11-09-15","section":"football","kind":"analysis","file":"articles/r-2026-11-09-15.md","head":"Brighton 2–1 Brentford","lead":"Won on set pieces after Brentford pressed them out of the first half."}
```

### Club dossiers: `~/x4/dossiers/<club>.md`

A standing file per club the Football line asks a dossier for (`brighton.md`), that you keep: ownership, the
data and recruitment model, sell-on (fees in and out, who was bought cheap and sold dear),
the coaching pipeline, finances (the latest accounts, wages, owner funding). In the
analyses' language, from
sources you find on the web this week: club accounts and statements, the Athletic, the FT,
Swiss Ramble, Companies House filings, interviews with the people involved. Write it the first
time; after that, add what is new under a `## <yyyy-mm-dd>` heading at the top and fold older
additions into the sections, so the file stays at most 750 words (`x4 sync` refuses more).
Every claim sourced; numbers with their year. Front matter as for any article
(`source: "x4 · dossier"`, `date` the day you updated it), then a piece in Football, whose
`lead` names what is new this week:

```json
{"id":"r-2026-11-09-14","section":"football","kind":"dossier","file":"dossiers/brighton.md","head":"Brighton: how the club works","lead":"Ownership, recruitment, sell-on, coaching, money."}
```

Results, fixtures and tables come from `x4 football`, which `x4 sync` and `x4 stage` run;
never write them yourself.

`x4` refuses a page that already went out under another id, and names that id. Drop it, or,
when the user should get it again, list it under the old id.

An issue's pieces are fixed for its day. When `x4 sync` or `x4 stage` says today's issue
went out already, it sent that issue again and left your new pieces out: say so, and write
the next issue tomorrow. Saved links still arrive.

Learn from `context.read` and `context.pieces` (each piece of the last two weeks: seen,
opened, finished, kept, minutes) and from the week's minutes per section in `x4 status`:
sections, topics and outlets the user passes over or leaves unfinished get fewer pieces,
finished ones the better slots. A piece not `seen` was never in front of them; it says
nothing about the piece. Ids: `r-<yyyy-mm-dd>-<n>`.

## 5. Bites and book puzzles: `~/x4/stream.jsonl`

Replace the file; keep the unseen items from `context` that are still good.

- **bites**: the X4 shows one a day, on the day's first opening of a book, so keep five
  unseen and no more. One screen, 70 to 110 words. From the user's current books
  (one idea made sharper: an example, a consequence, a link to another book, the strongest
  objection) and the tracks' lessons. Title is the idea in 3 to 6 words. Every `question`
  highlight gets a bite that answers it, with `highlight` set to the question's id; those go
  first.
- **highlight**: now and then in place of a bite, an old highlight that connects to what
  they read now.
- **book puzzles**, played from Play: `riddle`, `order` (a timeline or an argument's
  steps), `odd` (odd one out) or `logic` (a reasoning puzzle), from the books and the
  tracks. Hard enough to need thought, fair from the material alone. `why` in one sentence.
  Keep six unseen.

No other kind is written: no brief, article, game, letter, forecast, outcome or test, and
no `end`. Ids: `s-<yyyy-mm-dd>-<n>`.

Forecasts get settled first of all: for each one in `context.forecasts.due`, find what
happened from a source and run `x4 settle <id> yes|no|void <url>` (void when it cannot be
told; do not stretch a yes or no). Telling the user the outcome on the X4 waits; until then
say settled ones in your report.

## 6. Hungarian crosswords: `~/x4/crosswords/hu.jsonl`

Only when the writing language is `hu`. English crosswords come clued from Wiktionary with
`x4 words install en`; other languages have none yet.

7x7, hard, made of rare words from the user's reading. One puzzle a day; keep at least three unplayed.

**Seed words**, best first, into `~/x4/crosswords/seeds.txt`, one per line as
`word<TAB>title of the book or article`:

1. Words from new highlights and from `context.lookups`: the rare, precise ones (a lookup is a
   word the user did not know). Write the dictionary form, lowercase: `elmélkedés`, not
   `Elmélkedéseiben`.
2. Then rare words from the books and articles they are reading now (`context.books`, the Read
   list): names of things, terms, words a well-read person should own.

Hungarian words of 3 to 7 letters only (longer ones are skipped); no names, no slurs. About
three seeds per puzzle; the title is the book's or article's, at most 48 bytes, the same
spelling for every word from it.

```bash
x4 words grids hu --count 3 --words-file ~/x4/crosswords/seeds.txt --json
```

It writes the grids, with their `answer`s, to `crosswords/hu.grids.jsonl`: seed words first, the
rest rare words. A seed's entries have `"seed": true` and its `src`; `placed`, `unplaced` and
`skipped` say what happened to each seed. Keep the unplaced ones for the next sync.

Give every entry a `clue`: a short, hard definition, the way a good Hungarian rejtvény does
it for an expert: precise, a little oblique, never a synonym that gives it away ("Bölcseleti
nyugalom", "Holtág, más szóval"). Never English, never the answer or a word starting with it,
at most three lines on the device. Keep `src` on the words that came from reading and nothing
else; at most six titles per puzzle. Append the clued lines to `hu.jsonl`; `x4 sync` checks
every clue and source and pushes the file. Once solved, the device shows each title with its
words.

## 7. Glossaries: `~/x4/glossary/<book-key with / as __>.json`

For a book in `context` without a glossary, write 30–150 terms a reader of that book might look
up: names, coined terms, foreign and technical words, each defined in 1–2 sentences as the book
uses it. Skip common words; the dictionaries cover those.

## 8. Recaps: `~/x4/previously.jsonl`

One line per book in progress (`context.books` with a progress between 0 and 1, not a Read
article) whose `previously` is missing or differs from its `progress`:
`{"book": "<book key>", "progress": <its progress>, "text": "..."}`. Replace that book's line;
drop lines of books no longer in `context.books`. Read where they stopped:

```bash
x4 excerpt "<book key>" --json
```

The text is the last 2500 words before their place. Write the recap in one paragraph, at most 65 words: what the book has been doing lately and where they stopped, so
they can pick up the next page cold. Plain words, no symbols the font may lack (≽, ∧), no
spoilers past `progress`. The X4 shows it before the page when the book opens days after
they last read it, while the book is still at that progress.

## 9. Next books: `~/x4/next.jsonl`

For each book in `context.books` (not a Read article) past 0.75 progress with no `next`, pick
up to three books to read after it:

```bash
x4 shelf --json
```

It lists the unread books: on the card by key, and in `~/x4/library` (the user's own EPUBs) as
`library/<file>`. Pick by `interests.md`, the book itself and the highlights, best first; a
track's material only if the track needs it. Write `{"after": "<book key>", "books": ["<key>",
...]}`, one line per book. Sync sends a library pick as a sample of its first chapters, and the
whole book at the sync after they read in it. Drop a line once its book is finished. Never
fetch books from anywhere else; when the shelf is empty, write no line and say so.

## 10. Push and report

When there is a `voice.md`, first reread everything you wrote this sync in the writing
language against it: bites, puzzles' `why`, threads, recaps, each piece's question, link and
forecast, digests and clues. Fix what breaks a rule. `x4 sync` and `x4 stage` refuse what its
`x4 refuses` list names, and say where and what to write instead.

```bash
x4 sync --json
```

Then tell the user in three lines at most: highlights in, cards added and rewritten, the
issue by section (how many pieces each got, and which sections got none), bites and puzzles
queued, track lessons taken, recaps written, next books picked (and whole books sent),
whether dossiers went out. Nothing else.

Unplugged, instead:

```bash
x4 stage --json
```

It writes the issue and its articles, the bites and puzzles, cards, threads, recaps,
crosswords and `football.json` to `~/x4/outbox/`, sends them to the data repo, and refuses
invalid files the way `sync` does. Tell the user in two lines at most: the issue by section,
and that the X4 takes it on its next fetch.

## Taste

A good paper is edited: what is left out makes it. Choose like an editor who knows this one
reader, and write like a sharp friend who read the same book. A headline says the thing;
two lines say why it is worth ten minutes.
