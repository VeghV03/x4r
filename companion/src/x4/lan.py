"""The issue over Wi-Fi: `x4 stage` writes X4_HOME/outbox/, `x4 serve` hands it to the X4.

The contract is "LAN issue" and "Repo issue" in docs/PROTOCOL.md. The outbox holds the files
at their device paths (outbox/x4/stream.jsonl, outbox/books/read/<id>.epub) and manifest.json.
The X4 takes it from the Mac on the LAN, or from the data repo once the outbox is there. The
server answers only the token in X4_HOME/serve.json, and only for files the manifest lists.
"""

import hmac
import json
import os
import plistlib
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from .errors import InvalidData, X4Error
from .home import Home, read_json, write_json

PORT = 8744
MANIFEST = "manifest.json"
FIXED = ("/x4/stream.jsonl", "/x4/read.jsonl", "/x4/football.json", "/x4/threads.jsonl", "/x4/previously.jsonl")
# A Read article, a deck, a crossword set: as the firmware's issuePathAllowed has them.
NAMED = re.compile(
    r"/books/read/[A-Za-z0-9_-][A-Za-z0-9._-]{0,31}\.epub"
    r"|/x4/decks/[A-Za-z0-9_-][A-Za-z0-9._-]{0,31}\.jsonl"
    r"|/x4/crosswords/[A-Za-z0-9_-][A-Za-z0-9._-]?\.jsonl"
)
PRUNABLE = ("/books/read", "/x4/decks")
MAX_FILES, MAX_FILE, MAX_TOTAL = 48, 4 * 1024 * 1024, 16 * 1024 * 1024
MAX_BODY = 64 * 1024  # a POST /taken body
LABEL = "x4r.serve"
OLD_LABELS = ("buzz.solarpunk.x4.serve",)  # removed by install and uninstall
WATCH = 30  # seconds between checks that the LAN address is still ours


def allowed(path: str) -> bool:
    return path in FIXED or NAMED.fullmatch(path) is not None


def problems(files: list[tuple[str, int]], prune: list[str]) -> list[str]:
    """What breaks the manifest rules, for (path, size) pairs."""
    out = [f"{p}: not a path the X4 takes over Wi-Fi" for p, _ in files if not allowed(p)]
    out += [f"{p}: {size} bytes, more than {MAX_FILE}" for p, size in files if size > MAX_FILE]
    paths = [p for p, _ in files]
    out += [f"{p}: listed twice" for p in sorted({p for p in paths if paths.count(p) > 1})]
    if len(files) > MAX_FILES:
        out.append(f"{len(files)} files, more than {MAX_FILES}")
    if (total := sum(size for _, size in files)) > MAX_TOTAL:
        out.append(f"{total} bytes in all, more than {MAX_TOTAL}")
    out += [f"prune {p}: only {', '.join(PRUNABLE)}" for p in prune if p not in PRUNABLE]
    return out


def write_outbox(h: Home, files: list[tuple[str, bytes]], prune: list[str]) -> dict:
    """Replace X4_HOME/outbox with these files and their manifest, built next to it and swapped in."""
    if errors := problems([(p, len(d)) for p, d in files], prune):
        raise InvalidData("staged nothing: the issue breaks the limits of an issue over Wi-Fi", errors)
    manifest = {
        "issue": datetime.now().isoformat(timespec="seconds"),
        "files": [{"path": p, "size": len(d), "crc32": zlib.crc32(d)} for p, d in files],
        "prune": prune,
    }
    h.root.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=".outbox-", dir=h.root))
    try:
        for path, data in files:
            dst = tmp / path.lstrip("/")
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
        (tmp / MANIFEST).write_text(json.dumps(manifest, ensure_ascii=False, separators=(",", ":")) + "\n")
        old = None
        if h.outbox.exists():
            old = h.outbox.with_name(f".outbox-old-{secrets.token_hex(4)}")
            h.outbox.replace(old)
        tmp.replace(h.outbox)
        if old:
            shutil.rmtree(old, ignore_errors=True)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    return manifest


def empty_outbox(h: Home) -> None:
    if h.outbox.exists():
        shutil.rmtree(h.outbox)


def manifest(h: Home) -> dict | None:
    """The staged manifest, or None when nothing is staged."""
    try:
        return json.loads((h.outbox / MANIFEST).read_bytes())
    except (OSError, ValueError):
        return None


# where the Mac listens


def lan_ip() -> str | None:
    """The Mac's IPv4 address on the LAN: the source address of a route out. Sends nothing."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.0.1", 9))  # UDP connect only picks the route
        ip = s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()
    return None if ip.startswith(("127.", "0.")) else ip


def config(h: Home, port: int | None = None) -> dict:
    """X4_HOME/serve.json, made with a fresh token on first use. A new `port` is saved."""
    cfg = read_json(h.serve, {})
    changed = False
    if not cfg.get("token"):
        cfg["token"], changed = secrets.token_hex(16), True
    if (port and port != cfg.get("port")) or not cfg.get("port"):
        cfg["port"], changed = port or PORT, True
    if changed:
        h.root.mkdir(parents=True, exist_ok=True)
        write_json(h.serve, cfg)
        h.serve.chmod(0o600)  # the token lets anyone on the LAN read the issue
    return cfg


def mac(h: Home, ip: str | None = None) -> dict | None:
    """`mac` for /x4/net.json: where `x4 serve` listens, once serve.json exists and the Mac has a LAN address."""
    if not h.serve.exists():
        return None
    cfg = read_json(h.serve, {})
    ip = ip or lan_ip()
    if not ip or not cfg.get("token"):
        return None
    return {"url": f"http://{ip}:{cfg.get('port') or PORT}", "token": cfg["token"]}


# the server


def taken(h: Home, body: dict) -> None:
    """Record what the X4 took, so the next `x4 sync` does not push it again."""
    from .ops import _state

    current = manifest(h) or {}
    crcs = {f["path"]: f["crc32"] for f in current.get("files", [])}
    state = _state(h)
    if body.get("issue") == current.get("issue"):  # an older issue's files may differ from these
        for path in body.get("files") or []:
            if isinstance(path, str) and path in crcs:
                state["pushed"][path] = crcs[path]
    state["lan"] = {"issue": body.get("issue"), "at": datetime.now().isoformat(timespec="seconds")}
    write_json(h.sync_state, state)


def handler(h: Home, token: str) -> type[BaseHTTPRequestHandler]:
    expected = f"Bearer {token}".encode()

    class Handler(BaseHTTPRequestHandler):
        server_version = "x4"
        sys_version = ""

        def _authorized(self) -> bool:
            if hmac.compare_digest(self.headers.get("Authorization", "").encode(), expected):
                return True
            self._answer(401)
            return False

        def _answer(self, code: int, data: bytes = b"", kind: str = "application/json") -> None:
            self.send_response(code)
            if data:
                self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            if data:
                self.wfile.write(data)

        def do_GET(self) -> None:
            if not self._authorized():
                return
            url = urlsplit(self.path)
            if url.path == "/issue":
                data = None
                try:
                    data = (h.outbox / MANIFEST).read_bytes()
                except OSError:
                    pass
                if data:
                    self._answer(200, data)
                else:
                    self._answer(204)
            elif url.path == "/file":
                path = (parse_qs(url.query).get("path") or [""])[0]
                listed = {f["path"] for f in (manifest(h) or {}).get("files", [])}
                if path not in listed or not allowed(path):
                    return self._answer(404)
                try:
                    data = (h.outbox / path.lstrip("/")).read_bytes()
                except OSError:
                    return self._answer(404)
                self._answer(200, data, "application/octet-stream")
            else:
                self._answer(404)

        def do_POST(self) -> None:
            if not self._authorized():
                return
            if urlsplit(self.path).path != "/taken":
                return self._answer(404)
            size = int(self.headers.get("Content-Length") or 0)
            if not 0 < size <= MAX_BODY:
                return self._answer(400)
            try:
                body = json.loads(self.rfile.read(size))
                if not isinstance(body, dict):
                    raise ValueError
            except ValueError:
                return self._answer(400)
            taken(h, body)
            self._answer(204)

    return Handler


def server(h: Home, host: str, port: int) -> ThreadingHTTPServer:
    token = config(h)["token"]
    s = ThreadingHTTPServer((host, port), handler(h, token))
    s.daemon_threads = True
    return s


def serve(h: Home, port: int | None = None, host: str | None = None, ready=None) -> None:
    """Serve the outbox on the LAN until stopped. Without `host`, exits when the Mac's address
    changes, so launchd starts it again on the new one."""
    cfg = config(h, port)
    bind = host or lan_ip()
    if not bind:
        raise X4Error("no LAN address: is the Mac on a network?")
    try:
        s = server(h, bind, cfg["port"])
    except OSError as e:
        raise X4Error(f"cannot listen on {bind}:{cfg['port']}: {e}") from e
    if ready:
        ready({"url": f"http://{bind}:{cfg['port']}", "outbox": str(h.outbox)})
    thread = threading.Thread(target=s.serve_forever, daemon=True)
    thread.start()
    try:
        while thread.is_alive():
            time.sleep(WATCH)
            if not host and lan_ip() != bind:
                raise X4Error(f"the LAN address is no longer {bind}")
    finally:
        s.shutdown()
        s.server_close()


# launchd


def plist_path(label: str = LABEL) -> Path:
    return Path.home() / "Library/LaunchAgents" / f"{label}.plist"


def _bootout_old(domain: str) -> None:
    for old in OLD_LABELS:
        subprocess.run(["launchctl", "bootout", f"{domain}/{old}"], capture_output=True, check=False)
        plist_path(old).unlink(missing_ok=True)


def x4_command() -> str:
    """The `x4` of this environment (the venv's), for launchd."""
    local = Path(sys.executable).parent / "x4"
    return str(local) if local.exists() else shutil.which("x4") or str(local)


def plist(program: str, home: Path, log: Path) -> bytes:
    return plistlib.dumps(
        {
            "Label": LABEL,
            "ProgramArguments": [program, "serve"],
            "EnvironmentVariables": {"X4_HOME": str(home)},
            "RunAtLoad": True,
            "KeepAlive": True,
            "ThrottleInterval": 30,
            "StandardOutPath": str(log),
            "StandardErrorPath": str(log),
        }
    )


def install(h: Home, port: int | None = None) -> dict:
    """Run `x4 serve` at login with KeepAlive, and start it now."""
    cfg = config(h, port)
    path = plist_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    log = Path.home() / "Library/Logs/x4-serve.log"
    domain = f"gui/{os.getuid()}"
    subprocess.run(["launchctl", "bootout", f"{domain}/{LABEL}"], capture_output=True, check=False)  # an older one
    _bootout_old(domain)
    path.write_bytes(plist(x4_command(), h.root, log))
    r = subprocess.run(["launchctl", "bootstrap", domain, str(path)], capture_output=True, text=True, check=False)
    if r.returncode:
        raise X4Error(f"launchctl bootstrap: {r.stderr.strip() or r.returncode}")
    return {"plist": str(path), "log": str(log), "port": cfg["port"], "mac": mac(h)}


def uninstall() -> dict:
    path = plist_path()
    subprocess.run(["launchctl", "bootout", f"gui/{os.getuid()}/{LABEL}"], capture_output=True, check=False)
    existed = path.exists() or any(plist_path(old).exists() for old in OLD_LABELS)
    _bootout_old(f"gui/{os.getuid()}")
    path.unlink(missing_ok=True)
    return {"plist": str(path), "removed": existed}
