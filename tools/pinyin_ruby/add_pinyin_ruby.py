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

--ai reads each passage in context with Gemini (GEMINI_API_KEY, or --gemini-key-file) to pick the
reading of a character that has several: 石 is shí in 石頭 and dàn as a measure, 了 is le or liǎo.
The dictionary alone takes a word's first listed reading. A reading the model gives is used only
when CC-CEDICT lists it for that character; anything else falls back to the dictionary's.
"""

import argparse
import html
import os
import re
import sys

_TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(_TOOLS, "dict_convert"))
sys.path.insert(0, os.path.join(_TOOLS, "ruby_common"))
import convert_jmdict as conv  # noqa: E402
import ruby_epub  # noqa: E402
from ruby_epub import text_pieces  # noqa: E402

HAN = re.compile(r"[㐀-䶿一-鿿\U00020000-\U0003134f]")
MAX_WORD = 8


def load_cedict(path: str, char_readings: dict = None) -> dict:
    """{headword: [syllable, ...]} for both scripts; the first reading of a word wins.
    char_readings, when given, collects every reading CC-CEDICT has for each character, in any
    word: {char: {"shi2", "dan4"}}. It is what a model's answer is checked against."""
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
            if char_readings is not None:
                for hw in (trad, simp):
                    for ch, syl in zip(hw, syllables):
                        char_readings.setdefault(ch, set()).add(normalize_syllable(syl))
            for hw in (trad, simp):
                # The everyday reading wins over a surname's or place's (長 cháng, not Zhǎng).
                held = words.get(hw)
                if held is None or (not proper and held[1]):
                    words[hw] = (syllables, proper)
    return {hw: held[0] for hw, held in words.items()}


def normalize_syllable(syl: str) -> str:
    """One spelling per reading: lowercase, ü as "u:" (CC-CEDICT's), neutral tone as 5."""
    syl = syl.strip().lower().replace("ü", "u:").replace("v", "u:")
    if syl and not syl[-1].isdigit():
        syl += "5"
    return syl


# ── Readings in context (--ai) ─────────────────────────────────

AI_BATCH_HANZI = 600  # per request
AI_PROMPT = (
    "For each numbered passage of Chinese below, give the Hanyu Pinyin of every Chinese character "
    "as it is read in this context, in order. Write each syllable with a tone number 1-5 after it "
    "(5 for the neutral tone) and u: for ü, for example ni3 hao3, lu:4. One syllable per Chinese "
    "character; skip punctuation, digits and Latin letters. Answer with a JSON array holding one "
    "array of syllables per passage, in the order given, and nothing else.\n\n"
)


def contextual_readings(passages: list, char_readings: dict, ask) -> dict:
    """{passage: {index of a hanzi in it: syllable}} for the readings a model gave AND the
    dictionary knows for that character. ask(sentences) -> list of syllable lists, or None.

    Passages are asked about sentence by sentence. A sentence whose answer has the wrong number
    of syllables is dropped whole: once the count is off there is no telling which syllable
    belongs to which character."""
    sentences = {}  # sentence -> [(passage, start), ...]
    for passage in dict.fromkeys(passages):
        for start, sentence in ruby_epub.split_sentences(passage):
            if HAN.search(sentence):
                sentences.setdefault(sentence, []).append((passage, start))
    out = {}
    for batch in ruby_epub.batches(list(sentences), lambda t: len(HAN.findall(t)), AI_BATCH_HANZI):
        answer = ask(batch)
        if answer is None:
            print(f"  No usable answer for {len(batch)} sentence(s); dictionary readings kept", file=sys.stderr)
            continue
        for sentence, syllables in zip(batch, answer):
            positions = [i for i, ch in enumerate(sentence) if HAN.match(ch)]
            if len(syllables) != len(positions):
                continue
            for pos, syl in zip(positions, syllables):
                syl = normalize_syllable(str(syl))
                if syl in char_readings.get(sentence[pos], ()):
                    for passage, start in sentences[sentence]:
                        out.setdefault(passage, {})[start + pos] = syl
    return out


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
        syl = syl.lower()  # CC-CEDICT capitalises proper nouns; ruby does not
        reading = conv.pinyin_syllable_to_zhuyin(syl) if zhuyin else conv.pinyin_syllable_to_marks(syl)
        parts.append(f"<ruby>{html.escape(ch)}<rt>{html.escape(reading)}</rt></ruby>")
    return "".join(parts)


def annotate_xhtml(doc: str, words: dict, skip: set, zhuyin: bool, contextual: dict = None) -> str:
    """contextual: {text: {index: syllable}} from contextual_readings(), overriding the
    dictionary's reading character by character."""
    out = []
    for piece, is_text in text_pieces(doc):
        if not is_text:
            out.append(piece)
            continue
        text = html.unescape(piece)
        if not HAN.search(text):
            out.append(piece)
            continue
        override = (contextual or {}).get(text, {})
        built = []
        offset = 0
        for chunk, syllables in segment(text, words):
            if chunk in skip or (syllables is None and not any(offset + k in override for k in range(len(chunk)))):
                built.append(html.escape(chunk, quote=False))
            elif syllables is None:
                # A character the dictionary has no word for here, which the model read.
                built.append(ruby_for(chunk, [override[offset]], zhuyin))
            else:
                built.append(ruby_for(chunk, [override.get(offset + k, syl) for k, syl in enumerate(syllables)], zhuyin))
            offset += len(chunk)
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
    ruby_epub.add_ai_arguments(ap, "pick each character's reading in context")
    args = ap.parse_args()
    api_key = ruby_epub.api_key_from(args, ap) if args.ai else ""

    char_readings = {} if args.ai else None
    words = load_cedict(args.cedict, char_readings)
    print(f"CC-CEDICT: {len(words):,} headwords")
    skip = set()
    if args.skip_top > 0:
        if not args.frequency:
            ap.error("--skip-top needs --frequency")
        ranked = sorted(conv.load_frequency(args.frequency).items(), key=lambda kv: -kv[1])
        skip = {w for w, _ in ranked[: args.skip_top]}
        print(f"Skipping the {len(skip):,} commonest words")

    def annotate(name: str, doc: str) -> str:
        contextual = None
        if args.ai:
            passages = [html.unescape(p) for p, is_text in text_pieces(doc) if is_text]
            contextual = contextual_readings(
                passages, char_readings, lambda batch: ruby_epub.ask_per_passage(batch, AI_PROMPT, api_key))
            print(f"  {name}: {sum(len(v) for v in contextual.values()):,} readings confirmed in context")
        return annotate_xhtml(doc, words, skip, args.zhuyin, contextual)

    annotated = ruby_epub.rewrite_epub(args.epub, args.output, annotate)
    print(f"Wrote {args.output}: {annotated} document(s) annotated")


if __name__ == "__main__":
    main()
