"""Full flash backups and firmware updates. esptool is the recovery path."""

import hashlib
import re
import subprocess
import sys
import time
from pathlib import Path

from .device import connect, find_port
from .errors import DeviceError, InvalidData, X4Error

FLASH_SIZE = 0x1000000  # 16 MB
APP_OFFSET = 0x10000
OTADATA = ("0xe000", "0x2000")  # partitions.csv; erased, the bootloader boots app0
IMAGE_MAGIC = 0xE9
# The version x4r reports in hello: x4r-<[x4r] version>-<sha>, once in the image.
IMAGE_VERSION = re.compile(rb"x4r-\d+\.\d+\.\d+-[0-9a-f]{7,40}(?=\x00)")
BOOT_WAIT = 60.0  # seconds for the X4 to reboot and answer hello after a flash


def esptool(args: list[str]) -> str:
    """Run esptool, return its output. Raises DeviceError on failure."""
    r = subprocess.run([sys.executable, "-m", "esptool", *args], capture_output=True, text=True)
    out = r.stdout + r.stderr
    if r.returncode != 0:
        tail = "\n".join(out.strip().splitlines()[-5:])
        command = next((a for a, before in zip(args, ["", *args]) if not a.startswith("-") and not before.startswith("-")), "")
        raise DeviceError("esptool", f"esptool {command} failed:\n{tail}")
    return out


def _real_port(port: str | None) -> str:
    port = port or find_port()
    if port.startswith("sim:"):
        raise X4Error("esptool cannot talk to the simulator; use the real device")
    return port


def read_mac(port: str) -> str:
    out = esptool(["--port", port, "read-mac"])
    m = re.search(r"MAC:\s*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})", out, re.I)
    if not m:
        raise DeviceError("esptool", "esptool read-mac printed no MAC address")
    return m.group(1).lower()


def backup_path(out_dir: Path, mac: str) -> Path:
    return out_dir / f"stock-{mac.replace(':', '')}.bin"


def verified_backup(out_dir: Path, mac: str) -> Path | None:
    """The backup for this device, if a complete one exists (only verified dumps get the final name)."""
    path = backup_path(out_dir, mac)
    return path if path.is_file() and path.stat().st_size == FLASH_SIZE else None


def backup(port: str | None, out_dir: Path) -> dict:
    """Dump all 16 MB of flash, verify it against the chip, then give it its final name."""
    port = _real_port(port)
    mac = read_mac(port)
    path = backup_path(out_dir, mac)
    if path.exists():
        raise X4Error(f"{path} exists; refusing to overwrite a backup")
    out_dir.mkdir(parents=True, exist_ok=True)
    part = path.with_name(path.name + ".part")
    esptool(["--port", port, "read-flash", "0", hex(FLASH_SIZE), str(part)])
    size = part.stat().st_size if part.exists() else 0
    if size != FLASH_SIZE:
        raise DeviceError("esptool", f"read {size} bytes, expected {FLASH_SIZE}; {part} kept for inspection")
    esptool(["--port", port, "verify-flash", "0", str(part)])
    part.rename(path)
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"path": str(path), "mac": mac, "bytes": FLASH_SIZE, "sha256": digest}


def image_version(image: bytes) -> str | None:
    """The x4r version an app image will report in hello, or None for any other image."""
    found = {m.group().decode() for m in IMAGE_VERSION.finditer(image)}
    return found.pop() if len(found) == 1 else None


def _backup_for(backups: Path, mac: str | None) -> Path:
    """The verified backup for this device. Firmware older than the mac field in hello: any one."""
    if mac:
        saved = verified_backup(backups, mac)
    else:
        found = [p for p in sorted(backups.glob("stock-*.bin")) if p.stat().st_size == FLASH_SIZE]
        saved = found[0] if found else None
    if saved is None:
        raise X4Error(f"no verified backup for {mac or 'this X4'} in {backups}; run x4 backup first")
    return saved


def _await_boot(port: str) -> dict | None:
    """The hello of the X4 once it is back after a flash, or None if it never answers."""
    deadline = time.monotonic() + BOOT_WAIT
    while time.monotonic() < deadline:
        try:
            with connect(port) as dev:
                return dev.info
        except DeviceError:
            time.sleep(1.0)
    return None


def flash(port: str | None, firmware: Path, backups: Path) -> dict:
    """Install an x4r image and check that it booted.

    Over the protocol when the X4 speaks it: the image goes to the inactive slot, and the X4
    keeps it only once it has shown Home and heard hello. An image that never answers is reset,
    which rolls it back. Without the protocol, esptool writes app0 and clears otadata.
    """
    image = firmware.read_bytes()
    if not image or image[0] != IMAGE_MAGIC:
        raise InvalidData(f"{firmware}: not an ESP32 app image", [f"{firmware}: first byte must be 0xE9"])
    version = image_version(image)
    if version is None:
        raise InvalidData(
            f"{firmware}: not an x4r image",
            [f"{firmware}: no x4r-<version>-<sha> string; build it with `pio run -e x4r`"],
        )
    try:
        with connect(port) as dev:
            saved = _backup_for(backups, dev.info.get("mac"))
            before = dev.info
            slot = dev.flash(image)["slot"]
            port = dev.port
    except DeviceError as e:
        if e.code != "no_protocol":
            raise
        return _flash_esptool(port, firmware, backups, version)

    after = _await_boot(port)
    if after is None:
        # Still on probation: a reset makes the bootloader go back to the previous slot.
        read_mac(_real_port(port))  # esptool resets the chip when it is done
        after = _await_boot(port)
        if after is None:
            raise DeviceError("timeout", f"the X4 did not answer after flashing {version}, nor after a reset")
        raise DeviceError(
            "rolled_back",
            f"{version} never answered hello; the reset rolled the X4 back to {after.get('fw')}",
        )
    booted = after.get("slot") == slot if "slot" in after else after.get("fw") == version
    if not booted:
        raise DeviceError(
            "rolled_back",
            f"{version} did not reach Home; the X4 rolled back to {after.get('fw')} ({after.get('slot', before.get('fw'))})",
        )
    return {"method": "protocol", "slot": slot, "fw": after.get("fw"), "backup": str(saved)}


def _flash_esptool(port: str | None, firmware: Path, backups: Path, version: str) -> dict:
    port = _real_port(port)
    saved = _backup_for(backups, read_mac(port))
    esptool(["--port", port, "--after", "no-reset", "write-flash", hex(APP_OFFSET), str(firmware)])
    # otadata may name app1 after a protocol flash; erased, it names app0, which now holds the image.
    esptool(["--port", port, "erase-region", *OTADATA])
    after = _await_boot(port)
    if after is None or after.get("fw") != version:
        found = after.get("fw") if after else "no answer"
        raise DeviceError("not_booted", f"esptool wrote {version} to app0, but the X4 reports {found}")
    return {"method": "esptool", "offset": hex(APP_OFFSET), "fw": version, "backup": str(saved)}
