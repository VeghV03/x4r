import json
import os
import time
import zlib

import pytest

from x4 import device
from x4 import sim as simmod
from x4.device import Device, connect, find_port
from x4.errors import DeviceError


@pytest.fixture(autouse=True)
def first_id_is_one(monkeypatch):
    """Scripted replies name request 1; a real session starts at a random id."""
    monkeypatch.setattr(device.random, "randrange", lambda _n: 0)


def test_a_second_session_waits_for_the_port_then_reports_busy(monkeypatch):
    master, slave = os.openpty()
    port = os.ttyname(slave)
    first, _ = device.open_serial(port)
    monkeypatch.setattr(device, "PORT_WAIT", 0.3)
    try:
        with pytest.raises(DeviceError) as e:
            device.open_serial(port)
        assert e.value.code == "busy"
    finally:
        first.close()
        os.close(master)
        os.close(slave)


class FakeSerial:
    """Scripted port: `incoming` is what the device sends, `written` what the host sent."""

    def __init__(self, incoming: bytes = b""):
        self.incoming = bytearray(incoming)
        self.written = bytearray()
        self.timeout = 1

    def write(self, data: bytes) -> int:
        self.written += data
        return len(data)

    def read(self, n: int) -> bytes:
        out = bytes(self.incoming[:n])
        del self.incoming[:n]
        return out

    def read_until(self, expected: bytes = b"\n", size: int | None = None) -> bytes:
        i = self.incoming.find(expected)
        n = len(self.incoming) if i < 0 else i + 1
        if size is not None:
            n = min(n, size)
        return self.read(n)

    def close(self) -> None:
        pass


def test_ignores_noise_events_and_stale_replies():
    ser = FakeSerial(
        b"ESP-ROM:esp32c3\r\nI (10) boot\r\n"
        b'X4 0 event {"type":"usb"}\n'
        b'X4 7 ok {"stale":true}\n'
        b'X4 1 ok {"entries":[]}\n'
    )
    dev = Device(ser, "fake")
    assert dev.ls("/x4") == []
    assert dev.events == [{"type": "usb"}]
    assert bytes(ser.written) == b'X4 1 ls {"path":"/x4"}\n'


def test_err_reply_raises_with_code():
    dev = Device(FakeSerial(b'X4 1 err {"code":"not_found","message":"no such file"}\n'), "fake")
    with pytest.raises(DeviceError) as e:
        dev.get("/x4/a")
    assert e.value.code == "not_found"
    assert str(e.value) == "get /x4/a: not_found: no such file"


def test_malformed_line_is_a_protocol_error():
    dev = Device(FakeSerial(b"X4 one ok {}\n"), "fake")
    with pytest.raises(DeviceError) as e:
        dev.call("screen")
    assert e.value.code == "protocol"


def test_timeout(monkeypatch):
    dev = Device(FakeSerial(), "fake")
    with pytest.raises(DeviceError) as e:
        dev.call("screen", timeout=0.05)
    assert e.value.code == "timeout"


def test_hello_timeout_explains_stock_firmware(monkeypatch):
    monkeypatch.setattr(device, "HELLO_TIMEOUT", 0.05)
    dev = Device(FakeSerial(b"stock firmware log\n"), "/dev/cu.usbmodem101")
    with pytest.raises(DeviceError) as e:
        dev.hello()
    assert e.value.code == "no_protocol"
    assert "not running x4r firmware" in str(e.value)


def test_get_checks_crc(monkeypatch):
    monkeypatch.setattr(device, "GET_TRIES", 1)
    data = b"hello"
    ser = FakeSerial(f'X4 1 data {{"size":5,"crc32":{zlib.crc32(data) ^ 1}}}\n'.encode() + data + b"X4 1 ok {}\n")
    with pytest.raises(DeviceError) as e:
        Device(ser, "fake").get("/a")
    assert e.value.code == "crc"


def _range(rid: int, data: bytes, total: int | None = None) -> bytes:
    head = {"size": len(data), "crc32": zlib.crc32(data)}
    if total is not None:
        head["total"] = total
    return f"X4 {rid} data {json.dumps(head)}\n".encode() + data + f"X4 {rid} ok {{}}\n".encode()


def test_get_from_a_proto_1_device_reads_to_a_short_range():
    blob = os.urandom(device.GET_RANGE + 10)
    ser = FakeSerial(_range(1, blob[: device.GET_RANGE]) + _range(2, blob[device.GET_RANGE :]))
    assert Device(ser, "fake").get("/a") == blob
    asked = [json.loads(line.split(" ", 3)[3]) for line in bytes(ser.written).decode().splitlines()]
    assert asked == [
        {"path": "/a", "offset": 0, "length": device.GET_RANGE},
        {"path": "/a", "offset": device.GET_RANGE, "length": device.GET_RANGE},
    ]


def test_get_stops_at_the_total():
    blob = os.urandom(2 * device.GET_RANGE)
    ser = FakeSerial(_range(1, blob[: device.GET_RANGE], len(blob)) + _range(2, blob[device.GET_RANGE :], len(blob)))
    assert Device(ser, "fake").get("/a") == blob
    assert bytes(ser.written).count(b"\n") == 2  # no empty range at the end


def test_hello_sends_time_and_tz(sim):
    with connect(sim.port) as dev:
        assert dev.info["proto"] == 2
        assert dev.events == [{"type": "boot"}]  # boot noise before the first reply is skipped
    assert abs(sim.clock["time"] - time.time()) < 5
    assert isinstance(sim.clock["tz"], int)
    assert not sim.session  # bye was sent


def test_chunked_put_get_roundtrip(dev, sim):
    blob = os.urandom(3 * simmod.CHUNK + 123)
    dev.mkdir("/x4/decks")
    dev.put("/x4/decks/a.bin", blob)
    assert (sim.root / "x4/decks/a.bin").read_bytes() == blob
    assert dev.get("/x4/decks/a.bin") == blob
    assert dev.get("/x4/decks/a.bin", offset=10, length=5) == blob[10:15]
    assert dev.stat("/x4/decks/a.bin") == {"dir": False, "size": len(blob)}
    assert not list(sim.root.rglob("*.part"))


def test_get_asks_range_by_range(dev, sim):
    blob = os.urandom(3 * device.GET_RANGE + 5)
    (sim.root / "big.bin").write_bytes(blob)
    assert dev.get("/big.bin") == blob
    assert sim.payloads == 4
    start = device.GET_RANGE - 2
    assert dev.get("/big.bin", offset=start, length=device.GET_RANGE + 4) == blob[start : start + device.GET_RANGE + 4]
    assert dev.get("/big.bin", length=0) == b""
    (sim.root / "empty.bin").write_bytes(b"")
    assert dev.get("/empty.bin") == b""
    with pytest.raises(DeviceError) as e:
        dev.get("/big.bin", offset=len(blob) + 1)
    assert e.value.code == "bad_request"


@pytest.fixture
def quick_retries(monkeypatch):
    monkeypatch.setattr(device, "GET_TIMEOUT", 0.3)
    monkeypatch.setattr(device, "DRAIN_QUIET", 0.05)


def test_get_retries_a_broken_range(dev, sim, quick_retries):
    blob = os.urandom(2 * device.GET_RANGE + 7)
    (sim.root / "f.bin").write_bytes(blob)
    sim.faults = ["drop", "flip", "cut"]
    assert dev.get("/f.bin") == blob
    assert sim.payloads == 3 + 3
    assert dev.stat("/f.bin")["size"] == len(blob)  # the session is still in step


def test_get_gives_up_after_its_tries(dev, sim, quick_retries):
    (sim.root / "f.bin").write_bytes(b"x" * 100)
    sim.faults = ["flip"] * device.GET_TRIES
    with pytest.raises(DeviceError) as e:
        dev.get("/f.bin")
    assert e.value.code == "crc"
    assert sim.payloads == device.GET_TRIES
    assert dev.get("/f.bin") == b"x" * 100


def test_lossy_port_loses_a_whole_file_get_but_not_ranges(dev, sim, quick_retries):
    """The X4 bug: a multi-MB get in one burst lost bytes. Ranges come through intact."""
    blob = os.urandom(2 * 1024 * 1024 + 321)
    (sim.root / "book.epub").write_bytes(blob)
    sim.lossy = True
    with pytest.raises(DeviceError):
        dev._download("get", {"path": "/book.epub"}, 0.5)
    dev._drain()
    started = time.monotonic()
    assert dev.get("/book.epub") == blob
    assert time.monotonic() - started < 30


def test_put_counts_acks(sim):
    with connect(sim.port) as dev:
        acks = []
        original = dev._expect

        def spy(rid, what, kind, timeout=device.TIMEOUT):
            obj = original(rid, what, kind, timeout)
            if kind == "ack":
                acks.append(obj["n"])
            return obj

        dev._expect = spy
        dev.put("/f", b"x" * (2 * simmod.CHUNK + 1))
    assert acks == [simmod.CHUNK, 2 * simmod.CHUNK, 2 * simmod.CHUNK + 1]


def test_empty_put(dev, sim):
    dev.put("/empty", b"")
    assert (sim.root / "empty").read_bytes() == b""


def test_put_crc_mismatch_discards_part(dev, sim):
    data = b"abc" * 100
    with pytest.raises(DeviceError) as e:
        dev._upload("put", {"path": "/x.txt", "size": len(data), "crc32": zlib.crc32(data) ^ 1}, data)
    assert e.value.code == "crc"
    assert not (sim.root / "x.txt").exists()
    assert not (sim.root / "x.txt.part").exists()


def test_stalled_upload_is_aborted(dev, sim, monkeypatch):
    monkeypatch.setattr(simmod, "UPLOAD_IDLE", 0.2)
    rid = dev._send("put", {"path": "/s.bin", "size": 100, "crc32": 0})
    dev._expect(rid, "put", "ready")
    dev.ser.write(b"x" * 10)
    with pytest.raises(DeviceError) as e:
        dev._expect(rid, "put", "ok")
    assert e.value.code == "io"
    assert not list(sim.root.glob("s.bin*"))


def test_a_broken_upload_waits_for_the_device_to_drop_it(dev, sim, monkeypatch):
    import serial

    monkeypatch.setattr(simmod, "UPLOAD_IDLE", 0.2)
    write = dev.ser.write

    def cut(data: bytes) -> int:
        if not data.startswith(b"X4 "):
            write(data[:10])
            raise serial.SerialException("cable pulled")
        return write(data)

    monkeypatch.setattr(dev.ser, "write", cut)
    with pytest.raises(DeviceError) as e:
        dev.put("/b.bin", b"y" * 100)
    assert e.value.code == "io" and "write failed" in str(e.value)
    monkeypatch.setattr(dev.ser, "write", write)
    assert dev.ls("/") == []  # not taken as upload bytes


@pytest.mark.parametrize(
    "command,args,code",
    [
        ("get", {"path": "/missing"}, "not_found"),
        ("ls", {"path": "/missing"}, "not_found"),
        ("rm", {"path": "/missing"}, "not_found"),
        ("ls", {"path": "/../etc"}, "bad_request"),
        ("ls", {"path": "relative"}, "bad_request"),
        ("put", {"path": "/no/parent.txt", "size": 1, "crc32": 0}, "not_found"),
        ("press", {"button": "menu"}, "bad_request"),
        ("frobnicate", {}, "unknown_command"),
    ],
)
def test_error_codes(dev, command, args, code):
    with pytest.raises(DeviceError) as e:
        dev.call(command, args)
    assert e.value.code == code


def test_rm_dir_needs_recursive(dev, sim):
    dev.mkdir("/a/b")
    dev.put("/a/b/c", b"1")
    with pytest.raises(DeviceError) as e:
        dev.rm("/a")
    assert e.value.code == "bad_request"
    dev.rm("/a", recursive=True)
    assert not (sim.root / "a").exists()


def test_mkdir_over_file_exists(dev):
    dev.put("/f", b"1")
    with pytest.raises(DeviceError) as e:
        dev.mkdir("/f")
    assert e.value.code == "exists"


def test_mv_replaces_target(dev, sim):
    dev.put("/a", b"new")
    dev.put("/b", b"old")
    dev.mv("/a", "/b")
    assert (sim.root / "b").read_bytes() == b"new"
    assert dev.stat("/a") is None


def test_screenshot_frame(dev):
    head, frame = dev.screenshot()
    assert (head["width"], head["height"], head["bpp"]) == (800, 480, 1)
    assert len(frame) == 48000


def test_flash_verifies_sha256(dev, sim):
    image = b"\xe9" + os.urandom(10000)
    assert dev.flash(image) == {"slot": "ota_1"}
    assert sim.flashed == image


def test_flash_rejects_bad_sha(dev, sim):
    image = b"\xe9" * 100
    with pytest.raises(DeviceError) as e:
        dev._upload("flash", {"size": 100, "sha256": "0" * 64}, image)
    assert e.value.code == "crc"
    assert sim.flashed is None


def test_sim_port_in_process(tmp_path):
    (tmp_path / "x4").mkdir()
    with connect(f"sim:{tmp_path}") as dev:
        assert dev.ls("/") == [{"name": "x4", "dir": True, "size": 0}]


class Port:
    def __init__(self, device, vid):
        self.device, self.vid = device, vid


def test_find_port(monkeypatch):
    monkeypatch.delenv("X4_PORT", raising=False)
    ports = [Port("/dev/cu.Bluetooth", None), Port("/dev/tty.usbmodem9", 0x303A), Port("/dev/cu.usbmodem9", 0x303A)]
    monkeypatch.setattr(device.list_ports, "comports", lambda: ports)
    assert find_port() == "/dev/cu.usbmodem9"
    monkeypatch.setenv("X4_PORT", "sim:/tmp/x")
    assert find_port() == "sim:/tmp/x"


def test_find_port_none(monkeypatch):
    monkeypatch.delenv("X4_PORT", raising=False)
    monkeypatch.setattr(device.list_ports, "comports", lambda: [])
    monkeypatch.setattr(device.glob, "glob", lambda _: [])
    with pytest.raises(DeviceError) as e:
        find_port()
    assert e.value.code == "no_device"



def test_a_long_hungarian_path_fits_the_request_line(dev, sim):
    name = "/" + "ő" * 120  # 241 bytes in UTF-8; as \u escapes the line would pass 512
    dev.put(name, b"x")
    assert (sim.root / name[1:]).read_bytes() == b"x"
    with pytest.raises(DeviceError) as e:
        dev.call("ls", {"path": "/" + "a" * 600})
    assert e.value.code == "too_large"
