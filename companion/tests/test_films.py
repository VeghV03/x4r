import pytest
from records import write_jsonl

from x4 import films, ops
from x4.errors import InvalidData
from x4.models import check_read

SECTIONS = "Film: three arrivals a week\n"


def pick(i: str, title: str, year: int, season: int | None = None) -> dict:
    film = {"title": title, "year": year} | ({"season": season} if season else {})
    return {"id": i, "section": "film", "kind": "film", "file": f"articles/{i}.md", "head": title[:30], "lead": "Netflix, november 13-tól", "film": film}


def test_a_film_pick_says_what_it_is_and_only_a_pick_does(h):
    write_jsonl(h.read, [{"id": "r-1", "section": "film", "kind": "film", "file": "a.md", "head": "H", "lead": "L"}])
    with pytest.raises(InvalidData) as e:
        check_read(h.read)
    assert "film: a film pick (kind film) gives film" in e.value.details[0]
    write_jsonl(h.read, [{"id": "r-1", "section": "world", "kind": "news", "url": "https://a/1", "head": "H", "lead": "L", "film": {"title": "T", "year": 2026}}])
    with pytest.raises(InvalidData):
        check_read(h.read)


def test_seen_md_keeps_what_was_watched_out(h):
    h.seen_films.parent.mkdir(parents=True, exist_ok=True)
    h.seen_films.write_text("# watched\n\n- Black Doves (2024) season 1\nThe Paper (2025)\nAnora (2024)\nnot a line\n")
    reads = [ops.ReadSource.model_validate(p) for p in (
        pick("r-1", "Black Doves", 2024, 2),  # a season not yet seen
        pick("r-2", "black  doves", 2024, 1),
        pick("r-3", "The Paper", 2025, 2),  # the whole series is seen
        pick("r-4", "Anora", 2024),
        pick("r-5", "Anora", 2025),  # another film of the same name
    )]
    assert films.check(h, reads) == [
        f"{h.seen_films}:6: write 'Original title (year)', or 'Original title (year) season 2' for one season",
        f"{h.read}: r-2: film: black  doves (2024) season 1 is in seen.md; pick another",
        f"{h.read}: r-3: film: The Paper (2025) season 2 is in seen.md; pick another",
        f"{h.read}: r-4: film: Anora (2024) is in seen.md; pick another",
    ]


def test_sync_refuses_a_pick_already_seen(h):
    h.sections.parent.mkdir(parents=True, exist_ok=True)
    h.sections.write_text(SECTIONS)
    h.seen_films.write_text("Anora (2024)\n")
    write_jsonl(h.read, [pick("r-1", "Anora", 2024)])
    (h.root / "articles").mkdir()
    (h.root / "articles/r-1.md").write_text('---\ntitle: "Anora"\nlang: hu\n---\n\nSean Baker filmje.\n')
    _, _, errors = ops.check_issue(h)
    assert errors == [f"{h.read}: r-1: film: Anora (2024) is in seen.md; pick another"]
