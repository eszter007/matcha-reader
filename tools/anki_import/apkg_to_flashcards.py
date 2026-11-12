#!/usr/bin/env python3
"""Turns an Anki deck package (.apkg) into a flashcard deck for the reader, without any sync.

The reader cannot open an .apkg itself (it is a SQLite database, often zstd-compressed), so this
script does what a sync would: it renders every card the way Anki does, cleans the HTML for the
reader, and writes the same deck folder the sync writes:

    <out>/<deck id>/deck.json, cards.bin, style.css, <card id>q.xhtml, <card id>a.xhtml, images

Copy the folder into the SD card's system folder, as `.system/anki/<deck id>/` (or pass --sd with
the card's mount point and it is written there). Review progress then lives on the reader only.

Requires the Anki library and lxml:  pip install anki lxml pillow

Usage:
    python3 tools/anki_import/apkg_to_flashcards.py Kaishi.apkg --sd /Volumes/SDCARD
    python3 tools/anki_import/apkg_to_flashcards.py deck.apkg --out ./decks --deck "HSK 1"
"""
import argparse
import io
import json
import os
import re
import shutil
import struct
import sys
import tempfile

from anki.collection import Collection, ImportAnkiPackageRequest

MAX_CARDS = 2000            # as many as the reader keeps progress for
MAX_SIDE_BYTES = 40 * 1024  # one side, as stored (the sync's limit)
MAX_CSS = 32 * 1024
TOO_LARGE = 1               # cards.bin flag: shown as "too large" on the reader
DRAWABLE = (".jpg", ".jpeg", ".png", ".gif", ".bmp")
DROP_TAGS = {"script", "noscript", "audio", "video", "iframe", "object", "embed", "template", "svg", "style",
             "canvas", "math"}


def clean_side(html: str) -> str:
    """Anki's HTML as an XHTML fragment the reader's parser accepts, scripts and audio removed."""
    import lxml.html
    from lxml import etree

    html = re.sub(r"\[anki:play:[qa]:\d+\]|\[sound:[^\]]*\]", "", html)
    root = lxml.html.fragment_fromstring(html or "", create_parent="div")
    for el in list(root.iter()):
        if not isinstance(el.tag, str):  # comments, processing instructions
            if el.getparent() is not None:
                el.getparent().remove(el)
            continue
        cls = (el.get("class") or "").lower()
        if el.tag in DROP_TAGS or "material-icons" in cls or "material-symbols" in cls:
            el.getparent().remove(el)
            continue
        for attr in list(el.attrib):
            if attr.lower().startswith("on") or ":" in attr:
                del el.attrib[attr]
    for img in root.iter("img"):
        src = img.get("src") or ""
        if src.lower().endswith(".webp"):
            img.set("src", src + ".png")  # drawn from a PNG copy
    inner = etree.tostring(root, method="xml", encoding="unicode")
    return inner[len("<div>"):-len("</div>")] if inner.startswith("<div>") else inner


def styles_in(html: str) -> list:
    return re.findall(r"<style[^>]*>(.*?)</style>", html or "", flags=re.S | re.I)


def deck_query(name: str) -> str:
    """Anki search for a deck and its subdecks."""
    return '"deck:' + name.replace("\\", "\\\\").replace('"', '\\"').replace("*", "\\*").replace("_", "\\_") + '"'


def document(fragment: str) -> str:
    return ('<?xml version="1.0" encoding="utf-8"?><html xmlns="http://www.w3.org/1999/xhtml"><head><title/>'
            '</head><body><div class="card">' + fragment + "</div></body></html>")


def copy_image(media_dir: str, name: str, out_dir: str) -> None:
    """Copies a drawable image; WebP becomes a grayscale PNG sized for an e-ink screen."""
    if "/" in name or ".." in name or name.startswith("."):
        return
    original = name[:-4] if name.endswith(".webp.png") else name
    src = os.path.join(media_dir, original)
    dst = os.path.join(out_dir, name)
    if not os.path.isfile(src) or os.path.exists(dst):
        return
    if name.endswith(".webp.png"):
        try:
            from PIL import Image

            with Image.open(src) as im:
                im = im.convert("L")
                im.thumbnail((480, 480))
                im.save(dst, format="PNG", optimize=True)
        except Exception as error:  # noqa: BLE001 - a picture that cannot be read is left out
            print(f"  image {original} left out: {error}")
    elif name.lower().endswith(DRAWABLE):
        shutil.copyfile(src, dst)


def export_deck(col: Collection, deck_id: int, deck_name: str, out_root: str) -> int:
    out_dir = os.path.join(out_root, str(deck_id))
    os.makedirs(out_dir, exist_ok=True)
    card_ids = sorted(col.find_cards(deck_query(deck_name)))
    if len(card_ids) > MAX_CARDS:
        print(f"  {deck_name}: {len(card_ids)} cards, keeping the first {MAX_CARDS}")
        card_ids = card_ids[:MAX_CARDS]
    css_parts, records, too_large = [], [], 0
    for card_id in card_ids:
        card = col.get_card(card_id)
        note = card.note()
        flags = 0
        css = card.note_type().get("css", "")
        if css and css not in css_parts:
            css_parts.append(css)
        sides = []
        for html in (card.question(reload=True), card.answer()):
            for style in styles_in(html):
                if style not in css_parts:
                    css_parts.append(style)
            sides.append(document(clean_side(html)))
        if any(len(side.encode()) > MAX_SIDE_BYTES for side in sides):
            flags |= TOO_LARGE
            too_large += 1
        else:
            for suffix, side in zip(("q", "a"), sides):
                with open(os.path.join(out_dir, f"{card_id}{suffix}.xhtml"), "w", encoding="utf-8") as f:
                    f.write(side)
                for name in re.findall(r'src="([^"]+)"', side):
                    copy_image(col.media.dir(), name, out_dir)
        records.append(struct.pack("<QQII", card_id, note.id, int(note.mod), flags))
    with open(os.path.join(out_dir, "cards.bin"), "wb") as f:
        f.write(b"".join(records))
    css_text = "\n".join(css_parts)
    with open(os.path.join(out_dir, "style.css"), "w", encoding="utf-8") as f:
        f.write(css_text[:MAX_CSS])
    with open(os.path.join(out_dir, "deck.json"), "w", encoding="utf-8") as f:
        json.dump({"name": deck_name, "lastReviewId": 0}, f, ensure_ascii=False)
    note = f", {too_large} too large for the reader" if too_large else ""
    print(f"  {deck_name}: {len(records)} cards{note} -> {out_dir}")
    return len(records)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("apkg", help="the Anki package (.apkg / .colpkg)")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--sd", help="the SD card's mount point: written to <sd>/.system/anki/")
    target.add_argument("--out", help="a folder to write the deck folders to")
    parser.add_argument("--deck", action="append", default=[],
                        help="only this deck (and its subdecks), by name; may be repeated")
    args = parser.parse_args()

    out_root = args.out
    if args.sd:
        system = os.path.join(args.sd, "system") if os.path.isdir(os.path.join(args.sd, "system")) else \
            os.path.join(args.sd, ".system")
        out_root = os.path.join(system, "anki")
    os.makedirs(out_root, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        col = Collection(os.path.join(tmp, "collection.anki2"))
        col.import_anki_package(ImportAnkiPackageRequest(package_path=os.path.abspath(args.apkg)))
        wanted = args.deck
        decks = []
        for d in col.decks.all_names_and_ids():
            if d.name == "Default" and not col.find_cards(deck_query(d.name)):
                continue
            if wanted and not any(d.name == w or d.name.startswith(w + "::") for w in wanted):
                continue
            if not wanted and "::" in d.name:
                continue  # a whole package becomes its top-level decks, subdecks included
            if col.find_cards(deck_query(d.name)):
                decks.append((d.id, d.name))
        if not decks:
            print("No cards found" + (f" in {', '.join(wanted)}" if wanted else ""))
            return 1
        total = sum(export_deck(col, deck_id, name, out_root) for deck_id, name in decks)
        col.close()
    print(f"{total} cards written. Review them under Flashcards; nothing is synced.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
