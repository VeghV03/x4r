# x4 USB protocol, v2

The Mac talks to the X4 over its native USB-Serial/JTAG port (`/dev/cu.usbmodem*`, any baud).
The firmware owns one parser; the `x4` CLI owns one client. This file is the contract between them.

## Framing

Everything is lines of UTF-8 ending in `\n`, except binary payloads, whose length is always
announced in the line before them.

Host to device:

```
X4 <id> <command> <json-object>\n
```

Device to host:

```
X4 <id> ok <json-object>\n
X4 <id> err {"code":"<code>","message":"<text>"}\n
X4 <id> ready {...}\n          device is ready for a binary upload
X4 <id> ack {"n":<bytes so far>}\n
X4 <id> data {"size":N,"crc32":C}\n   followed by exactly N raw bytes
X4 0 event {"type":"..."}\n    unsolicited, id is always 0
```

- `id` is a host-chosen decimal number, echoed back. One command is in flight at a time.
- The host ignores every line that does not start with `X4 ` (log lines, boot noise).
- `crc32` is the standard CRC-32 (IEEE, as zlib `crc32`), unsigned decimal.
- Error codes: `bad_request`, `not_found`, `exists`, `io`, `no_space`, `crc`, `busy`, `too_large`, `unknown_command`.

## Session

- The device does not auto-sleep on USB power, so the host can always reach it; the timeout
  counts from unplugging.
- `hello` opens a session. While a session is open the device stays at full CPU speed and writes
  no log lines to the port.
- The session ends on `bye`, on USB disconnect, or after 60 s with no command.
- When the session ends, the device reloads anything the host changed (decks, stream, books,
  fonts, dictionaries) and redraws the current screen if it shows that data.
- On screen, an open session shows nothing extra. The device is just awake.

## Commands

| Command | Args | Reply |
|---|---|---|
| `hello` | `{"time": <unix s>, "tz": <offset min>}` | `{"proto":2,"fw":"<ver>","board":"x4","slot":"ota_0","mac":"<aa:bb:..>","battery":80,"battery_mv":3912,"charging":true,"sd_free":N,"sd_total":N,"heap_free":N,"heap_min":N,"heap_block":N,"day":20769,"issue":"2026-11-12","retention":0.87}` (`day`: DATA.md "Time: the day"; `issue`: the date of the issue on the X4, absent without one; `retention` absent before any review; heap: internal RAM free, lowest since boot, largest block, bytes; `slot`: the running app slot; `mac`: the chip's base MAC, which names its backup) |
| `time` | `{"time":…, "tz":…}` | `{}`. Sets the wall clock and the trusted-time floor. |
| `ls` | `{"path":"/x4"}` | `{"entries":[{"name":"a.json","dir":false,"size":120}]}` |
| `stat` | `{"path":…}` | `{"dir":false,"size":N}` or `not_found` |
| `get` | `{"path":…, "offset":0, "length":4096}` | `data {"size":N,"crc32":C,"total":T}` + bytes, then `ok {}`; see below |
| `put` | `{"path":…, "size":N, "crc32":C}` | `ready {"chunk":4096}`, then see below |
| `rm` | `{"path":…, "recursive":false}` | `{}` |
| `mkdir` | `{"path":…}` | `{}` (creates parents, ok if it exists) |
| `mv` | `{"from":…, "to":…}` | `{}` (replaces the target) |
| `screenshot` | `{}` | `data` line with `{"size":48000,"crc32":C,"width":800,"height":480,"bpp":1,"rotate":90}` (`rotate`: degrees clockwise that turn the frame upright for the current reading orientation), then the framebuffer bytes as stored, then `ok {}` |
| `press` | `{"button":"confirm", "long":false}` | `{}` after the press is handled. Buttons: `back confirm left right up down power` |
| `screen` | `{}` | `{"activity":"<name>"}`, on a question screen also `"step":{"phase":"prompt","done":0,"total":1,"kind":"choice","id":"meditations/c2","selected":0}` |
| `heap` | `{}` | `{"free":N,"min":N,"block":N,"low":[{"at":"net:weather","min":13788}]}`: free heap now, its lowest since power-on, the largest free block, and the last 8 new lows with the stage that had just run |
| `lag` | `{}` | `{"presses":[{"activity","idle","up","panel","done","last","updates","mode","low_clock"}],"marks":[{"tag","ms"}]}`, the last 16 presses, oldest first, and the marks of the latest |
| `net` | `{"wifi":{"ssid":…,"password":…}}`, `{"forget":"<ssid>"}`, `{"fetch":true}` or `{}` | `{"net":{"networks":["Home"],"fetching":false,"fetched":<unix s>,"error":"<last failure>"}}`. Saves or forgets a Wi-Fi network, or starts a fetch now (see below); every form reports. `hello` carries the same `net` object. |
| `flash` | `{"size":N, "sha256":"<hex>"}` | `ready {"chunk":4096}`, upload as `put`, then `ok {"slot":"ota_1"}` and the device reboots into the new slot |
| `reboot` | `{}` | `ok {}`, then reboot |
| `bye` | `{}` | `ok {}` |

### Uploads (`put`, `flash`)

1. Host sends the command. Device opens `<path>.part` (or the inactive OTA slot) and replies `ready {"chunk":4096}`.
2. Host sends raw bytes in chunks of at most `chunk` bytes. After each chunk the device replies `ack {"n":<total so far>}`. The host waits for each `ack` before sending the next chunk.
3. After the last chunk the device checks size and CRC (or SHA-256 for `flash`), renames `.part` to the final path atomically, and replies `ok {}`. On a mismatch it deletes the `.part` file and replies `err {"code":"crc"}`.
4. If no bytes arrive for 5 s mid-upload, the device aborts and deletes the `.part` file.

### Downloads (`get`)

The port has no flow control: a long burst the host does not keep up with loses bytes on the
way (a whole 3.9 MB epub in one `get` came back thousands of bytes short). So the host reads a
file in ranges of at most 4096 bytes, one `get` at a time, the same size as an upload chunk:

1. Host sends `get` with `offset` and `length` (at most 4096). The device replies
   `data {"size":N,"crc32":C,"total":T}`, exactly N bytes, then `ok {}`. `total` is the file's
   size; N is shorter than `length` only at the end of the file.
2. The host checks the range's CRC as soon as its N bytes are in, before it waits for `ok`, so
   lost bytes (which pull the start of the `ok` line into the range) fail at once.
3. On a CRC mismatch, a timeout (3 s for the line or the bytes) or a garbled line, the host
   reads until the port is quiet for 0.2 s, then asks for the same range again, up to 5 times.
4. The host stops at `total`. A v1 device sends no `total`; the host then stops at the first
   range that comes back short, which may be empty.

A `get` without `length` still answers with the rest of the file in one reply; v2 hosts never
send one. A `screenshot` comes in one reply and is asked again the same way.

### Edge cases

- `put` into a folder that does not exist: `err not_found`. The host runs `mkdir` first.
- `put` with `size` 0: the device replies `ready`, then `ok {}` at once, with no acks.
- An upload that stalls for 5 s: `err io`, and the `.part` file is deleted.
- `rm` on a folder that is not empty without `"recursive":true`: `err bad_request`.
- After `flash` replies `ok`, the device reboots; the host does not send `bye`.
- The host holds DTR and RTS low when it opens the port, so opening never resets the chip.

### Firmware updates

`flash` writes the inactive OTA slot and switches to it. The bootloader has app rollback
enabled: the new image keeps the slot once it has painted the home screen or a reader page and
then either a host has said `hello` or it has run for 2 minutes; going to sleep after that paint
also keeps it. A reset before then boots the previous slot. `x4 flash` checks the image is an
x4r build, waits for `hello` from the slot it wrote, and resets an image that never answers,
which rolls it back. `esptool` stays the recovery path: it writes app0 and erases otadata.

## Examples

```
> X4 1 hello {"time":1793714400,"tz":60}
< X4 1 ok {"proto":2,"fw":"x4r-0.1.0-5e81c0cc","board":"x4","slot":"ota_0","mac":"38:44:be:98:7a:94","battery":80,"battery_mv":3912,"charging":true,"sd_free":7516192768,"sd_total":7948206080,"day":20769,"issue":"2026-11-12"}
> X4 2 get {"path":"/x4/highlights.jsonl","offset":0,"length":4096}
< X4 2 data {"size":4096,"crc32":2215402233,"total":5120}
< <4096 bytes>
< X4 2 ok {}
> X4 3 get {"path":"/x4/highlights.jsonl","offset":4096,"length":4096}
< X4 3 data {"size":1024,"crc32":901372466,"total":5120}
< <1024 bytes>
< X4 3 ok {}
> X4 4 put {"path":"/x4/decks/stoicism.jsonl","size":9000,"crc32":3735928559}
< X4 4 ready {"chunk":4096}
> <4096 bytes>
< X4 4 ack {"n":4096}
> <4096 bytes>
< X4 4 ack {"n":8192}
> <808 bytes>
< X4 4 ack {"n":9000}
< X4 4 ok {}
> X4 5 bye {}
< X4 5 ok {}
```

## Device notes (x4r firmware)

Details the tables above leave open, as the firmware implements them.

- Any valid command opens the session; `hello` is still the expected first command. The
  session also ends when the host stops reading for 2 s. Disconnect means USB power loss;
  DTR and RTS are ignored.
- Request lines are at most 512 bytes; a longer line gets `too_large`. Paths are absolute,
  at most 255 bytes, with no empty (`//`, trailing `/`), `.` or `..` component. `time` must
  fall in 2025–2099; `tz` is minutes east of UTC, between -1440 and 1440. In `hello` both are
  optional. A `time` without `tz` keeps the offset the host sent last.
- Uploads: the device acks when the byte count reaches a multiple of `chunk` or the announced
  size, so every chunk except the last must be exactly `chunk` bytes. If a write fails, the
  device reads the rest of that chunk and sends `err {"code":"io"}` instead of its ack. If
  the finished `.part` cannot be closed, it is removed and the reply is `err io`.
  `put` needs the folder to exist (`not_found`), rejects a folder path (`bad_request`) and
  checks free space first (`no_space`).
- `get`: `offset` past the end is `bad_request`; a `length` past the end is cut to the end.
  The device reads the range twice (CRC, then bytes). If the second read fails, it pads the
  announced size with zeros and sends `err {"code":"io"}` instead of `ok`. For 50 ms after
  a `get` the main loop skips its 10 ms idle delay, so the next range is served at once.
- `ls` gives folders `"size":0`. `mkdir` on an existing file is `exists`. `rm` on a non-empty
  folder without `recursive` is `bad_request`. `mv` onto a free name is one FAT rename. FAT
  names ignore case, so a `mv` that changes only the case is a rename of the same file or
  folder, never a replace. Replacing a file parks the
  target as `<to>.x4mv`, renames the source in, then removes the parked file; on failure the
  target is put back. A folder can be moved, but never replaces anything (`exists`) and
  never moves into itself (`bad_request`).
- Files the host changes under `/x4` are re-read the next time the device needs them, even
  mid-session, and still after a reboot or a power cut. When the session ends, the front page
  and the Library are drawn again from what changed.
- `screenshot`: the panel's native frame, 800 × 480, row-major, 100 bytes per row, most
  significant bit = leftmost pixel, 1 = white, 0 = black. Night mode is applied at output,
  so the buffer is never inverted. Only the black/white plane is sent, not grey levels.
  The frame is in panel orientation; to show it upright, rotate it 90° clockwise for
  Portrait, 180° for Landscape Clockwise, 90° counter-clockwise for Portrait Inverted, and
  not at all for Landscape Counter-Clockwise.
- `press` names physical buttons; the user's button remapping still applies. A short press
  holds 80 ms, a long press 1200 ms. The reply comes once the release has been handled and
  the screen has finished redrawing, or after 15 s (also when someone holds that button
  down). Another command during a press gets `busy`. A long `power` press puts the device to
  sleep and gets no reply.
- `screen` returns the firmware's activity name: `Home` (the front page), `Section`,
  `Football`, `EpubReader`, `Questions` and so on. `Questions` is every step screen: the cards
  after a chapter, a piece's question and what follows it, a bite, a book puzzle. It adds
  `step`: `phase` (`prompt`, `revealed`, `answered`, `empty`); after a chapter, `done` and
  `total`, the cards asked and how many it asks; and, unless empty, the step's `kind` and `id`
  (a stream item's id, a piece's Read id, or a card key). `selected` is the row a key would act
  on; `picked` the option chosen, once answered; `holding` is true while an `order` row is
  picked up. The text is the host's to look up by `id`.
- `lag` times each button press to the panel update it causes, in ms from button-down: `up`
  the release (0 while held), `panel` the update starting, `done` it returning, `last` the last update of the press returning
  (an update starting within 1.5 s of the one before belongs to the same press, as the gray pass
  does) and `updates` how many there were. `idle` is the
  time since the previous input, `mode` the update (`fast`, `half`, `full`, `gray`),
  `low_clock` whether the CPU was at its idle clock. `marks` are named points in the latest
  press's work, in ms from its button-down (up to 16): `exit` and `exited` around the old screen's
  teardown, `entered` after the new screen's setup. After a restart or wake the first entry is
  `wake`, timed from the app starting (the bootloader before it is not counted). Kept in RAM; a
  restart or sleep clears it.
- `net`: at most 8 networks, SSID 1–32 bytes, password at most 63 (empty for an open network),
  kept in `/.crosspoint/wifi.json` with the password obfuscated by the chip's MAC. The device
  goes online on its own: once it has rested on the front page for 2 s (no press), at most once an hour
  after a good fetch (every 10 minutes after a failed one), and before the sleep screen when the
  agenda is over 30 minutes old (at most 25 s). A fetch joins the network that answered last
  first, sets the clock from NTP (`pool.ntp.org`, `time.google.com`; the clock then counts as
  the host's would, so the date shows and the day is the date), fetches the forecast and
  each calendar in `/x4/net.json` over TLS checked against ESP-IDF's certificate bundle, writes
  `/x4/agenda.json`, and turns the radio off. Leaving the front page stops a fetch (waiting at most 4 s);
  what finished is kept. `error` names the first step that failed in the last fetch:
  `wifi: no network`, `time: no answer`, `weather: <status>`, `calendar <n>: <status>` or
  `calendar <n>: not iCal`, where a negative status means no HTTP answer. `{"fetch":true}`
  without a saved network is `bad_request`.
- `flash` writes the image raw into the inactive slot and points otadata at it, like the SD
  card update (`esp_ota_end`'s image check rejects valid images on X4 silicon). The SHA-256
  covers the received bytes. A first byte other than `0xE9` is `bad_request`, an image larger
  than the slot is `too_large`. While the running image is still on probation (it has not
  reached Home or a reader page since it was flashed) `flash` is `busy`: the other slot holds
  the image a rollback returns to. `flash` and `reboot` restart the device 100 ms after `ok`.

## LAN issue

Not USB: plain HTTP on the home network between `x4 serve` on the Mac and the X4's fetch. The
X4 finds the Mac in `/x4/net.json` (`mac.url`, `mac.token`) and sends
`Authorization: Bearer <token>` with every request; anything else gets 401. The Mac serves
`~/x4/outbox/` only, and only on its LAN address.

| Request | Answer |
|---|---|
| `GET /issue` | `200` and the manifest below, or `204` when nothing is staged. |
| `GET /file?path=<path>` | `200` and the file's bytes, for a path the manifest lists. |
| `POST /taken` | Body `{"issue":"<id>","files":["/x4/stream.jsonl",…]}`; `204`. The Mac records what the X4 took, so the next `x4 sync` does not push it again. |

```json
{"issue":"2026-11-09T06:41:12","files":[{"path":"/x4/stream.jsonl","size":4210,"crc32":305419896},
 {"path":"/books/read/r-2026-11-09-1.epub","size":48211,"crc32":1203948123}],"prune":["/books/read"]}
```

- `path`: only `/x4/stream.jsonl`, `/x4/read.jsonl`, `/x4/football.json`, `/x4/threads.jsonl`,
  `/x4/previously.jsonl`, `/x4/decks/<deck-id>.jsonl`, `/x4/crosswords/<lang>.jsonl` and
  `/books/read/<id>.epub`. At most 48 files, each at most 4 MB, 16 MB in all; a manifest that
  breaks a rule is refused whole.
- `prune`: folders whose files the manifest does not list are removed once the issue is in:
  `/x4/decks` (its `.jsonl` files; a deck removed retires its cards). `/books/read` is
  accepted and removes nothing: the front page removes the articles that neither the issue
  nor a kept piece needs when it takes the issue (DATA.md, Kept).
- The X4 skips an issue it already took and a file whose `crc32` matches what it took last
  time for that path. It writes each file to `<path>.lan`, checks size and CRC, and only when
  all arrived replaces the files, prunes, sends `POST /taken`, and reloads as after a USB
  session that wrote files. A failure leaves the old files in place; the next fetch tries
  again. A fetch that is stopped keeps the `.lan` files that arrived whole, and the next one
  downloads only the rest.
- Connecting to the Mac waits 3 s, so the X4 away from home loses little. Errors read
  `mac: <status>` in `net.error`.


## Repo issue

Both ways are built and have run on the X4.

Not USB: HTTPS between the X4 and GitHub's API, `https://api.github.com`, for the repo named
in `/x4/net.json` (`repo.name`, `repo.token`). Every request carries
`Authorization: Bearer <token>` and `User-Agent: x4r`. The repo's layout is "The data repo" in
[DATA.md](DATA.md). With `repo` set the X4 does not ask the Mac on the LAN.

| Request | Answer |
|---|---|
| `GET /repos/<name>/contents/outbox/manifest.json` | `200` and the manifest, or `404` when nothing is staged. |
| `GET /repos/<name>/contents/outbox<path>` | `200` and the file's bytes, for a path the manifest lists. |
| `PUT /repos/<name>/contents/inbox/<file>` | Body `{"message":"x4","content":"<the file in base64>"}`; `201`. On `422` the X4 asks for the file: if it is there at the same size, it counts as sent. |

A `GET` asks for the bytes themselves with `Accept: application/vnd.github.raw+json`.

**Down.** The manifest and its rules are the LAN's, at most 8 KB (48 files come to about
4 KB). The X4 asks the repo last in a fetch, after the weather and the calendars, so the agenda
never waits behind an issue's files. An issue already taken is skipped, a file whose `crc32`
matches what was taken last is skipped, each file goes to `<path>.lan` and all are swapped in
together or not at all. The manifest has a connection of its own, closed before it is parsed; the files then share
one, since opening a connection to GitHub takes the X4 about 3 s. A stopped fetch keeps the
files that arrived, and the next fetch goes on from there. What the X4 took goes
up as `inbox/<time>-taken.json`, with the records.

**Up.** At the end of a fetch, and only in one that set the clock, the X4 sends what it has,
each as a new file (names in DATA.md):

- When a fetch starts, a log with lines in it is renamed to `<log>.pulling`, unless one is
  still waiting, so records written during the fetch start a new file. That file is sent. The
  name it goes up under is kept in `/x4/state/up.json` with the file's size until GitHub has
  answered, so a retry sends the same name and never a second copy, and a file that is not the
  same one any more gets a new name. Once GitHub has it the `.pulling` file is removed.
- At most 256 KB a file. A larger log waits for the cable.
- `stats.json` is copied when the fetch starts and goes up when it differs from the one that
  went last.
- After an issue is taken from the repo, what was taken goes up the same way.
- The uploads share one connection. The first failure ends them for this fetch.
- A cable pull takes a waiting `.pulling` file as it always did. If its upload had arrived and
  only the answer was lost, the lines come in twice, and `x4` keeps each record once.

Errors read `repo: <what> <status>` (the manifest or a file) and `up: <what> <status>` in
`net.error`; a negative status means no HTTP answer. The next fetch tries again.

Checked on 11 November 2026: the `Accept` header above returns the file's bytes, a repo with
nothing in it answers `404`, and a `PUT` to a file that is already there answers `422`.
