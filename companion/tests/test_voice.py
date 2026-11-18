from records import write_jsonl

from x4 import ops, voice

GUIDE = """# Voice

## Sentences

- `"` is a quote mark here too, but outside the heading it is no rule

## x4 refuses

- `"` write „…”
- ` - ` write ` – `
- `kulcsfontosságú` say what it decides
"""


def test_no_guide_refuses_nothing(h):
    write_jsonl(h.stream, [{"id": "s-1", "kind": "bite", "title": "T", "body": 'Ez "idézet".'}])
    assert voice.check(h) == []


def test_the_guide_lists_what_x4_refuses(h):
    (h.root / "voice.md").write_text(GUIDE)
    assert [r for r, _ in voice.rules(h)] == ['"', " - ", "kulcsfontosságú"]


def test_hungarian_texts_are_checked_and_other_peoples_words_are_not(h):
    (h.root / "voice.md").write_text(GUIDE)
    write_jsonl(h.stream, [
        {"id": "s-1", "kind": "bite", "title": "T", "body": 'Azt mondta, "majd meglátjuk".'},
        {"id": "s-2", "kind": "highlight", "title": "T", "body": 'He said "we will see".'},  # the book's words
        {"id": "s-3", "kind": "odd", "prompt": "Melyik lóg ki?", "options": ["a", "b - c"], "answer": 1, "why": "Kulcsfontosságú."},
    ])
    write_jsonl(h.threads, [{"id": "t-1", "title": "T", "body": "Hétfőn - ahogy várták - megszavazták.", "since": "2026-11-01", "updated": "2026-11-12"}])
    write_jsonl(h.read, [
        {"id": "r-1", "section": "world", "kind": "news", "url": "https://a/1", "head": 'A "nagy" vita', "lead": "L",
         "predict": {"prompt": "Nő vagy csökken?", "options": ["Nő", "Csökken"], "answer": 0, "quote": 'It "grew".'}},
        {"id": "r-2", "section": "world", "kind": "translation", "file": "articles/r-2.md", "head": "H", "lead": "L"},
        {"id": "r-3", "section": "world", "kind": "found", "file": "articles/r-3.md", "head": "H", "lead": "L"},
    ])
    (h.root / "articles").mkdir()
    (h.root / "articles/r-2.md").write_text('---\ntitle: "Cím"\nlang: hu\n---\n\nA lista:\n\n- első pont\n- második\n\nEgy mondat - kötőjellel.\n')
    (h.root / "articles/r-3.md").write_text('---\ntitle: "T"\nlang: en\n---\n\nAn English "quote" - fine.\n')
    errors = voice.check(h)
    where = [e.split(": '")[0] for e in errors]
    assert where == [
        f"{h.stream}:1: body",
        f"{h.stream}:3: options.1",
        f"{h.stream}:3: why",
        f"{h.threads}:1: body",
        f"{h.root}/articles/r-2.md:11",
    ]
    assert "'\"' in '…Azt mondta, \"majd meglátjuk\".…': write „…” (voice.md)" in errors[0]


def test_sync_refuses_what_the_guide_refuses(dev, h):
    (h.root / "voice.md").write_text(GUIDE)
    write_jsonl(h.previously, [{"book": "books/a.epub", "progress": 0.4, "text": "Ott tartasz - a második fejezetnél."}])
    _, _, errors = ops.check_issue(h)
    assert errors and errors[0].startswith(f"{h.previously}:1: text: ' - '")
