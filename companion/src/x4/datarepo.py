"""X4_HOME as a checkout of the data repo: fetch first, send after.

The contract is "The data repo" in docs/DATA.md. The repo is the true copy of the writer's
memory, so whoever writes brings its copy up to the repo before it starts (`x4 pull`,
`x4 context`) and sends what it wrote when it is done (`x4 sync`, `x4 stage`). A home that is
not a checkout with a remote is left alone: nothing here runs.
"""

import re
import subprocess

from .errors import X4Error
from .home import Home

# What never goes to the repo: secrets, and what can be had again.
IGNORE = (
    "net.json",
    "repo.json",
    "serve.json",
    "football.key",
    "cache/",
    "library/",
    ".outbox-*/",
    "*.tmp",
    "*.pending",
    ".DS_Store",
)


def _git(h: Home, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(h.root), *args], capture_output=True, text=True)


def _said(r: subprocess.CompletedProcess) -> list[str]:
    return [line for line in (r.stderr + r.stdout).splitlines() if line.strip()][-6:]


def is_checkout(h: Home) -> bool:
    """True when X4_HOME is a git checkout with a remote to fetch from and send to."""
    return (h.root / ".git").exists() and bool(_git(h, "remote").stdout.strip())


def fetch_first(h: Home) -> dict | None:
    """Bring this copy up to the repo, keeping what was written here since. None when X4_HOME
    is not a checkout."""
    if not is_checkout(h):
        return None
    r = _git(h, "fetch", "--quiet", "origin")
    if r.returncode:
        raise X4Error("cannot reach the data repo; nothing was changed here", _said(r))
    upstream = "origin/" + _git(h, "branch", "--show-current").stdout.strip()
    if _git(h, "rev-parse", "--verify", "--quiet", upstream).returncode:
        return {"fetched": 0}  # the repo has nothing yet
    behind = _git(h, "rev-list", "--count", f"HEAD..{upstream}").stdout.strip()
    if behind in ("", "0"):
        return {"fetched": 0}
    r = _git(h, "rebase", "--autostash", upstream)
    if r.returncode:
        _git(h, "rebase", "--abort")
        raise X4Error(
            "the data repo and this copy changed the same file; this copy is as it was",
            _said(r) + [f"settle it by hand: git -C {h.root} status"],
        )
    return {"fetched": int(behind)}


def send_after(h: Home, what: str) -> dict | None:
    """Commit what was written and send it to the repo. The X4 adds files to the repo's inbox
    at any hour; those never clash with what a writer wrote, so the copy is put on top of them
    and sent. Refuses when another writer changed the repo since this copy fetched it: the
    commit stays here. None when X4_HOME is not a checkout."""
    if not is_checkout(h):
        return None
    if h.football_cfg.exists() and re.search(r"^\s*key\s*:", h.football_cfg.read_text(encoding="utf-8"), re.M):
        raise X4Error(f"not sent: {h.football_cfg} holds the football key", [f"move it into {h.football_key}, one line"])
    _git(h, "add", "-A")
    staged = _git(h, "diff", "--cached", "--quiet").returncode != 0
    if not staged and not _ahead(h):
        return {"sent": False}
    if staged:
        r = _git(h, "commit", "--quiet", "-m", what)
        if r.returncode:
            raise X4Error("could not commit to the data repo's copy", _said(r))
    upstream = "origin/" + _git(h, "branch", "--show-current").stdout.strip()
    for _ in range(4):
        r = _git(h, "push", "--quiet", "origin", "HEAD")
        if r.returncode == 0:
            return {"sent": True}
        if _git(h, "fetch", "--quiet", "origin").returncode:
            break
        # Every file the repo's new commits touched, also one added and removed again.
        theirs = _git(h, "log", "--name-only", "--format=", f"HEAD..{upstream}").stdout.split("\n")
        theirs = [name for name in theirs if name]
        if not theirs or any(not name.startswith("inbox/") for name in theirs):
            break  # another writer, or nothing new: the push failed for another reason
        if _git(h, "rebase", upstream).returncode:
            _git(h, "rebase", "--abort")
            break
    raise X4Error(
        "not sent: another writer changed the data repo, or it cannot be reached. What was written is committed here",
        _said(r) + [f"then: git -C {h.root} pull --rebase && git -C {h.root} push"],
    )


def _ahead(h: Home) -> bool:
    """True when a commit here is not in the repo yet, or the repo has no branch yet."""
    branch = _git(h, "branch", "--show-current").stdout.strip()
    r = _git(h, "rev-list", "--count", f"origin/{branch}..HEAD")
    return r.returncode != 0 or r.stdout.strip() not in ("", "0")
