"""Primary sources for the top news stories: the acts, rulings, statements and documents a
story rests on, so Claude can write the story from them rather than from the coverage.

Where they come from, in order:
- the engine's clusters: Közlöny (and other constitutive) items that discover put in the story;
- the source articles: links to official domains and to PDFs in the article body, and the
  acts the text cites by number (`345/2026. (X. 30.) Korm. rendelet`, `(EU) 2024/1689`);
- the engine's Közlöny index (`scripts/kozlony.mjs`, out/kozlony/acts.jsonl): cited acts get
  their title and issue, and the engine's claim clustering (engine/link.mjs) pairs acts with
  headlines that state the same thing;
- the engine's watched EU dossiers (data/eu-dossiers.json, `scripts/eurlex.mjs`): a headline
  naming one (GDPR, DSA, AI Act) gets the act and its latest movements.

Bounded: the top TOP stories, SOURCES_PER_STORY articles each, a time budget per step.
Every failure is a warning; the stories go out with what was found.
"""

import json
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor, wait
from datetime import date, timedelta
from pathlib import Path
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit

TOP = 8
SOURCES_PER_STORY = 3
MAX_PRIMARY = 6
FETCH_BUDGET = 90  # seconds for every article download together
KOZLONY_ISSUES = 3  # newest gazette issues kozlony.mjs reads per run
KOZLONY_TIMEOUT = 300
EURLEX_TIMEOUT = 180
LINK_TIMEOUT = 60
RECENT_DAYS = 30  # acts and dossier movements this recent are offered
NO_FETCH = {"24.hu"}
LINK = Path(__file__).parent / "engine" / "link.mjs"

# Host suffix -> kind; the first match wins. A host with a `gov` label (gov.uk, gov.il) is government.
OFFICIAL: list[tuple[str, str]] = [
    ("magyarkozlony.hu", "gazette"), ("njt.hu", "law"), ("kormany.hu", "government"),
    ("parlament.hu", "parliament"), ("alkotmanybirosag.hu", "court"), ("kuria-birosag.hu", "court"),
    ("birosag.hu", "court"), ("mnb.hu", "government"), ("ksh.hu", "government"),
    ("eur-lex.europa.eu", "law"), ("curia.europa.eu", "court"), ("europarl.europa.eu", "parliament"),
    ("europa.eu", "government"),
    ("federalregister.gov", "gazette"), ("congress.gov", "parliament"), ("supremecourt.gov", "court"),
    ("uscourts.gov", "court"), ("npc.gov.cn", "parliament"), ("court.gov.cn", "court"),
    ("icj-cij.org", "court"), ("icc-cpi.int", "court"), ("un.org", "international"),
    ("ochaopt.org", "international"), ("unrwa.org", "international"), ("who.int", "international"),
    ("xinhuanet.com", "state-media"), ("news.cn", "state-media"), ("globaltimes.cn", "state-media"),
    ("people.com.cn", "state-media"), ("chinadaily.com.cn", "state-media"),
]  # fmt: skip
CONSTITUTIVE = {"kozlony": "gazette", "mkogy": "parliament", "njt": "law", "eurlex": "law"}  # engine feed ids

HU_ACT = re.compile(
    r"(\d{1,4}/\d{4}\.\s*\([IVXLC]+\.\s*\d{1,2}\.\))\s*((?:Korm\.|[A-ZÁÉÍÓÖŐÚÜŰ][A-Za-zÁÉÍÓÖŐÚÜŰáéíóöőúüű]{1,5}\.?)\s*(?:rendelet|határozat))"
)
HU_LAW = re.compile(r"(\d{4})\.\s*évi\s+([IVXLCDM]+)\.\s*törvén")  # törvény, törvénnyel
EU_ACT = re.compile(
    r"(?:\b(Regulation|Directive|Decision)\s+)?\((?:EU|EC|EK)\)\s*(No\s*)?(\d{1,4})/(\d{1,4})(?:\s+(rendelet|irányelv|határozat))?"
)
EU_TYPE = {"regulation": "R", "rendelet": "R", "directive": "L", "irányelv": "L", "decision": "D", "határozat": "D"}
GENERIC_ANCHOR = {"itt", "here", "link", "this", "ez", "ezt", "ide", "source", "forrás", "pdf"}


def _host(url: str) -> str:
    return (urlsplit(url).hostname or "").lower().removeprefix("www.")


def kind_of(url: str) -> str | None:
    """The kind of primary source a URL is, or None for anything else."""
    host = _host(url)
    if not host:
        return None
    for suffix, kind in OFFICIAL:
        if host == suffix or host.endswith("." + suffix):
            return kind
    if "gov" in host.split("."):
        return "government"
    if urlsplit(url).path.lower().endswith(".pdf"):
        return "document"
    return None


def _roman(s: str) -> int:
    values = {"I": 1, "V": 5, "X": 10, "L": 50, "C": 100, "D": 500, "M": 1000}
    total = 0
    for a, b in zip(s, s[1:] + " "):
        v = values[a]
        total += -v if b != " " and values.get(b, 0) > v else v
    return total


def _norm(s: str) -> str:
    return re.sub(r"\s+", "", s).lower()


def _item(title: str, url: str, kind: str, day: str | None = None) -> dict:
    parts = urlsplit(url)  # without tracking parameters and fragment
    pairs = parse_qsl(parts.query, keep_blank_values=True)
    if any(k.startswith("utm_") for k, _ in pairs):
        parts = parts._replace(query=urlencode([(k, v) for k, v in pairs if not k.startswith("utm_")]))
    url = urlunsplit(parts._replace(fragment=""))
    out = {"title": " ".join(title.split())[:160], "url": url, "kind": kind}
    if day:
        out["date"] = day
    return out


# the article pages


def _page(url: str) -> tuple[list[tuple[str, str]], str]:
    """Links (href, anchor text) in the article body and its plain text."""
    import lxml.html
    import trafilatura

    from .convert import net

    raw = net.fetch(url)
    body = trafilatura.extract(raw, url=url, output_format="html", include_links=True, favor_recall=True)
    if not body:
        return [], ""
    root = lxml.html.fromstring(body)
    links = [(a.get("href", ""), " ".join(a.text_content().split())) for a in root.iter("a")]
    return links, root.text_content()


def _pages(stories: list[dict], warnings: list[str]) -> dict[int, list[tuple[str, list, str]]]:
    jobs = []
    for i, s in enumerate(stories):
        urls = [x["url"] for x in s.get("sources", []) if _host(x["url"]) not in NO_FETCH]
        jobs += [(i, u) for u in urls[:SOURCES_PER_STORY]]
    out: dict[int, list] = {}
    if not jobs:
        return out
    pool = ThreadPoolExecutor(max_workers=8)
    futures = {pool.submit(_page, u): (i, u) for i, u in jobs}
    done, _ = wait(futures, timeout=FETCH_BUDGET)
    pool.shutdown(wait=False, cancel_futures=True)
    failed = 0
    for f, (i, u) in futures.items():
        if f not in done or f.exception() is not None:
            failed += 1
            continue
        links, text = f.result()
        out.setdefault(i, []).append((u, links, text))
    if failed:
        warnings.append(f"primary: {failed} of {len(jobs)} source articles not read; their links are missing")
    return out


def _links(page_url: str, links: list[tuple[str, str]]) -> list[dict]:
    own = _host(page_url)
    out = []
    for href, text in links:
        if not href.startswith(("http://", "https://")) or _host(href) == own:
            continue
        parts = urlsplit(href)
        if parts.path.strip("/") == "" and not parts.query:
            continue  # a home page is not a document
        kind = kind_of(href)
        if kind:
            title = text if len(text) > 3 and text.lower() not in GENERIC_ANCHOR else _host(href) + parts.path
            out.append(_item(title, href.split("#")[0], kind))
    return out


# acts cited in the text


def _hu_citations(text: str, acts: dict[str, dict]) -> list[dict]:
    out = []
    for m in HU_ACT.finditer(text):
        number, typ = " ".join(m.group(1).split()), " ".join(m.group(2).split())
        act = acts.get(_norm(number) + "|" + _norm(typ))
        if act:
            out.append(_act(act))
    for m in HU_LAW.finditer(text):
        year, num = m.group(1), _roman(m.group(2))
        act = acts.get(_norm(f"{year}. évi {m.group(2)}.") + "|törvény")
        if act:
            out.append(_act(act))
        else:
            out.append(_item(f"{year}. évi {m.group(2)}. törvény", f"https://njt.hu/jogszabaly/{year}-{num}-00-00", "law"))
    return out


def _celex_url(celex: str) -> str:
    return f"https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:{celex}"


def _eu_citations(text: str, titles: dict[str, str]) -> list[dict]:
    out = []
    for m in EU_ACT.finditer(text):
        word = (m.group(1) or m.group(5) or "").lower()
        if word not in EU_TYPE:
            continue
        a, b = m.group(3), m.group(4)
        year, num = (b, a) if m.group(2) else (a, b)  # "No 1308/2013" is number/year
        if len(year) != 4:
            continue
        celex = f"3{year}{EU_TYPE[word]}{int(num):04d}"
        out.append(_item(titles.get(celex) or " ".join(m.group(0).split()), _celex_url(celex), "law"))
    return out


# the engine's records


def _act(act: dict) -> dict:
    title = f"{act.get('number', '')} {act.get('actType', '')} {act.get('title', '')}".strip()
    return _item(title, str(act.get("source", {}).get("url") or "https://magyarkozlony.hu/"), "gazette", act.get("issueDate"))


def _kozlony(root: Path, today: date, run, warnings: list[str]) -> dict[str, dict]:
    """The engine's Közlöny index after reading the newest issues, keyed by number|type."""
    issues = root / "out" / "kozlony"
    # kozlony.mjs keeps a PDF whose text it could not extract and skips it from then on; --force reads it again.
    unread = any(not pdf.with_suffix(".txt").exists() for pdf in issues.glob("*/MK-*.pdf"))
    try:
        run(root, "kozlony.mjs", [f"--limit={KOZLONY_ISSUES}"] + (["--force"] if unread else []), KOZLONY_TIMEOUT)
    except Exception as e:  # noqa: BLE001 - the index from earlier runs still helps
        warnings.append(f"primary: {e}")
    if any(not pdf.with_suffix(".txt").exists() for pdf in issues.glob("*/MK-*.pdf")):
        warnings.append("primary: Közlöny issues without text; install pdftotext (brew install poppler)")
    path = root / "out" / "kozlony" / "acts.jsonl"
    acts: dict[str, dict] = {}
    if not path.exists():
        return acts
    cutoff = (today - timedelta(days=365)).isoformat()
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            act = json.loads(line)
            if (act.get("issueDate") or cutoff) < cutoff:
                continue
            typ = "törvény" if "törvény" in act["actType"] else _norm(act["actType"])
            acts[_norm(act["number"]) + "|" + typ] = act
        except (ValueError, KeyError, TypeError, AttributeError):
            continue
    return acts


def _engine_links(root: Path, stories: list[dict], acts: list[dict], warnings: list[str]) -> set[tuple[int, str]]:
    """(story index, act id) pairs the engine's claim clustering puts together."""
    if not acts:
        return set()
    payload = {
        "stories": [{"id": i, "titles": [s["title"]] + [x["title"] for x in s.get("sources", []) if x.get("title")]} for i, s in enumerate(stories)],
        "acts": [{"id": a["id"], "title": a["title"]} for a in acts],
    }
    try:
        r = subprocess.run(
            ["node", str(LINK), str(root)], input=json.dumps(payload), capture_output=True, text=True, timeout=LINK_TIMEOUT
        )
        if r.returncode:
            raise RuntimeError(((r.stderr or "").strip().splitlines() or ["no output"])[-1][:200])
        return {(int(s), str(a)) for s, a in json.loads(r.stdout)["links"]}
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.SubprocessError) as e:
        warnings.append(f"primary: engine clustering failed ({e})")
        return set()


def _dossiers(root: Path) -> list[dict]:
    """The engine's watched EU dossiers with the names a headline may use."""
    try:
        data = json.loads((root / "data" / "eu-dossiers.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    out = []
    for d in data.get("dossiers") or []:
        name = str(d.get("nev") or "")
        aliases = {str(d.get("id") or "").replace("-", " "), name.split(" (")[0].split(" — ")[0]}
        aliases |= set(re.findall(r"\(([^)]+)\)", name))
        aliases = {a.strip() for a in aliases if len(a.strip()) >= 3}
        pattern = re.compile(r"(?<![\w-])(?:" + "|".join(map(re.escape, sorted(aliases, key=len, reverse=True))) + r")(?!\w)", re.I)
        out.append({"id": d.get("id"), "celex": d.get("celex"), "name": name, "match": pattern})
    return out


def _watch(root: Path, today: date) -> dict[str, list[dict]]:
    """Recent works citing each dossier's act, from the engine's eurlex watch log."""
    path = root / "out" / "eurlex" / "watch.jsonl"
    out: dict[str, list[dict]] = {}
    if not path.exists():
        return out
    cutoff = (today - timedelta(days=RECENT_DAYS)).isoformat()
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            rec = json.loads(line)
        except ValueError:
            continue
        if str(rec.get("date") or "") >= cutoff:
            out.setdefault(rec.get("dossier"), []).append(rec)
    for recs in out.values():
        recs.sort(key=lambda r: r["date"], reverse=True)
    return out


def _eu_titles(root: Path, dossiers: list[dict]) -> dict[str, str]:
    titles = {d["celex"]: d["name"] for d in dossiers if d.get("celex")}
    for f in (root / "out" / "eurlex").glob("3*.json"):
        try:
            titles.setdefault(f.stem, json.loads(f.read_text(encoding="utf-8"))["title"][:160])
        except (OSError, ValueError, KeyError, TypeError):
            continue
    return titles


def attach(stories: list[dict], root: Path | None, today: date, run) -> list[str]:
    """Add `primary` to the first TOP stories in place. `run(root, script, args, timeout)` runs an engine script."""
    top = stories[:TOP]
    warnings: list[str] = []
    if not top:
        return warnings
    found: list[list[dict]] = [list(s.get("primary", [])) for s in top]
    pages = _pages(top, warnings)

    acts: dict[str, dict] = {}
    dossiers: list[dict] = []
    titles: dict[str, str] = {}
    if root is not None:
        if any(x.get("lang") == "hu" for s in top for x in s.get("sources", [])):
            acts = _kozlony(root, today, run, warnings)
        dossiers = _dossiers(root)
        titles = _eu_titles(root, dossiers)

    for i, s in enumerate(top):
        for url, links, text in pages.get(i, []):
            found[i] += _links(url, links) + _hu_citations(text, acts) + _eu_citations(text, titles)

    if root is not None:
        recent = (today - timedelta(days=RECENT_DAYS)).isoformat()
        fresh = [a for a in acts.values() if (a.get("issueDate") or recent) >= recent]
        by_id = {a["id"]: a for a in fresh}
        for i, act_id in sorted(_engine_links(root, top, fresh, warnings)):
            found[i].append(_act(by_id[act_id]))
        named = [
            (i, d) for i, s in enumerate(top) for d in dossiers
            if d["match"].search(" ".join([s["title"]] + [x.get("title", "") for x in s.get("sources", [])]))
        ]  # fmt: skip
        if named:
            try:
                run(root, "eurlex.mjs", [], EURLEX_TIMEOUT)
            except Exception as e:  # noqa: BLE001 - the dossier act itself is still listed
                warnings.append(f"primary: {e}")
            watch = _watch(root, today)
            for i, d in named:
                found[i].append(_item(d["name"], _celex_url(d["celex"]), "law"))
                for rec in watch.get(d["id"], [])[:2]:
                    kind = "law" if str(rec["celex"]).startswith("3") else "document"
                    found[i].append(_item(rec.get("title") or rec["celex"], _celex_url(rec["celex"]), kind, rec["date"]))

    for s, items in zip(top, found):
        seen, unique = set(), []
        for item in items:
            key = item["url"].rstrip("/")
            if key not in seen:
                seen.add(key)
                unique.append(item)
        if unique:
            s["primary"] = unique[:MAX_PRIMARY]
        else:
            s.pop("primary", None)
    return warnings
