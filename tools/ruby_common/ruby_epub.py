"""What the ruby annotators share: walking an EPUB's text, and asking Gemini about it.

add_pinyin_ruby.py and add_furigana_ruby.py both put <ruby> over a book's text on a computer,
because the device renders ruby but does not work readings out itself.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

GEMINI_MODEL = "gemini-3.8-flash"
GEMINI_URL = f"https://generativelanguage.googleapis.com/v1beta/models/{GEMINI_MODEL}:generateContent"

# Text that must not be touched: tags, existing ruby, scripts/styles, and the <head>.
SKIP_BLOCK = re.compile(r"(<(ruby|rt|rp|script|style|head|title)\b.*?</\2\s*>)", re.S | re.I)
# A tag ends at the first ">" outside a quoted attribute value: title="甲 > 乙" is one tag.
TAG_OR_TEXT = re.compile(r"""(<(?:[^>"']|"[^"]*"|'[^']*')*>)""")
_SENTENCE_END = "。！？!?…\n"


def text_pieces(doc: str):
    """(piece, is_text) over a document, in order; only is_text pieces may be annotated."""
    for block in SKIP_BLOCK.split(doc):
        if block is None or block.lower() in ("ruby", "rt", "rp", "script", "style", "head", "title"):
            continue
        if SKIP_BLOCK.fullmatch(block):
            yield block, False
            continue
        for piece in TAG_OR_TEXT.split(block):
            yield piece, bool(piece) and not piece.startswith("<")


def split_sentences(text: str, max_chars: int = 200):
    """(start, sentence) over text, cut after sentence punctuation (and the quotes that close
    it), or at max_chars where a passage has none. A model keeps count over a sentence; over a
    page it drifts, and one miscount costs every reading in the request."""
    out = []
    start = 0
    i = 0
    n = len(text)
    while i < n:
        end = i + 1
        if text[i] in _SENTENCE_END:
            while end < n and text[end] in _SENTENCE_END + "」』”’）)":
                end += 1
        elif end - start < max_chars:
            i += 1
            continue
        out.append((start, text[start:end]))
        start = i = end
    if start < n:
        out.append((start, text[start:]))
    return out


def batches(items: list, weight, limit: int):
    """Consecutive groups of items whose weights sum to at most limit (one item may exceed it)."""
    group, size = [], 0
    for item in items:
        w = weight(item)
        if group and size + w > limit:
            yield group
            group, size = [], 0
        group.append(item)
        size += w
    if group:
        yield group


def numbered(passages: list) -> str:
    return "\n".join(f"{i + 1}. {p}" for i, p in enumerate(passages))


def gemini_json(prompt: str, api_key: str, timeout: int = 120, retries: int = 3):
    """The model's JSON answer to prompt, parsed, or None after the retries."""
    # No temperature: Gemini 3.8 drops the sampling parameters, and a reading is checked against
    # the word anyway, so a varied answer cannot put a wrong one into the book.
    payload = {"contents": [{"parts": [{"text": prompt}]}],
               "generationConfig": {"responseMimeType": "application/json"}}
    for _ in range(retries):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tf:
            json.dump(payload, tf)
            payload_path = tf.name
        try:
            result = subprocess.run(
                ["curl", "-s", "-X", "POST", GEMINI_URL, "-H", "Content-Type: application/json",
                 "-H", f"x-goog-api-key: {api_key}", "-d", f"@{payload_path}"],
                capture_output=True, text=True, timeout=timeout)
        except (subprocess.SubprocessError, OSError):
            continue
        finally:
            os.unlink(payload_path)
        try:
            response = json.loads(result.stdout)
            if "error" in response:
                print(f"  Gemini: {response['error'].get('message', '')[:160]}", file=sys.stderr)
                continue
            return json.loads(response["candidates"][0]["content"]["parts"][0]["text"])
        except (KeyError, IndexError, TypeError, json.JSONDecodeError):
            continue
    return None


def ask_per_passage(passages: list, prompt: str, api_key: str):
    """One answer per passage, in order, or None when the model did not give exactly that."""
    answer = gemini_json(prompt + numbered(passages), api_key)
    if isinstance(answer, list) and len(answer) == len(passages) and all(isinstance(a, list) for a in answer):
        return answer
    return None


def add_ai_arguments(ap, what: str):
    ap.add_argument("--ai", action="store_true", help=f"{what} with Gemini (the book's text is sent to it)")
    ap.add_argument("--gemini-key-file", help="file holding the Gemini API key (default: $GEMINI_API_KEY)")


def api_key_from(args, ap) -> str:
    key = ""
    if args.gemini_key_file:
        with open(args.gemini_key_file, encoding="utf-8") as f:
            key = f.read().strip()
    key = key or os.environ.get("GEMINI_API_KEY", "").strip()
    if not key:
        ap.error("--ai needs a Gemini API key: --gemini-key-file or GEMINI_API_KEY")
    return key


def rewrite_epub(src: str, dst: str, annotate) -> int:
    """Copy src to dst, passing every XHTML document through annotate(name, doc) -> doc.
    Returns how many documents changed."""
    changed = 0
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w") as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "mimetype":
                zout.writestr(item, data, compress_type=zipfile.ZIP_STORED)
                continue
            if item.filename.lower().endswith((".xhtml", ".html", ".htm")):
                try:
                    doc = data.decode("utf-8")
                except UnicodeDecodeError:
                    zout.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED)
                    continue
                new = annotate(item.filename, doc)
                if new != doc:
                    changed += 1
                data = new.encode("utf-8")
            zout.writestr(item, data, compress_type=zipfile.ZIP_DEFLATED)
    return changed
