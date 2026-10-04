#!/usr/bin/env python3
"""Annotate a Japanese EPUB with furigana, for books that ship without it.

The device renders <ruby> from the book itself and lets the Furigana toggle hide it, so readings
are added here, once, on a computer. A kanji's reading depends on the word and the sentence it
is in (今日 is きょう or こんにち, 行った is いった or おこなった), so they are read in context by
Gemini: the book's text is sent to it, sentence by sentence, under your own API key.

Usage:
    python3 add_furigana_ruby.py --ai --gemini-key-file gemini.key book.epub out.epub
    GEMINI_API_KEY=... python3 add_furigana_ruby.py --ai book.epub out.epub

Furigana the book already has is kept as it is, and nothing is added inside it. A reading is used
only when it is kana and fits the word as written: 食べる read たべる puts た over 食 and leaves
べる alone. Anything that does not fit is left bare rather than guessed at.
"""

import argparse
import html
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "ruby_common"))
import ruby_epub  # noqa: E402
from ruby_epub import text_pieces  # noqa: E402

KANJI = re.compile(r"[㐀-䶿一-鿿々〆ヶ\U00020000-\U0003134f]")
KANJI_RUN = re.compile(r"[㐀-䶿一-鿿々〆ヶ\U00020000-\U0003134f]+")
HIRAGANA = re.compile(r"[ぁ-ゖー]+")
MAX_KANA_PER_KANJI = 6  # 承る is うけたまわ: five for one kanji; more than this is not a reading
AI_BATCH_CHARS = 500  # per request
AI_PROMPT = (
    "For each numbered Japanese sentence below, list every word that contains kanji, in the order "
    "the words appear, with its reading in this context. Give each as a pair [word, reading]: the "
    "word exactly as written in the sentence, okurigana included, and the reading of that whole "
    "word in hiragana. Leave out words written only in kana, digits or Latin letters. Answer with "
    "a JSON array holding one array of pairs per sentence, in the order given, and nothing else.\n\n"
)


def to_hiragana(text: str) -> str:
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def align(word: str, reading: str):
    """[(start, end, kana)] for each kanji run of word, or None when reading does not fit it.

    The kana written in the word must appear in the reading where they stand: 取り引き read
    とりひき gives と over 取 and ひ over 引. A reading that cannot be laid over the word this way
    is not one for it."""
    reading = to_hiragana(reading.strip())
    if not HIRAGANA.fullmatch(reading):
        return None
    pattern = []
    runs = []
    pos = 0
    for m in KANJI_RUN.finditer(word):
        pattern.append(re.escape(to_hiragana(word[pos:m.start()])))
        pattern.append("(.+?)")
        runs.append(m)
        pos = m.end()
    if not runs:
        return None
    pattern.append(re.escape(to_hiragana(word[pos:])))
    fitted = re.fullmatch("".join(pattern), reading)
    if not fitted:
        return None
    out = []
    for m, kana in zip(runs, fitted.groups()):
        if len(kana) > MAX_KANA_PER_KANJI * (m.end() - m.start()):
            return None
        out.append((m.start(), m.end(), kana))
    return out


def contextual_furigana(passages: list, ask) -> dict:
    """{passage: [(start, end, kana), ...]} in order, from a model's [word, reading] pairs.
    ask(sentences) -> one list of pairs per sentence, or None.

    A pair is used only when its word is found in the sentence after the previous one and its
    reading fits (see align); the rest of the sentence is still annotated."""
    sentences = {}  # sentence -> [(passage, start), ...]
    for passage in dict.fromkeys(passages):
        for start, sentence in ruby_epub.split_sentences(passage):
            if KANJI.search(sentence):
                sentences.setdefault(sentence, []).append((passage, start))
    out = {}
    for batch in ruby_epub.batches(list(sentences), len, AI_BATCH_CHARS):
        answer = ask(batch)
        if answer is None:
            print(f"  No usable answer for {len(batch)} sentence(s); left without furigana", file=sys.stderr)
            continue
        for sentence, pairs in zip(batch, answer):
            cursor = 0
            spans = []
            for pair in pairs:
                if not (isinstance(pair, list) and len(pair) == 2 and all(isinstance(x, str) for x in pair)):
                    continue
                word, reading = pair
                at = sentence.find(word, cursor) if word else -1
                fitted = align(word, reading) if at >= 0 else None
                if not fitted:
                    continue
                spans.extend((at + s, at + e, kana) for s, e, kana in fitted)
                cursor = at + len(word)
            for passage, start in sentences[sentence]:
                out.setdefault(passage, []).extend((start + s, start + e, kana) for s, e, kana in spans)
    for spans in out.values():
        spans.sort()
    return out


def annotate_xhtml(doc: str, furigana: dict) -> str:
    out = []
    for piece, is_text in text_pieces(doc):
        text = html.unescape(piece) if is_text else ""
        spans = furigana.get(text) if is_text else None
        if not spans:
            out.append(piece)
            continue
        built = []
        pos = 0
        for start, end, kana in spans:
            if start < pos:
                continue  # overlaps the previous span: keep the first
            built.append(html.escape(text[pos:start], quote=False))
            built.append(f"<ruby>{html.escape(text[start:end])}<rt>{html.escape(kana)}</rt></ruby>")
            pos = end
        built.append(html.escape(text[pos:], quote=False))
        out.append("".join(built))
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("epub")
    ap.add_argument("output")
    ruby_epub.add_ai_arguments(ap, "read each word in context")
    args = ap.parse_args()
    if not args.ai:
        ap.error("furigana is read in context, which needs --ai (the book's text is sent to Gemini)")
    api_key = ruby_epub.api_key_from(args, ap)

    def annotate(name: str, doc: str) -> str:
        passages = [html.unescape(p) for p, is_text in text_pieces(doc) if is_text]
        furigana = contextual_furigana(passages, lambda batch: ruby_epub.ask_per_passage(batch, AI_PROMPT, api_key))
        print(f"  {name}: {sum(len(v) for v in furigana.values()):,} readings added")
        return annotate_xhtml(doc, furigana)

    annotated = ruby_epub.rewrite_epub(args.epub, args.output, annotate)
    print(f"Wrote {args.output}: {annotated} document(s) annotated")


if __name__ == "__main__":
    main()
