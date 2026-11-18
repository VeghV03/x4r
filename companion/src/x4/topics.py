"""Topic tags for news stories and feed entries: plain keyword rules, no model.

Edit the table to change what lands where. A word matches whole and case-blind; a trailing
`*` matches any ending (`kína*` finds kínai, Kínában); a word in capitals (EU, US) matches only
in capitals. `hosts` tag every item from or citing a domain. Hungarian text that names no topic
and no place in ABROAD is about Hungary: the domestic press's default subject. A feed's own
`topic` in feeds.yaml comes first.
"""

import re
from functools import cache
from urllib.parse import urlsplit

TOPICS: dict[str, dict[str, list[str]]] = {
    "hungary": {
        "words": [
            "magyar", "magyarok*", "magyarország*", "hungar*", "budapest*", "orbán*", "fidesz*",
            "tisza-párt*", "tisza párt*", "KDNP", "mi hazánk", "MSZP", "országgyűlés*",
            "alkotmánybíróság*", "kúria*", "MNB", "magyar nemzeti bank", "forint*", "szijjártó*",
            "magyar péter*", "lázár jános*", "rogán*", "varga mihály*", "sulyok tamás*",
            "debrecen*", "szeged*", "pécs*", "győr*", "miskolc*", "paks*",
        ],
        "hosts": ["magyarkozlony.hu", "kormany.hu", "parlament.hu", "njt.hu"],
    },
    "us": {
        "words": [
            "US", "U.S.", "USA", "united states", "amerikai*", "egyesült államok*", "trump*", "vance",
            "rubio", "washington*", "white house", "fehér ház*", "pentagon*", "congress*",
            "kongresszus*", "senate", "senator*", "supreme court", "republican*", "democrats",
            "democratic party", "republikánus*", "demokrata párt*", "GOP", "FBI", "CIA",
            "federal reserve", "the fed", "new york", "california", "texas",
        ],
        "hosts": ["whitehouse.gov", "congress.gov", "federalregister.gov", "supremecourt.gov"],
    },
    "eu": {
        "words": [
            "EU", "europe*", "európ*", "uniós", "brussels", "brüsszel*",
            "european commission", "európai bizottság*", "european parliament", "európai parlament*",
            "european council", "európai tanács*", "council of the EU", "von der leyen*", "kaja kallas",
            "antónio costa", "ECB", "EKB", "eurozone", "eurózón*", "euro area", "schengen*", "eurostat",
            "CJEU", "európai bíróság*", "court of justice of the eu*", "frontex", "OLAF", "EPPO",
            "európai ügyészség*", "rule of law", "jogállamiság*", "article 7", "hetes cikk*",
        ],
        "hosts": ["europa.eu"],
    },
    "palestine": {
        "words": [
            "palestin*", "palesztin*", "gaza*", "gáza*", "gázába*", "gázából", "gázától", "gázával", "west bank", "ciszjordán*", "hamas*", "hamász*",
            "rafah", "khan younis", "jenin", "dzsenin", "ramallah", "UNRWA", "occupied palestinian*",
            "east jerusalem", "kelet-jeruzsálem*", "fatah", "mahmoud abbas", "two-state", "kétállami*",
        ],
        "hosts": ["ochaopt.org", "unrwa.org"],
    },
    "china": {
        "words": [
            "china", "chinese", "kína*", "kínába*", "kínából", "kínának", "kínára", "kínáról", "kínától",
            "kínával", "PRC", "beijing", "peking*", "xi jinping*", "hszi csin-ping*",
            "li qiang", "li csiang", "wang yi", "communist party of china", "CCP", "CPC", "KKP",
            "taiwan*", "tajvan*", "hong kong*", "hongkong*", "shanghai", "sanghaj*", "shenzhen",
            "sencsen", "guangdong", "kuangtung", "xinjiang", "hszincsiang*", "uyghur*", "ujgur*",
            "tibet*", "yuan", "jüan*", "renminbi", "huawei", "BYD", "CATL", "alibaba", "tencent",
            "belt and road", "egy övezet, egy út", "NPC", "politburo", "politbüro*", "PLA",
            "people's liberation army", "népi felszabadító hadsereg*", "south china sea",
            "dél-kínai-tenger*",
        ],
        "hosts": ["gov.cn", "sixthtone.com", "caixinglobal.com", "thepaper.cn"],
    },
}  # fmt: skip

# Places and people abroad: Hungarian text naming one is not about Hungary by default.
ABROAD = [
    "külföld*", "orosz*", "moszkv*", "putyin*",
    "ukrajn*", "ukrán*", "kijev*", "zelenszkij*", "német*", "berlin*", "osztrák*", "ausztri*",
    "bécs*", "szlovák*", "pozsony*", "román*", "bukarest*", "szerb*", "belgrád*", "horvát*",
    "lengyel*", "varsó*", "cseh*", "prága*", "francia*", "párizs*", "brit*", "london*", "angol*",
    "olasz*", "róma*", "spanyol*", "madrid*", "török*", "isztambul*", "izrael*", "irán*",
    "japán*", "tokió*", "india*", "indiai*", "afrik*", "brazil*", "kanad*", "mexik*", "ausztrál*",
    "NATO", "ENSZ", "G7", "G20",
]  # fmt: skip


@cache
def _pattern(word: str) -> re.Pattern:
    prefix = word.endswith("*")
    body = re.escape(word.rstrip("*")).replace(r"\ ", r"\s+")
    flags = 0 if word == word.upper() else re.IGNORECASE
    return re.compile(rf"(?<![\w-]){body}{r'[\w-]*' if prefix else r'(?!\w)'}", flags)


def _hits(words: list[str], text: str) -> bool:
    return any(_pattern(w).search(text) for w in words)


def _host(url: str) -> str:
    return (urlsplit(url).hostname or "").removeprefix("www.")


def tag(text: str, lang: str = "", urls=(), extra=()) -> list[str]:
    """Topics for an item from its text (title, summary), language, URLs, and the topics it carries."""
    hosts = {_host(u) for u in urls}
    found = [
        topic
        for topic, rule in TOPICS.items()
        if _hits(rule["words"], text) or any(h == d or h.endswith("." + d) for h in hosts for d in rule["hosts"])
    ]
    if not found and lang == "hu" and not _hits(ABROAD, text):
        found = ["hungary"]
    return list(dict.fromkeys([t for t in extra if t] + found))
