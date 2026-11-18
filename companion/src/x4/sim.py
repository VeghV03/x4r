"""A fake X4 that speaks docs/PROTOCOL.md over a pty, with a local folder as its SD card.

`X4_PORT=sim:<dir>` runs one in-process; `x4 sim <dir>` serves one until Ctrl-C.
`x4 sim --lossy` loses bytes on the way out the way the real port does.
`x4 sim --play` plays the paper: presses go to `x4play`, the firmware's own issue reader and key
logic built for the Mac, so `press` and `screen` do what they do on the X4, and `screenshot` is
drawn by `x4paper`, the firmware's own drawing. `Sim(..., daily_set=True)` plays the set that
came before the paper.
"""

import hashlib
import json
import os
import random
import re
import select
import shutil
import subprocess
import tempfile
import threading
import time
import tty
import zlib
from pathlib import Path

import serial

from . import days

CHUNK = 4096
MAX_LINE = 512  # usb::kMaxLine
MAX_PATH = 255  # usb::kMaxPath
UPLOAD_IDLE = 5.0
SESSION_IDLE = 60.0
SD_TOTAL = 7_948_206_080
BUTTONS = {"back", "confirm", "left", "right", "up", "down", "power"}
WIDTH, HEIGHT = 800, 480
BOOT_NOISE = b"ESP-ROM:esp32c3-api1-20210207\r\nI (312) main: x4-reader sim\r\n" + b'X4 0 event {"type":"boot"}\n'
# Lossy port: a payload longer than BURST loses a stretch past it, as `get` of a whole
# epub did on the X4; LOSS is the chance that any other payload loses one byte.
BURST = 8192
LOSS = 0.05
FAULTS = {"drop", "flip", "cut"}


# firmware/test/learn/x4play, where CLAUDE.md's host test build puts it; firmware/test/draw/x4paper
# beside it, and the card fonts it draws with (`X4_FONTS`, else build/fonts: Literata 12 and 14).
BUILD = Path(__file__).resolve().parents[3] / "build"
PLAYER = BUILD / "test/learn/x4play"
PAINTER = BUILD / "test/draw/x4paper"


def player_path() -> Path:
    """The set player: `X4_PLAY`, else the host test build's. Raises if it is not built."""
    path = Path(os.environ.get("X4_PLAY") or PLAYER)
    if not path.is_file():
        raise FileNotFoundError(
            f"{path} is not built. Build it: "
            "~/.platformio/packages/tool-cmake/bin/cmake -S firmware/test -B build/test && "
            "~/.platformio/packages/tool-cmake/bin/cmake --build build/test --target x4play"
        )
    return path


class Player:
    """`x4play` over the simulator's folder: one command a line in, one JSON line back."""

    def __init__(self, binary: Path, root: Path):
        command = [str(binary), str(root)]
        self._proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(self, command: str) -> dict:
        self._proc.stdin.write(command + "\n")
        self._proc.stdin.flush()
        reply = json.loads(self._proc.stdout.readline())
        if "error" in reply:
            raise SimError("bad_request", reply["error"])
        return reply

    def close(self) -> None:
        self._proc.stdin.close()
        self._proc.wait(timeout=5)


def _valid_path(p: str) -> bool:
    """UsbProtocol.cpp validPath: absolute, at most MAX_PATH bytes, no empty, . or .. component."""
    if not p.startswith("/") or len(p.encode()) > MAX_PATH:
        return False
    if p == "/":
        return True
    if any(c < " " or c in "\x7f\\" for c in p):
        return False
    return all(part not in ("", ".", "..") for part in p[1:].split("/"))


class SimError(Exception):
    def __init__(self, code: str, message: str = ""):
        super().__init__(message or code)
        self.code = code
        self.message = message or code


class Sim:
    """Device state. `presses`, `clock` and `flashed` record what the host did.

    `faults` breaks the next payloads sent, one each: "drop" loses a byte, "flip" changes
    one, "cut" loses the payload and the `ok` after it. `lossy` breaks them like the X4's port.
    """

    def __init__(self, root: Path, lossy: bool = False, play: Path | None = None):
        self.root = root.resolve()
        self.lossy = lossy
        self.faults: list[str] = []
        self.payloads = 0  # data payloads sent
        self._rng = random.Random(4)
        self.root.mkdir(parents=True, exist_ok=True)
        # With a player (player_path()), press and screen play the paper.
        self.player = Player(play, self.root) if play else None
        self.presses: list[dict] = []
        self.busy_presses = 0  # the next presses refused as busy, as while one is still settling
        self.lag: list[dict] = []  # what `lag` reports: the device's press timings
        self.heap_low: list[dict] = []  # what `heap` reports as the last new lows
        self.clock: dict | None = None
        self.activity = "home"
        self.slot = "ota_0"
        self.flashed: bytes | None = None
        self.fw = "x4r-0.0.0-0000000"
        self.mac = "aa:bb:cc:dd:ee:ff"
        self.rolls_back = False  # the next flashed image never confirms; the old slot boots
        self.session = False
        self.booted = False
        self.networks: dict[str, str] = {}  # Wi-Fi ssid -> password, as `net` saved them
        self.fetched = 0
        self._fd = -1
        self._buf = b""
        self._last = time.monotonic()

    # io

    def _write(self, data: bytes) -> None:
        while data:
            n = os.write(self._fd, data)
            data = data[n:]

    def _reply(self, rid: int, kind: str, obj: dict) -> None:
        self._write(f"X4 {rid} {kind} {json.dumps(obj, separators=(',', ':'))}\n".encode())

    def _payload(self, rid: int, head: dict, data: bytes) -> None:
        """A `data` line, the bytes and `ok`, through whatever loses bytes on the way."""
        self.payloads += 1
        fault = self.faults.pop(0) if self.faults else None
        if fault is None and self.lossy:
            if len(data) > BURST:
                data = data[:BURST] + data[BURST + len(data) // 20 :]
            elif data and self._rng.random() < LOSS:
                fault = self._rng.choice(sorted(FAULTS))
        self._reply(rid, "data", head)
        if fault == "cut":
            return
        if data and fault == "drop":
            i = self._rng.randrange(len(data))
            data = data[:i] + data[i + 1 :]
        elif data and fault == "flip":
            i = self._rng.randrange(len(data))
            data = data[:i] + bytes([data[i] ^ 0x40]) + data[i + 1 :]
        self._write(data)
        self._reply(rid, "ok", {})

    def _fill(self, timeout: float) -> bool:
        ready, _, _ = select.select([self._fd], [], [], timeout)
        if not ready:
            return False
        try:
            chunk = os.read(self._fd, 65536)
        except OSError:
            return False
        self._buf += chunk
        return bool(chunk)

    def _recv(self, n: int) -> bytes | None:
        """Exactly n bytes, or None if the host goes quiet for UPLOAD_IDLE seconds. Like the X4,
        the upload has taken the bytes that came, so they are dropped with it."""
        while len(self._buf) < n:
            if not self._fill(UPLOAD_IDLE):
                self._buf = b""
                return None
        out, self._buf = self._buf[:n], self._buf[n:]
        return out

    def serve(self, fd: int, stop: threading.Event) -> None:
        self._fd = fd
        while not stop.is_set():
            if b"\n" not in self._buf:
                self._fill(0.1)
                if self.session and time.monotonic() - self._last > SESSION_IDLE:
                    self.session = False
                continue
            line, self._buf = self._buf.split(b"\n", 1)
            line = line.rstrip(b"\r")
            self._handle(line.decode("utf-8", "replace"), len(line) > MAX_LINE)

    # dispatch

    def _handle(self, line: str, too_long: bool = False) -> None:
        if not line.startswith("X4 "):
            return
        self._last = time.monotonic()
        if not self.booted:
            self._write(BOOT_NOISE)
            self.booted = True
        parts = line.split(" ", 3)
        try:
            rid = int(parts[1])
        except (IndexError, ValueError):
            self._reply(0, "err", {"code": "bad_request", "message": "missing request id"})
            return
        if too_long:
            self._reply(rid, "err", {"code": "too_large", "message": f"request line over {MAX_LINE} bytes"})
            return
        try:
            command = parts[2] if len(parts) > 2 else ""
            try:
                args = json.loads(parts[3]) if len(parts) > 3 else {}
            except json.JSONDecodeError:
                raise SimError("bad_request", "args are not JSON") from None
            if not isinstance(args, dict):
                raise SimError("bad_request", "args must be a JSON object")
            handler = getattr(self, f"cmd_{command}", None)
            if handler is None:
                raise SimError("unknown_command", f"unknown command {command!r}")
            result = handler(rid, args)
            if result is not None:
                self._reply(rid, "ok", result)
        except SimError as e:
            self._reply(rid, "err", {"code": e.code, "message": e.message})
        except OSError as e:
            self._reply(rid, "err", {"code": "io", "message": str(e)})

    def _path(self, args: dict, key: str = "path") -> Path:
        p = args.get(key)
        if not isinstance(p, str) or not _valid_path(p):
            raise SimError("bad_request", f"{key} must be absolute, without . or ..")
        full = (self.root / p.lstrip("/")).resolve()
        if full != self.root and self.root not in full.parents:
            raise SimError("bad_request", f"{key} is outside the SD card")
        return full

    def _int(self, args: dict, key: str) -> int:
        v = args.get(key)
        if not isinstance(v, int) or isinstance(v, bool) or v < 0:
            raise SimError("bad_request", f"{key} must be a non-negative integer")
        return v

    def _used(self) -> int:
        return sum(f.stat().st_size for f in self.root.rglob("*") if f.is_file())

    def _stats(self) -> dict:
        try:
            return json.loads((self.root / "x4/stats.json").read_text())
        except (OSError, ValueError):
            return {}

    def _issue(self) -> str | None:
        """The date of the issue on the card, as hello names it."""
        try:
            with (self.root / "x4/read.jsonl").open(encoding="utf-8") as f:
                head = json.loads(f.readline())
        except (OSError, ValueError):
            return None
        return head.get("date") if isinstance(head, dict) and isinstance(head.get("date"), str) else None

    def _receive(self, rid: int, size: int) -> bytes | None:
        self._reply(rid, "ready", {"chunk": CHUNK})
        data = bytearray()
        while len(data) < size:
            part = self._recv(min(CHUNK, size - len(data)))
            if part is None:
                return None
            data += part
            self._reply(rid, "ack", {"n": len(data)})
        return bytes(data)

    # commands

    def cmd_hello(self, rid: int, args: dict) -> dict:
        self.cmd_time(rid, args)
        self.session = True
        stats = self._stats()
        return {
            "proto": 2,
            "fw": self.fw,
            "board": "x4",
            "slot": self.slot,
            "mac": self.mac,
            "battery": 82,
            "charging": True,
            "sd_free": SD_TOTAL - self._used(),
            "sd_total": SD_TOTAL,
            "day": stats.get("day", days.today()),
            **({"issue": issue} if (issue := self._issue()) else {}),
            **({"retention": stats["retention"]} if "retention" in stats else {}),
            "heap_free": 150 * 1024,
            "heap_min": 120 * 1024,
            "heap_block": 64 * 1024,
            "net": self._net(),
        }

    def _net(self) -> dict:
        return {"networks": sorted(self.networks), "fetching": False, **({"fetched": self.fetched} if self.fetched else {})}

    def cmd_net(self, rid: int, args: dict) -> dict:
        add = args.get("wifi")
        if add is not None:
            ssid = add.get("ssid") if isinstance(add, dict) else None
            if not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32:
                raise SimError("bad_request", "wifi.ssid must be 1 to 32 bytes")
            self.networks[ssid] = add.get("password", "")
        if isinstance(args.get("forget"), str):
            self.networks.pop(args["forget"], None)
        if args.get("fetch"):
            if not self.networks:
                raise SimError("bad_request", "no Wi-Fi network saved")
            # A fetch that found the zone, one event and a forecast in /x4/net.json.
            cfg = json.loads((self.root / "x4/net.json").read_text()) if (self.root / "x4/net.json").exists() else {}
            self.fetched = int(time.time())
            agenda: dict = {"fetched": self.fetched}
            if cfg.get("calendars"):
                agenda["events"] = [{"start": self.fetched + 3600, "end": self.fetched + 7200, "title": "Sim event", "cal": 0, "uid": 1}]
            if cfg.get("weather"):
                agenda["weather"] = [{"date": time.strftime("%Y-%m-%d"), "code": 3, "high": 12, "low": 4}]
            (self.root / "x4").mkdir(parents=True, exist_ok=True)
            (self.root / "x4/agenda.json").write_text(json.dumps(agenda))
        return {"net": self._net()}

    def cmd_time(self, rid: int, args: dict) -> dict:
        self.clock = {"time": self._int(args, "time"), "tz": args.get("tz", 0)}
        if self.player:
            self.player.ask(f"time {self.clock['time']} {int(self.clock['tz'] or 0)}")
        return {}

    def _wrote(self, path: Path) -> None:
        """As the X4 does after a put, rm or mv under /x4: the open set is brought up to date."""
        data = self.root / "x4"
        if self.player and (path == data or data in path.parents):
            self.player.ask("changed")

    def cmd_bye(self, rid: int, args: dict) -> dict:
        self.session = False
        return {}

    def cmd_ls(self, rid: int, args: dict) -> dict:
        d = self._path(args)
        if not d.exists():
            raise SimError("not_found", "no such folder")
        if not d.is_dir():
            raise SimError("bad_request", "not a folder")
        entries = [
            {"name": f.name, "dir": f.is_dir(), "size": 0 if f.is_dir() else f.stat().st_size}
            for f in sorted(d.iterdir())
            if not f.name.endswith(".part")
        ]
        return {"entries": entries}

    def cmd_stat(self, rid: int, args: dict) -> dict:
        p = self._path(args)
        if not p.exists():
            raise SimError("not_found", "no such file")
        return {"dir": p.is_dir(), "size": 0 if p.is_dir() else p.stat().st_size}

    def cmd_get(self, rid: int, args: dict) -> None:
        p = self._path(args)
        if not p.exists():
            raise SimError("not_found", "no such file")
        if p.is_dir():
            raise SimError("bad_request", "is a folder")
        offset = self._int({"offset": args.get("offset", 0)}, "offset")
        length = args.get("length")
        total = p.stat().st_size
        if offset > total:
            raise SimError("bad_request", "offset past the end")
        with p.open("rb") as f:
            f.seek(offset)
            data = f.read() if length is None else f.read(self._int(args, "length"))
        self._payload(rid, {"size": len(data), "crc32": zlib.crc32(data), "total": total}, data)

    def cmd_put(self, rid: int, args: dict) -> None:
        p = self._path(args)
        size, crc = self._int(args, "size"), self._int(args, "crc32")
        if not p.parent.is_dir():
            raise SimError("not_found", "parent folder does not exist; mkdir it first")
        if p.is_dir():
            raise SimError("bad_request", "is a folder")
        if size > SD_TOTAL - self._used():
            raise SimError("no_space", "not enough free space")
        part = p.with_name(p.name + ".part")
        data = self._receive(rid, size)
        if data is None:
            part.unlink(missing_ok=True)
            raise SimError("io", "upload stalled; aborted")
        part.write_bytes(data)
        if zlib.crc32(data) != crc:
            part.unlink()
            raise SimError("crc", "CRC mismatch; upload discarded")
        part.replace(p)
        self._wrote(p)
        self._reply(rid, "ok", {})

    def cmd_rm(self, rid: int, args: dict) -> dict:
        p = self._path(args)
        if p == self.root:
            raise SimError("bad_request", "cannot remove the SD card root")
        if not p.exists():
            raise SimError("not_found", "no such file")
        if p.is_dir():
            if any(p.iterdir()) and not args.get("recursive"):
                raise SimError("bad_request", "folder is not empty; pass recursive")
            shutil.rmtree(p)
        else:
            p.unlink()
        self._wrote(p)
        return {}

    def cmd_mkdir(self, rid: int, args: dict) -> dict:
        p = self._path(args)
        if p.exists() and not p.is_dir():
            raise SimError("exists", "a file has that name")
        p.mkdir(parents=True, exist_ok=True)
        return {}

    def cmd_mv(self, rid: int, args: dict) -> dict:
        src, dst = self._path(args, "from"), self._path(args, "to")
        if not src.exists():
            raise SimError("not_found", "no such file")
        if not dst.parent.is_dir():
            raise SimError("not_found", "target folder does not exist")
        if dst.is_dir() and dst != src:
            shutil.rmtree(dst)
        src.replace(dst)
        self._wrote(src)
        self._wrote(dst)
        return {}

    def _drawn(self) -> tuple[bytes, int, int] | None:
        """The paper's screen as the firmware draws it: (1-bit frame, width, height), or None
        when no paper screen shows or `x4paper` or its fonts are not there."""
        view = self.player.ask("screen").get("paper") if self.player else None
        fonts = Path(os.environ.get("X4_FONTS") or BUILD / "fonts")
        if not view or not PAINTER.is_file() or not fonts.is_dir():
            return None
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "screen.pgm"
            done = subprocess.run(
                [str(PAINTER), "--fonts", str(fonts), "--out", str(out)], input=json.dumps(view), text=True, capture_output=True
            )
            if done.returncode != 0 or not out.is_file():
                return None
            magic, size, _, pixels = out.read_bytes().split(b"\n", 3)
        width, height = map(int, size.split())
        if magic != b"P5" or len(pixels) != width * height:
            return None
        frame = bytearray(width * height // 8)
        for i, value in enumerate(pixels):
            if value >= 128:  # 1 is white
                frame[i >> 3] |= 0x80 >> (i & 7)
        return bytes(frame), width, height

    def cmd_screenshot(self, rid: int, args: dict) -> None:
        frame, width, height = self._drawn() or (b"\xff" * (WIDTH * HEIGHT // 8), WIDTH, HEIGHT)  # else blank
        head = {"size": len(frame), "crc32": zlib.crc32(frame), "width": width, "height": height, "bpp": 1}
        self._reply(rid, "data", head)
        self._write(frame)
        self._reply(rid, "ok", {})

    def cmd_press(self, rid: int, args: dict) -> dict:
        button = args.get("button")
        if button not in BUTTONS:
            raise SimError("bad_request", f"button must be one of {' '.join(sorted(BUTTONS))}")
        if self.busy_presses > 0:
            self.busy_presses -= 1
            raise SimError("busy", "a press is in progress")
        self.presses.append({"button": button, "long": bool(args.get("long", False))})
        if self.player:
            self.player.ask(f"press {button}{' long' if args.get('long') else ''}")
        return {}

    def cmd_screen(self, rid: int, args: dict) -> dict:
        return self.player.ask("screen") if self.player else {"activity": self.activity}

    def cmd_heap(self, rid: int, args: dict) -> dict:
        return {"free": 150 * 1024, "min": 120 * 1024, "block": 64 * 1024, "low": self.heap_low}

    def cmd_lag(self, rid: int, args: dict) -> dict:
        return {"presses": self.lag}

    def cmd_flash(self, rid: int, args: dict) -> None:
        size, digest = self._int(args, "size"), args.get("sha256")
        if not isinstance(digest, str) or len(digest) != 64:
            raise SimError("bad_request", "sha256 must be 64 hex characters")
        if size > 0x640000:
            raise SimError("too_large", "image is larger than an OTA slot")
        data = self._receive(rid, size)
        if data is None:
            raise SimError("io", "upload stalled; aborted")
        if hashlib.sha256(data).hexdigest() != digest.lower():
            raise SimError("crc", "SHA-256 mismatch; image discarded")
        self.flashed = data
        written = "ota_1" if self.slot == "ota_0" else "ota_0"
        self._reply(rid, "ok", {"slot": written})
        if not self.rolls_back:
            self.slot = written
            found = re.search(rb"x4r-[0-9A-Za-z.-]+", data)
            self.fw = found.group().decode() if found else "unknown"
        self._reboot()

    def cmd_reboot(self, rid: int, args: dict) -> None:
        self._reply(rid, "ok", {})
        self._reboot()

    def _reboot(self) -> None:
        self.session = False
        self.booted = False
        self.activity = "home"


def serve_pty(sim: Sim) -> tuple[str, threading.Event]:
    """Serve `sim` on a new pty. Returns the port path and an event that stops it."""
    master, slave = os.openpty()
    tty.setraw(slave)
    stop = threading.Event()

    def run() -> None:
        try:
            sim.serve(master, stop)
        finally:
            os.close(master)
            os.close(slave)

    threading.Thread(target=run, daemon=True, name="x4-sim").start()
    return os.ttyname(slave), stop


def start(root: Path) -> tuple[serial.Serial, callable]:
    """An in-process simulator and an open serial port to it."""
    path, stop = serve_pty(Sim(root))
    ser = serial.Serial(path, timeout=1)
    return ser, stop.set


def run_forever(root: Path, lossy: bool = False, play: Path | None = None) -> None:
    sim = Sim(root, lossy, play)
    path, stop = serve_pty(sim)
    print(f"X4_PORT={path}  (SD card: {sim.root})", flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        stop.set()
