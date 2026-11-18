"""Client for the USB serial protocol in docs/PROTOCOL.md."""

import errno
import glob
import hashlib
import json
import os
import random
import time
import zlib
from collections.abc import Callable
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports

from .errors import DeviceError

ESPRESSIF_VID = 0x303A
TIMEOUT = 10.0  # seconds to wait for any single reply or block of bytes
HELLO_TIMEOUT = 3.0
PRESS_TIMEOUT = 17.0
# The X4 refuses a press as busy while the one before it is still settling; it never acted on it.
PRESS_TRIES = 4
PRESS_RETRY_WAIT = 0.5
PUT_TRIES = 2
HELLO_TRIES = 3
READ_BLOCK = 64 * 1024
MAX_LINE = 16 * 1024 * 1024  # `ls` answers a folder in one line
# `get` asks for one range at a time: the port drops bytes from a long burst nobody paces.
GET_RANGE = 4096
GET_TRIES = 5
GET_TIMEOUT = 3.0  # seconds for one range's reply or bytes
GET_RETRY = {"crc", "timeout", "protocol"}
DRAIN_QUIET = 0.2  # seconds of silence that end a drain
DRAIN_MAX = 5.0
PORT_WAIT = 30.0  # seconds to wait while another x4 command holds the port
UPLOAD_STALL = 5.0  # the device drops an upload that gets no bytes for this long (PROTOCOL.md)


def find_port() -> str:
    """The X4's port: X4_PORT, else the first Espressif USB device, else the first usbmodem."""
    if env := os.environ.get("X4_PORT"):
        return env
    for p in sorted(list_ports.comports(), key=lambda p: p.device):
        if p.vid == ESPRESSIF_VID and p.device.startswith("/dev/cu."):
            return p.device
    for p in sorted(list_ports.comports(), key=lambda p: p.device):
        if p.vid == ESPRESSIF_VID:
            return p.device
    found = sorted(glob.glob("/dev/cu.usbmodem*"))
    if found:
        return found[0]
    raise DeviceError("no_device", "no X4 on USB; plug it in, or pass --port or set X4_PORT")


def open_serial(port: str) -> tuple[serial.Serial, Callable[[], None] | None]:
    """Open a port. `sim:<dir>` starts an in-process simulator with `<dir>` as its SD card."""
    if port.startswith("sim:"):
        from .sim import start

        return start(Path(port[4:]).expanduser())
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200  # ignored by USB-Serial/JTAG
    ser.timeout = 1
    ser.write_timeout = TIMEOUT
    # One session at a time: two on one port would read each other's replies, and the X4
    # would take a second command's bytes as the first one's upload.
    ser.exclusive = True
    deadline = time.monotonic() + PORT_WAIT
    while True:
        try:
            ser.open()
            break
        except serial.SerialException as e:
            if e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                raise DeviceError("io", f"cannot open {port}: {e}") from e
            if time.monotonic() >= deadline:
                raise DeviceError("busy", f"another x4 command has held {port} for {PORT_WAIT:g} s") from e
            time.sleep(0.2)
    # The ESP32-C3 resets when RTS is high while DTR is low. macOS opens the port
    # with both high, so lower RTS before DTR and never pass through that state.
    try:
        ser.rts = False
        ser.dtr = False
    except OSError as e:
        if e.errno not in (errno.ENOTTY, errno.EINVAL):  # a pty has no modem lines
            raise
    return ser, None


def now() -> dict:
    """The Mac's wall clock as `time` and `hello` args."""
    offset = datetime.now().astimezone().utcoffset()
    tz = int(offset.total_seconds() // 60) if offset else 0
    return {"time": int(time.time()), "tz": tz}


class Device:
    """One protocol session. Use `connect()`; it sends `hello` on open and `bye` on close."""

    def __init__(self, ser: serial.Serial, port: str, closer: Callable[[], None] | None = None):
        self.ser = ser
        self.port = port
        self._closer = closer
        # Ids start at random, so a late reply to an earlier session is never taken for this one's.
        self._id = random.randrange(1 << 30)
        self.info: dict = {}
        self.events: list[dict] = []

    # framing

    def _send(self, command: str, args: dict) -> int:
        self._id += 1
        # UTF-8, not \u escapes: a path in Hungarian would cost up to 6 bytes a letter of the 512.
        line = f"X4 {self._id} {command} {json.dumps(args, ensure_ascii=False, separators=(',', ':'))}\n"
        try:
            self.ser.write(line.encode())
        except serial.SerialException as e:
            raise DeviceError("io", f"{command}: write to {self.port} failed: {e}") from e
        return self._id

    def _readline(self, deadline: float) -> bytes | None:
        buf = b""
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            self.ser.timeout = min(left, 1.0)
            try:
                buf += self.ser.read_until(b"\n", MAX_LINE - len(buf))
            except serial.SerialException as e:
                raise DeviceError("io", f"read from {self.port} failed: {e}") from e
            if buf.endswith(b"\n") or len(buf) >= MAX_LINE:
                return buf

    def _next(self, rid: int, what: str, timeout: float = TIMEOUT) -> tuple[str, dict]:
        """The next reply to request `rid`, skipping log lines, events and stale replies."""
        deadline = time.monotonic() + timeout
        while True:
            raw = self._readline(deadline)
            if raw is None:
                raise DeviceError("timeout", f"{what}: no reply from the X4 within {timeout:g} s")
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if not line.startswith("X4 "):
                continue
            parts = line.split(" ", 3)
            try:
                lid, kind, obj = int(parts[1]), parts[2], json.loads(parts[3])
            except (IndexError, ValueError):
                raise DeviceError("protocol", f"{what}: malformed line from the X4: {line[:200]!r}") from None
            if kind == "event":
                self.events.append(obj)
                continue
            if lid != rid:
                continue
            if kind == "err":
                code = obj.get("code", "io")
                raise DeviceError(code, f"{what}: {code}: {obj.get('message') or code}", replied=True)
            return kind, obj

    def _expect(self, rid: int, what: str, kind: str, timeout: float = TIMEOUT) -> dict:
        got, obj = self._next(rid, what, timeout)
        if got != kind:
            raise DeviceError("protocol", f"{what}: expected {kind}, got {got} {obj}")
        return obj

    def _read_exact(self, n: int, what: str, timeout: float = TIMEOUT) -> bytes:
        out = bytearray()
        while len(out) < n:
            self.ser.timeout = timeout
            block = self.ser.read(min(READ_BLOCK, n - len(out)))
            if not block:
                raise DeviceError("timeout", f"{what}: stalled after {len(out)} of {n} bytes")
            out += block
        return bytes(out)

    def _drain(self) -> None:
        """Drop what is left of a broken reply, so the next one starts on a clean line."""
        deadline = time.monotonic() + DRAIN_MAX
        self.ser.timeout = DRAIN_QUIET
        try:
            while self.ser.read(READ_BLOCK) and time.monotonic() < deadline:
                pass
        except serial.SerialException as e:
            raise DeviceError("io", f"read from {self.port} failed: {e}") from e

    # commands

    def call(self, command: str, args: dict | None = None, timeout: float = TIMEOUT) -> dict:
        """A command whose reply is a single `ok` line."""
        what = _what(command, args)
        rid = self._send(command, args or {})
        return self._expect(rid, what, "ok", timeout)

    def _download(self, command: str, args: dict, timeout: float = TIMEOUT) -> tuple[dict, bytes]:
        what = _what(command, args)
        rid = self._send(command, args)
        head = self._expect(rid, what, "data", timeout)
        data = self._read_exact(int(head["size"]), what, timeout)
        # Lost bytes pull in the start of the `ok` line: the CRC catches that before the wait for it.
        if zlib.crc32(data) != int(head["crc32"]):
            raise DeviceError("crc", f"{what}: CRC mismatch on the received bytes; retry")
        self._expect(rid, what, "ok", timeout)
        return head, data

    def _download_retried(self, command: str, args: dict) -> tuple[dict, bytes]:
        """`_download`, asked again when bytes were lost on the way."""
        for _ in range(GET_TRIES - 1):
            try:
                return self._download(command, args, GET_TIMEOUT)
            except DeviceError as e:
                if e.code not in GET_RETRY:
                    raise
                self._drain()
        return self._download(command, args, GET_TIMEOUT)

    def _upload(self, command: str, args: dict, data: bytes, timeout: float = TIMEOUT) -> dict:
        what = _what(command, args)
        rid = self._send(command, args)
        chunk = int(self._expect(rid, what, "ready").get("chunk", 4096))
        sent = 0
        try:
            while sent < len(data):
                part = data[sent : sent + chunk]
                try:
                    self.ser.write(part)
                except serial.SerialException as e:
                    raise DeviceError("io", f"{what}: write failed after {sent} bytes: {e}") from e
                sent += len(part)
                ack = self._expect(rid, what, "ack")
                if ack.get("n") != sent:
                    raise DeviceError("protocol", f"{what}: device acked {ack.get('n')} bytes, sent {sent}")
            return self._expect(rid, what, "ok", timeout)
        except DeviceError as e:
            if not e.replied:
                self._abandon(rid)
            raise

    def _abandon(self, rid: int) -> None:
        """The device still takes every byte as part of the upload, the next command too. Send
        nothing until it drops the upload and says so."""
        try:
            self._next(rid, "abandon", UPLOAD_STALL + 2.0)
        except DeviceError:
            pass

    def hello(self) -> dict:
        # The X4 takes about 3 s to boot after a reset or flash, and drops what arrives
        # before its loop runs, so ask again; a late answer to an earlier hello is skipped.
        for _ in range(HELLO_TRIES):
            rid = self._send("hello", now())
            try:
                self.info = self._expect(rid, "hello", "ok", HELLO_TIMEOUT)
                return self.info
            except DeviceError as e:
                if e.code != "timeout":
                    raise
        raise DeviceError(
            "no_protocol",
            f"{self.port} did not answer hello: the X4 is not running x4r firmware, or it is asleep",
        )

    def ls(self, path: str) -> list[dict]:
        return self.call("ls", {"path": path})["entries"]

    def stat(self, path: str) -> dict | None:
        try:
            return self.call("stat", {"path": path})
        except DeviceError as e:
            if e.code == "not_found":
                return None
            raise

    def get(self, path: str, offset: int = 0, length: int | None = None) -> bytes:
        """`length` bytes from `offset`, or to the end; one checked and retried range at a time."""
        out = bytearray()
        while True:
            want = GET_RANGE if length is None else min(GET_RANGE, length - len(out))
            head, data = self._download_retried("get", {"path": path, "offset": offset + len(out), "length": want})
            out += data
            if len(data) < want or (length is not None and len(out) >= length):
                return bytes(out)
            # proto 1 sends no total: the range that comes back short or empty ends the file.
            if "total" in head and offset + len(out) >= int(head["total"]):
                return bytes(out)

    def put(self, path: str, data: bytes) -> None:
        args = {"path": path, "size": len(data), "crc32": zlib.crc32(data)}
        for attempt in range(PUT_TRIES):
            try:
                self._upload("put", args, data)
                return
            except DeviceError as e:
                # The device dropped the corrupt copy; a long upload can hit a rare bad byte.
                if e.code != "crc" or attempt == PUT_TRIES - 1:
                    raise

    def rm(self, path: str, recursive: bool = False) -> None:
        self.call("rm", {"path": path, "recursive": recursive})

    def mkdir(self, path: str) -> None:
        self.call("mkdir", {"path": path})

    def mv(self, src: str, dst: str) -> None:
        self.call("mv", {"from": src, "to": dst})

    def screenshot(self) -> tuple[dict, bytes]:
        return self._download_retried("screenshot", {})

    def press(self, button: str, long: bool = False) -> None:
        # The device answers once the screen has redrawn, or after 15 s. A press refused as busy
        # never happened, so it is sent again; a timeout is not, since that press may have.
        for attempt in range(PRESS_TRIES):
            try:
                self.call("press", {"button": button, "long": long}, timeout=PRESS_TIMEOUT)
                return
            except DeviceError as e:
                if e.code != "busy" or attempt == PRESS_TRIES - 1:
                    raise
                time.sleep(PRESS_RETRY_WAIT)

    def screen(self) -> dict:
        """`activity`, and `step` while a step shows: its `kind`, `id` and `phase`."""
        return self.call("screen")

    def heap(self) -> dict:
        """Free memory, and the stage that ran at each of its last new lows. Empty on a firmware
        without the command."""
        try:
            return self.call("heap")
        except DeviceError as e:
            if e.code != "unknown_command":
                raise
            return {}

    def lag(self) -> dict:
        reply = self.call("lag")
        return {"presses": reply["presses"], "marks": reply.get("marks", [])}

    def flash(self, image: bytes) -> dict:
        args = {"size": len(image), "sha256": hashlib.sha256(image).hexdigest()}
        result = self._upload("flash", args, image, timeout=60)
        self.info = {}  # the device reboots; the session is over
        return result

    def close(self) -> None:
        try:
            if self.info:
                self.call("bye", timeout=2)
        except DeviceError:
            pass
        finally:
            self.ser.close()
            if self._closer:
                self._closer()

    def __enter__(self) -> "Device":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def _what(command: str, args: dict | None) -> str:
    path = (args or {}).get("path")
    return f"{command} {path}" if path else command


def connect(port: str | None = None) -> Device:
    """Open the port and say hello with the Mac's time. Close it to say bye."""
    port = port or find_port()
    ser, closer = open_serial(port)
    dev = Device(ser, port, closer)
    try:
        dev.hello()
    except BaseException:
        dev.close()
        raise
    return dev
