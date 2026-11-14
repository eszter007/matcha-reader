#!/usr/bin/env python3
"""Builds docs/anki/matcha-sentence.apkg from the template files beside it.

The package holds the "Matcha sentence" note type (fields in the sentence-mining CSV's
order) and one example card, so importing it into Anki adds the note type in one step.

Requires the Anki library: pip install anki
Usage: python3 tools/anki_template/build_apkg.py
"""
import os
import sys
import tempfile

from anki.collection import Collection, DeckIdLimit, ExportAnkiPackageOptions

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SRC = os.path.join(ROOT, "docs", "anki")
OUT = os.path.join(SRC, "matcha-sentence.apkg")
NOTETYPE = "Matcha sentence"
DECK = "Matcha"
# The CSV's columns after GUID, in order; Tags is not a field.
FIELDS = ["Word", "Reading", "Sentence", "Definition", "Book", "Author", "Date", "Dictionary", "Furigana"]
EXAMPLE = {
    "Word": "木漏れ日",
    "Reading": "こもれび",
    "Sentence": "窓の外では、<b>木漏れ日</b>が静かに揺れていた。",
    "Definition": "1. sunlight filtering through the leaves of trees",
    "Book": "Example",
    "Furigana": "木[こ]漏[も]れ 日[び]",
}


def read(name):
    with open(os.path.join(SRC, name), encoding="utf-8") as f:
        return f.read()


def main():
    with tempfile.TemporaryDirectory() as tmp:
        col = Collection(os.path.join(tmp, "collection.anki2"))
        mm = col.models
        nt = mm.new(NOTETYPE)
        for name in FIELDS:
            mm.add_field(nt, mm.new_field(name))
        template = mm.new_template("Card 1")
        template["qfmt"] = read("front.html")
        template["afmt"] = read("back.html")
        mm.add_template(nt, template)
        nt["css"] = read("style.css")
        mm.add(nt)
        nt = mm.by_name(NOTETYPE)

        deck_id = col.decks.id(DECK)
        note = col.new_note(nt)
        for key, value in EXAMPLE.items():
            note[key] = value
        note.tags = ["matcha", "example"]
        col.add_note(note, deck_id)

        options = ExportAnkiPackageOptions(with_scheduling=False, with_deck_configs=False, with_media=False,
                                           legacy=True)
        col.export_anki_package(out_path=OUT, options=options, limit=DeckIdLimit(deck_id))
        col.close()
    print(f"Wrote {os.path.relpath(OUT, ROOT)} ({os.path.getsize(OUT)} bytes)")


if __name__ == "__main__":
    sys.exit(main())
