from pathlib import Path

import pytest

from x4 import flashing
from x4.errors import DeviceError, InvalidData, X4Error

MAC = "aa:bb:cc:dd:ee:ff"


def command(args: list[str]) -> str:
    """The esptool command in `args`, past `--port P` and `--after A`."""
    return [a for i, a in enumerate(args) if not a.startswith("-") and not args[i - 1].startswith("-")][0]


@pytest.fixture
def esptool(monkeypatch):
    """Fake esptool: records calls, writes a full-size dump for read-flash."""
    calls: list[list[str]] = []

    def run(args: list[str]) -> str:
        calls.append(args)
        if command(args) == "read-mac":
            return f"Chip is ESP32-C3\nMAC: {MAC}\n"
        if command(args) == "read-flash":
            with open(args[-1], "wb") as f:
                f.truncate(flashing.FLASH_SIZE)
        return "ok"

    monkeypatch.setattr(flashing, "esptool", run)
    return calls


def test_backup_reads_verifies_and_names(esptool, tmp_path):
    out = flashing.backup("/dev/cu.fake", tmp_path)
    path = tmp_path / "stock-aabbccddeeff.bin"
    assert out["path"] == str(path) and path.stat().st_size == flashing.FLASH_SIZE
    assert [command(c) for c in esptool] == ["read-mac", "read-flash", "verify-flash"]
    assert esptool[1][3:5] == ["0", "0x1000000"]
    assert not list(tmp_path.glob("*.part"))


def test_backup_refuses_to_overwrite(esptool, tmp_path):
    (tmp_path / "stock-aabbccddeeff.bin").write_bytes(b"old")
    with pytest.raises(X4Error, match="refusing to overwrite"):
        flashing.backup("/dev/cu.fake", tmp_path)
    assert (tmp_path / "stock-aabbccddeeff.bin").read_bytes() == b"old"


def test_backup_short_read_keeps_part(monkeypatch, tmp_path):
    def run(args):
        if args[2] == "read-mac":
            return f"MAC: {MAC}"
        Path(args[-1]).write_bytes(b"short")
        return ""

    monkeypatch.setattr(flashing, "esptool", run)
    with pytest.raises(DeviceError, match="expected 16777216"):
        flashing.backup("/dev/cu.fake", tmp_path)
    assert not (tmp_path / "stock-aabbccddeeff.bin").exists()


def test_backup_refuses_simulator(esptool):
    with pytest.raises(X4Error, match="simulator"):
        flashing.backup("sim:/tmp/x", Path("/tmp"))


VERSION = "x4r-0.2.0-abcdef1"


def firmware(tmp_path, version: str = VERSION) -> Path:
    fw = tmp_path / "fw.bin"
    fw.write_bytes(b"\xe9" + b"\x00" * 2000 + version.encode() + b"\x00" * 3000)
    return fw


def backups(tmp_path) -> Path:
    out = tmp_path / "backups"
    out.mkdir(exist_ok=True)
    with open(out / "stock-aabbccddeeff.bin", "wb") as f:
        f.truncate(flashing.FLASH_SIZE)
    return out


def test_image_version():
    assert flashing.image_version(b"\xe9..x4r-0.1.0-5e81c0cc\x00..") == "x4r-0.1.0-5e81c0cc"
    assert flashing.image_version(b"\xe9..1.0.0-dev-main-5e81c0cc\x00..") is None
    assert flashing.image_version(b"x4r-0.1.0-aaaaaaa\x00x4r-0.1.0-bbbbbbb\x00") is None


def test_flash_over_protocol_checks_the_boot(sim, tmp_path, esptool):
    out = flashing.flash(sim.port, firmware(tmp_path), backups(tmp_path))
    assert out["method"] == "protocol" and out["slot"] == "ota_1" and out["fw"] == VERSION
    assert sim.flashed == firmware(tmp_path).read_bytes()
    assert sim.slot == "ota_1"
    assert esptool == []


def test_flash_over_protocol_needs_this_devices_backup(sim, tmp_path):
    out = tmp_path / "backups"
    out.mkdir()
    with open(out / "stock-001122334455.bin", "wb") as f:  # another X4's
        f.truncate(flashing.FLASH_SIZE)
    with pytest.raises(X4Error, match="no verified backup for aa:bb:cc:dd:ee:ff"):
        flashing.flash(sim.port, firmware(tmp_path), out)
    assert sim.flashed is None


def test_flash_reports_a_rollback(sim, tmp_path):
    sim.rolls_back = True
    with pytest.raises(DeviceError) as e:
        flashing.flash(sim.port, firmware(tmp_path), backups(tmp_path))
    assert e.value.code == "rolled_back" and "x4r-0.0.0-0000000" in str(e.value)


def test_flash_rejects_a_default_build(sim, tmp_path):
    fw = tmp_path / "fw.bin"
    fw.write_bytes(b"\xe9" + b"\x00" * 100 + b"2.0.0-dev-main-abcdef1\x00")
    with pytest.raises(InvalidData, match="not an x4r image"):
        flashing.flash(sim.port, fw, backups(tmp_path))
    assert sim.flashed is None


def no_protocol(port):
    raise DeviceError("no_protocol", "stock firmware")


def test_esptool_flash_needs_verified_backup(monkeypatch, esptool, tmp_path):
    monkeypatch.setattr(flashing, "connect", no_protocol)
    with pytest.raises(X4Error, match="no verified backup"):
        flashing.flash("/dev/cu.fake", firmware(tmp_path), tmp_path)
    assert [command(c) for c in esptool] == ["read-mac"]
    (tmp_path / "stock-aabbccddeeff.bin").write_bytes(b"partial")  # wrong size: not a verified dump
    with pytest.raises(X4Error, match="no verified backup"):
        flashing.flash("/dev/cu.fake", firmware(tmp_path), tmp_path)


def test_esptool_flash_writes_app0_and_clears_otadata(monkeypatch, esptool, tmp_path):
    monkeypatch.setattr(flashing, "connect", no_protocol)
    monkeypatch.setattr(flashing, "_await_boot", lambda port: {"fw": VERSION, "slot": "ota_0"})
    out = flashing.flash("/dev/cu.fake", firmware(tmp_path), backups(tmp_path))
    assert out["method"] == "esptool" and out["offset"] == "0x10000" and out["fw"] == VERSION
    assert esptool[-2] == ["--port", "/dev/cu.fake", "--after", "no-reset", "write-flash", "0x10000", str(firmware(tmp_path))]
    assert esptool[-1] == ["--port", "/dev/cu.fake", "erase-region", "0xe000", "0x2000"]


def test_flash_rejects_non_image(tmp_path):
    fw = tmp_path / "x.bin"
    fw.write_bytes(b"MZ")
    with pytest.raises(X4Error, match="not an ESP32 app image"):
        flashing.flash("sim:/tmp", fw, tmp_path)
