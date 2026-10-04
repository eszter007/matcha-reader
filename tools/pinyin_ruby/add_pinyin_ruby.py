#!/usr/bin/env python3
"""Annotate a Chinese EPUB with pinyin ruby, so the reader shows it above the text like furigana.

The device renders <ruby> from the book itself and lets the Furigana toggle hide it, so pinyin
is added here, once, on a computer: every run of hanzi is segmented against CC-CEDICT by longest
match and each character gets its syllable as <rt>. Doing this on the device would compete with
page layout for the little RAM there is.

Usage:
    python3 add_pinyin_ruby.py --cedict cedict_1_0_ts_utf-8_mdbg.txt book.epub out.epub
    python3 add_pinyin_ruby.py --cedict cedict.u8 --frequency dict.txt --skip-top 1500 book.epub out.epub
    python3 add_pinyin_ruby.py --cedict cedict.u8 --zhuyin book.epub out.epub

--skip-top N leaves the N commonest words (by the --frequency list) bare, so only the words a
learner is likely to need are annotated. --zhuyin writes bopomofo instead of pinyin.
"""

import argparse
import html
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dict_convert"))
import convert_jmdict as conv  # noqa: E402

HAN = re.compile(r"[㐀-䶿一-鿿\U00020000-\U0003134f]")
MAX_WORD = 8


def load_cedict(path: str) -> dict:
    """{headword: [syllable, ...]} for both scripts; the first reading of a word wins."""
    words = {}
    opener = __import__("gzip").open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            m = conv._CEDICT_LINE_RE.match(line.rstrip("\n"))
            if not m:
                continue
            trad, simp, pinyin = m.group(1), m.group(2), m.group(3)
            syllables = [p for p in pinyin.split(" ") if p]
            if len(syllables) != len(trad):
                continue  # 儿化 and odd entries: one syllable per character is what ruby needs
            proper = conv.is_proper_noun_pinyin(pinyin)
            for hw in (trad, simp):
                # The everyday reading wins over a surname's or place's (長 cháng, not Zhǎng).
                held = words.get(hw)
                if held is None or (not proper and held[1]):
                    words[hw] = (syllables, proper)
    return {hw: held[0] for hw, held in words.items()}


def segment(text: str, words: dict):
    """Yield (chunk, syllables or None) over text, longest dictionary match first."""
    i = 0
    n = len(text)
    while i < n:
        if not HAN.match(text[i]):
            j = i
            while j < n and not HAN.match(text[j]):
                j += 1
            yield text[i:j], None
            i = j
            continue
        best = 0
        for length in range(min(MAX_WORD, n - i), 0, -1):
            if text[i:i + length] in words:
                best = length
                break
        if best == 0:
            yield text[i], None
            i += 1
        else:
            yield text[i:i + best], words[text[i:i + best]]
            i += best


def ruby_for(chunk: str, syllables: list, zhuyin: bool) -> str:
    parts = []
    for ch, syl in zip(chunk, syllables):
        reading = conv.pinyin_syllable_to_zhuyin(syl) if zhuyin else conv.pinyin_syllable_to_marks(syl)
        parts.append(f"<ruby>{html.escape(ch)}<rt>{html.escape(reading)}</rt></ruby>")
    return "".join(parts)


# Text that must not be touched: tags, existing ruby, scripts/styles, and the <head>.
SKIP_BLOCK = re.compile(r"(<(ruby|rt|rp|script|style|head|title)\b.*?</\2\s*>)", re.S | re.I)
TAG_OR_TEXT = re.compile(r"(<[^>]*>)")


def annotate_xhtml(doc: str, words: dict, skip: set, zhuyin: bool) -> str:
    out = []
    for block in SKIP_BLOCK.split(doc):
        if block is None or block in ("ruby", "rt", "rp", "script", "style", "head", "title"):
            continue
        if SKIP_BLOCK.fullmatch(block):
            out.append(block)
            continue
        for piece in TAG_OR_TEXT.split(block):
            if not piece or piece.startswith("<"):
                out.append(piece)
                continue
            text = html.unescape(piece)
            if not HAN.search(text):
                out.append(piece)
                continue
            built = []
            for chunk, syllables in segment(text, words):
                if syllables is None or chunk in skip:
                    built.append(html.escape(chunk, quote=False))
                else:
                    built.append(ruby_for(chunk, syllables, zhuyin))
            out.append("".join(built))
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("epub")
    ap.add_argument("output")
    ap.add_argument("--cedict", required=True, help="CC-CEDICT text file (.u8/.txt, optionally .gz)")
    ap.add_argument("--frequency", help="word list for --skip-top (jieba dict.txt, BCC, SUBTLEX)")
    ap.add_argument("--skip-top", type=int, default=0, help="leave the N commonest words unannotated")
    ap.add_argument("--zhuyin", action="store_true", help="bopomofo instead of pinyin")
    args = ap.parse_args()

    words = load_cedict(args.cedict)
    print(f"CC-CEDICT: {len(words):,} headwords")
    skip = set()
    if args.skip_top > 0:
        if not args.frequency:
            ap.error("--skip-top needs --frequency")
        ranked = sorted(conv.load_frequency(args.frequency).items(), key=lambda kv: -kv[1])
        skip = {w for w, _ in ranked[: args.skip_top]}
        print(f"Skipping the {len(skip):,} commonest words")

    annotated = 0
    with zipfile.ZipFile(args.epub) as zin, zipfile.ZipFile(args.output, "w") as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            lower = item.filename.lower()
            if item.filename == "mimetype":
                zout.writestr(item, data, compress_type=zipfile.ZIP_STORED)
                continue
            if lower.endswith((".xhtml", ".html", ".htm")):
                try:
                    doc = data.decode("utf-8")
                except UnicodeDecodeError:
                    zout.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED)
                    continue
                new = annotate_xhtml(doc, words, skip, args.zhuyin)
                if new != doc:
                    annotated += 1
                data = new.encode("utf-8")
            zout.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED)
    print(f"Wrote {args.output}: {annotated} document(s) annotated")


if __name__ == "__main__":
    main()
