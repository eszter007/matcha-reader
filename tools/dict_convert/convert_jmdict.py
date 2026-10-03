#!/usr/bin/env python3
"""Convert dictionary files to binary index files for CrossPoint Reader.

Supported input formats:
  Japanese (--lang ja, the default)
  - jmdict-simplified JSON (.json / .json.tgz)
  - Yomitan/Yomichan (.zip containing term_bank_N.json)
  - MDict (.mdx) — requires: pip install readmdict
  Chinese (--lang zh)
  - CC-CEDICT raw text (.u8 / .txt: "繁體 简体 [pin1 yin1] /gloss/gloss/")
  - MoE 重編國語辭典 as the g0v moedict JSON (dict-revised.json, also .json.xz)
  - Yomitan .zip (e.g. the CC-CEDICT Yomitan build) and MDict .mdx, as above

Emits (basename set by --name, default "vocab"):
  vocab.idx   -- sorted array of 40-byte records (headword + offset + length + priority)
  vocab.dat   -- variable-length definition text blob
  vocab.title -- the dictionary's display name (Chinese, or when --title is given)

Usage:
#JMdict(default — downloads if no-- input given)
    python3 convert_jmdict.py [--input jmdict-eng-3.5.0.json] [--output-dir ./output]

#Yomitan dictionary zip
    python3 convert_jmdict.py --input jmdict-yomitan.zip --output-dir ./output

#MDict.mdx file
    python3 convert_jmdict.py --input dictionary.mdx --output-dir ./output

#Chinese : CC - CEDICT, ranked by a frequency list, with zhuyin beside the pinyin
    python3 convert_jmdict.py --lang zh --input cedict_1_0_ts_utf-8_mdbg.txt \
        --frequency dict.txt --zhuyin --output-dir /sd/dictionaries/zh/

#Chinese : CC - CEDICT and the MoE dictionary merged into one vocabulary file
    python3 convert_jmdict.py --lang zh --input cedict_ts.u8 --input dict-revised.json.xz \
        --output-dir /sd/dictionaries/zh/
"""

import argparse
import json
import math
import os
import re
import struct
import sys
import urllib.request

JMDICT_URL = "https://github.com/scriptin/jmdict-simplified/releases/latest/download/jmdict-eng-3.5.0.json.tgz"
HEADWORD_SIZE = 32
RECORD_FORMAT = f"<{HEADWORD_SIZE}sIHBB"  # headword(32) + offset(4) + length(2) + priority(1) + pad(1)
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)

assert RECORD_SIZE == 40, f"Record size mismatch: {RECORD_SIZE}"

# ── Shared helpers ──────────────────────────────────────────────


def strip_html(html: str) -> str:
    """Strip HTML tags and decode common entities to plain text."""
    text = re.sub(r"<br\s*/?>", "\n", html, flags=re.IGNORECASE)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("&amp;", "&")
    text = text.replace("&lt;", "<")
    text = text.replace("&gt;", ">")
    text = text.replace("&quot;", '"')
    text = text.replace("&nbsp;", " ")
    text = text.replace("&#x27;", "'")
    text = text.replace("&#39;", "'")
    lines = [line.strip() for line in text.split("\n")]
    return "\n".join(line for line in lines if line)


def write_binary(records: list, output_dir: str, name: str = "vocab", title: str = ""):
    """Write (headword_bytes, definition_bytes, priority, pos_flags) tuples to idx+dat files.

    Expects records to be pre-validated (headword_bytes < HEADWORD_SIZE). A non-empty title is
    written to <name>.title, which the device shows in the lookup panel's footer.
    """
    records.sort(key=lambda r: r[0])

    os.makedirs(output_dir, exist_ok=True)
    dat_path = os.path.join(output_dir, f"{name}.dat")
    idx_path = os.path.join(output_dir, f"{name}.idx")

    dat_offset = 0
    index_entries = []

    with open(dat_path, "wb") as dat_f:
        # One copy per distinct definition: a Chinese entry is indexed under both its traditional
        # and simplified form, which sort far apart, and a Japanese one under kanji and kana.
        stored = {}

        for hw_bytes, def_bytes, priority, pos_flags in records:
            if len(def_bytes) > 0xFFFF:
                def_bytes = def_bytes[:0xFFFF]
            hit = stored.get(def_bytes)
            if hit is None:
                hit = (dat_offset, len(def_bytes))
                dat_f.write(def_bytes)
                dat_offset += len(def_bytes)
                stored[def_bytes] = hit
            offset, length = hit

            padded_hw = hw_bytes + b"\x00" * (HEADWORD_SIZE - len(hw_bytes))
            index_entries.append((padded_hw, offset, length, priority, pos_flags))

    with open(idx_path, "wb") as idx_f:
        for padded_hw, offset, length, priority, pos_flags in index_entries:
            idx_f.write(struct.pack(RECORD_FORMAT, padded_hw, offset, length, priority, pos_flags))

    title_path = os.path.join(output_dir, f"{name}.title")
    # The device reads the first 39 bytes; keep whole characters within that.
    title = title.strip().encode("utf-8")[:36].decode("utf-8", errors="ignore").strip()
    if title:
        with open(title_path, "w", encoding="utf-8") as title_f:
            title_f.write(title + "\n")
    elif os.path.exists(title_path):
        os.remove(title_path)  # a stale name from an earlier conversion would mislabel this one

    dat_size = os.path.getsize(dat_path)
    idx_size = os.path.getsize(idx_path)
    print(f"Output:")
    print(f"  {idx_path}: {idx_size:,} bytes ({len(index_entries):,} records)")
    print(f"  {dat_path}: {dat_size:,} bytes")
    if title:
        print(f"  {title_path}: {title}")
    print(f"  Total: {(idx_size + dat_size) / 1024 / 1024:.1f} MB")

# ── JMdict(jmdict - simplified JSON) ─────────────────────────────


def download_jmdict(output_path: str) -> str:
    """Download and extract jmdict-simplified JSON."""
    import tarfile

    tgz_path = output_path + ".tgz"
    if not os.path.exists(tgz_path):
        print(f"Downloading {JMDICT_URL}...")
        urllib.request.urlretrieve(JMDICT_URL, tgz_path)
        print(f"Downloaded to {tgz_path}")

    json_path = None
    with tarfile.open(tgz_path, "r:gz") as tar:
        for member in tar.getmembers():
            if member.name.endswith(".json"):
                tar.extract(member, os.path.dirname(output_path))
                json_path = os.path.join(os.path.dirname(output_path), member.name)
                break

    if not json_path:
        print("ERROR: No JSON file found in tarball", file=sys.stderr)
        sys.exit(1)

    return json_path


def compute_priority_jmdict(entry: dict) -> int:
    """Map JMdict priority tags to a 0-255 score (higher = more common)."""
    is_common = False
    for kanji in entry.get("kanji", []):
        if kanji.get("common", False):
            is_common = True
            break
    if not is_common:
        for kana in entry.get("kana", []):
            if kana.get("common", False):
                is_common = True
                break
    return 200 if is_common else 100

#Part - of - speech flag bits-- must mirror DictIndexRecord::POS_* in lib / Dict / DictIndex.h.
# 0 means "no POS data" and the firmware then accepts every deinflection candidate, so leaving
#flags unset is always safe(fail open).
POS_V1 = 0x01     # ichidan verb
POS_V5 = 0x02     # godan verb
POS_VS = 0x04     # suru verb
POS_VK = 0x08     # kuru verb
POS_ADJ_I = 0x10  # i-adjective
POS_OTHER = 0x20  # tagged, but none of the above (noun, particle, na-adjective, ...)
POS_READING = 0x40  # kana READING record of an entry that has kanji headwords (not a kana lemma)
POS_ANY_VERB = POS_V1 | POS_V5 | POS_VS | POS_VK


def pos_flags_from_tags(tags) -> int:
    """Map JMdict partOfSpeech tags / Yomitan rules to POS flag bits.

    Prefix-matches the verb classes so subtags stay covered (v5k-s, v5aru, v1-s, vs-i, adj-ix).
    A verb-ish tag with an unrecognized class fails OPEN (all verb bits) rather than closed --
    a wrongly-rejected real conjugation is worse than letting a rare archaic verb through.
    Transitivity tags (vt/vi) say nothing about conjugation class and are ignored.
    """
    flags = 0
    for t in tags:
        if not t:
            continue
        if t in ("vt", "vi", "aux", "aux-adj", "exp"):
            continue  # not a conjugation class
        if t.startswith("v1"):
            flags |= POS_V1
        elif t.startswith("v5") or t.startswith("v4") or t.startswith("iv"):
            flags |= POS_V5  # v4* (archaic yodan) conjugates closest to godan for our rule set
        elif t.startswith("vs"):
            flags |= POS_VS
        elif t.startswith("vk"):
            flags |= POS_VK
        elif t.startswith("adj-i"):
            flags |= POS_ADJ_I
        elif t.startswith("v") or t == "aux-v":
            flags |= POS_ANY_VERB  # unknown verb subtype: fail open across verb classes
        else:
            flags |= POS_OTHER
    return flags


def pos_flags_jmdict(entry: dict) -> int:
    tags = []
    for sense in entry.get("sense", []):
        tags.extend(sense.get("partOfSpeech", []))
    return pos_flags_from_tags(tags)

def format_definition_jmdict(entry: dict) -> str:
    """Format an entry's readings and glosses into a compact display string."""
    parts = []

    readings = [k["text"] for k in entry.get("kana", [])]
    if readings:
        parts.append("【" + "、".join(readings[:3]) + "】")

    senses = entry.get("sense", [])
    for i, sense in enumerate(senses[:3]):
        glosses = [g["text"] for g in sense.get("gloss", [])]
        if glosses:
            prefix = f"{i+1}. " if len(senses) > 1 else ""
            parts.append(prefix + "; ".join(glosses[:4]))

    return "\n".join(parts)


def convert_jmdict(json_path: str) -> list:
    """Convert JMdict JSON to index records."""
    print(f"Loading {json_path}...")
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    words = data.get("words", [])
    print(f"Processing {len(words)} JMdict entries...")

    records = []
    for entry in words:
        definition = format_definition_jmdict(entry)
        def_bytes = definition.encode("utf-8")
        priority = compute_priority_jmdict(entry)
        pos_flags = pos_flags_jmdict(entry)

        seen_headwords = set()
        for kanji in entry.get("kanji", []):
            hw = kanji["text"]
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE or hw_bytes in seen_headwords:
                continue
            seen_headwords.add(hw_bytes)
            records.append((hw_bytes, def_bytes, priority, pos_flags))

#Kana records of an entry that also has kanji headwords are READING records : text that
#matches them is usually conjugation morphology, not the word(しながら vs 品柄).The
#firmware suppresses uncommon flagged records in hiragana segmentation.Entries with no
#kanji form at all are kana LEMMAS-- their kana record is the word itself, unflagged.
        kana_flags = pos_flags | (POS_READING if entry.get("kanji") else 0)
        for kana in entry.get("kana", []):
            hw = kana["text"]
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE or hw_bytes in seen_headwords:
                continue
            seen_headwords.add(hw_bytes)
            records.append((hw_bytes, def_bytes, priority, kana_flags))

    print(f"Generated {len(records)} index records")
    return records

# ── Yomitan / Yomichan(.zip) ───────────────────────────────────


def flatten_structured_content(content) -> str:
    """Recursively extract display text from Yomitan structured content,
    using the semantic data-content attributes from Jitendex."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(flatten_structured_content(item) for item in content)
    if isinstance(content, dict):
        ctype = content.get("type", "")
        if ctype == "text":
            return content.get("text", "")
        if ctype == "image":
            return ""
        if ctype == "structured-content":
            return flatten_structured_content(content.get("content", ""))

        inner = content.get("content", "")
        tag = content.get("tag", "")
        data = content.get("data", {}) if isinstance(content.get("data"), dict) else {
}
        dc = data.get("content", "")
        cls = data.get("class", "")
        text = flatten_structured_content(inner)

        if tag == "br":
            return "\n"
        if tag == "rt":
            return ""
        if tag == "ruby":
            return text

#All tag - class spans → [noun][math][colloquial] etc.inline
        if cls == "tag" and dc in ("part-of-speech-info", "field-info", "misc-info",
                                   "dialect-info", "language-info"):
            return "[" + text + "] "
        if cls == "tag" and dc == "forms-label":
            return ""

#Glossary list items — newline before to separate from POS tags
        if dc == "glossary" and tag == "ul":
            return "\n" + text
        if tag == "li" and not dc:
            return "• " + text.strip() + "\n"

#Sense groups
        if dc == "sense-group" and tag in ("li", "div"):
            return text + "\n"
        if dc == "sense" and tag == "li":
            return text

#Notes — indented with arrow
        if dc == "sense-note-label":
            return text + ": "
        if dc == "sense-note-content":
            return text + "\n"
        if dc == "sense-note" and cls == "extra-box":
            return "  → " + text

#Example sentences — each part on its own line, indented
        if dc == "example-sentence-a":
            return text + "\n"
        if dc == "example-sentence-b":
            return text + "\n"
        if dc == "example-sentence" and cls == "extra-box":
            return "  " + text
        if dc == "example-keyword":
            return text

#Cross - references
        if dc == "xref" and cls == "extra-box":
            return ""
        if dc == "reference-label":
            return text + " "

#Other forms — skip to save space
        if dc == "forms":
            return ""

#Attribution
        if dc == "attribution-footnote":
            return ""

#Generic block elements
        if tag in ("div", "p", "blockquote", "section"):
            if dc == "extra-info":
                return text
            return text
        if tag in ("ol", "ul"):
            return text

        return text
    return str(content)


def format_definition_yomitan(headword: str, reading: str, definitions) -> str:
    """Format a Yomitan entry's reading + definitions into display string."""
    parts = []
#Only show reading if it differs from headword(skip for kana - only entries)
    if reading and reading != headword:
        parts.append(f"【{reading}】")

    def flatten_list_defn(d) -> str:
#Yomitan list - form definitions(variant / redirect entries) like
#["引っ張り上げる", ["redirected from 引っぱり上げる"]].Join the string
#parts; drop "redirected from ..." cross - reference noise.
        if isinstance(d, str):
            if d.startswith("redirected from"):
                return ""
            return d
        if isinstance(d, dict):
            return flatten_structured_content(d)
        if isinstance(d, list):
            pieces = [flatten_list_defn(x) for x in d]
            return " ".join(p for p in pieces if p)
        return ""

    if isinstance(definitions, list):
        non_empty = []
        for defn in definitions[:6]:
            if isinstance(defn, str):
                text = defn
            elif isinstance(defn, dict):
                text = flatten_structured_content(defn)
            elif isinstance(defn, list):
                text = flatten_list_defn(defn)
            else:
                text = str(defn)
            text = text.strip()
            if text:
                non_empty.append(text)
#Drop pure - duplicate entries(redirect variant repeating the first gloss)
        deduped = []
        for t in non_empty:
            if t not in deduped:
                deduped.append(t)
        non_empty = deduped

        for i, text in enumerate(non_empty):
            if len(non_empty) > 1:
                parts.append(f"\n{i+1}. {text}")
            else:
                parts.append(f"\n{text}")

    result = "\n".join(parts)
    result = re.sub(r'[ \t]+', ' ', result)
    result = re.sub(r'\n{3,}', '\n\n', result)
    result = re.sub(r'• • ', '• ', result)
    return result.strip()


def find_redirect_target(definitions) -> str:
    """If a Yomitan entry is purely a redirect (variant spelling pointing to a
    canonical headword via a 'redirect-glossary'), return the target headword.
    Otherwise return ''."""
    def search(node):
        if isinstance(node, dict):
            data = node.get("data")
            if isinstance(data, dict) and data.get("content") == "redirect-glossary":
#The target headword is the link text(strip the ⟶ arrow).
                txt = flatten_structured_content(node).replace("⟶", "").strip()
                return txt
            return search(node.get("content"))
        if isinstance(node, list):
            for x in node:
                r = search(x)
                if r:
                    return r
        return ""
    return search(definitions)


def convert_yomitan(zip_path: str, reading_records: bool = True) -> tuple:
    """Convert a Yomitan/Yomichan .zip dictionary to index records.

    reading_records=False skips the extra record per kana reading. Chinese dictionaries carry
    pinyin in the reading field, which no page text ever matches, so the records would only
    bloat the index. Returns (records, title).
    """
    import zipfile

    print(f"Loading {zip_path}...")
    title = ""

    with zipfile.ZipFile(zip_path, "r") as z:
        names = z.namelist()

        if "index.json" in names:
            with z.open("index.json") as f:
                meta = json.load(f)
            title = str(meta.get("title", ""))
            print(f"  Dictionary: {title or '(unknown)'}")
            print(f"  Format version: {meta.get('format', meta.get('version', '?'))}")

        term_banks = sorted(n for n in names if re.match(r"term_bank_\d+\.json$", n))
        if not term_banks:
            print("ERROR: No term_bank_N.json files found in zip", file=sys.stderr)
            sys.exit(1)

        print(f"  Found {len(term_banks)} term bank files")

#Pass 1 : load all entries; build a headword → best definition map for
#non - redirect entries so variant / redirect entries can be resolved.
        all_entries = []
        canonical_defs = {}  # headword → (definition_string, priority)
        for bank_name in term_banks:
            with z.open(bank_name) as f:
                entries = json.load(f)
            for entry in entries:
                if not isinstance(entry, list) or len(entry) < 6:
                    continue
                headword = entry[0]
                if not headword or not isinstance(headword, str):
                    continue
                reading = entry[1] if len(entry) > 1 else ""
                rules = entry[3] if len(entry) > 3 and isinstance(entry[3], str) else ""
                score = entry[4] if len(entry) > 4 else 0
                definitions = entry[5] if len(entry) > 5 else []
                redirect = find_redirect_target(definitions)
                all_entries.append((headword, reading, score, definitions, redirect, rules))
                if not redirect:
                    definition = format_definition_yomitan(headword, reading, definitions)
                    if definition:
                        priority = max(0, min(255, int(score) + 128)) if isinstance(score, (int, float)) else 100
                        prev = canonical_defs.get(headword)
                        if prev is None or priority > prev[1]:
                            canonical_defs[headword] = (definition, priority)

#Pass 2 : emit records, resolving redirects to the target's real definition.
        records = []
        entry_count = 0
        for headword, reading, score, definitions, redirect, rules in all_entries:
            if redirect:
                target = canonical_defs.get(redirect)
                if not target:
                    continue  # dangling redirect — skip the useless circular entry
#Show the canonical spelling note + the real definition.
                definition = f"= {redirect}\n{target[0]}"
                priority = target[1]
            else:
                definition = format_definition_yomitan(headword, reading, definitions)
                if not definition:
                    continue
                priority = max(0, min(255, int(score) + 128)) if isinstance(score, (int, float)) else 100

            def_bytes = definition.encode("utf-8")
#Yomitan spec : empty rules = "word is not inflected" --that IS positive POS data
#(a non - conjugating word), so stamp POS_OTHER rather than the fail - open 0.
            pos_flags = pos_flags_from_tags(rules.split()) if rules.strip() else POS_OTHER
            seen_headwords = set()
            hw_bytes = headword.encode("utf-8")
            if len(hw_bytes) < HEADWORD_SIZE:
                seen_headwords.add(hw_bytes)
                records.append((hw_bytes, def_bytes, priority, pos_flags))

            if reading_records and reading and reading != headword and not redirect:
                r_bytes = reading.encode("utf-8")
                if len(r_bytes) < HEADWORD_SIZE and r_bytes not in seen_headwords:
                    r_def = format_definition_yomitan(reading, reading, definitions)
                    if r_def:
#reading != headword means kana reading of a kanji headword : flag it
#(see POS_READING above).
                        records.append((r_bytes, r_def.encode("utf-8"), priority,
                                        pos_flags | POS_READING))

            entry_count += 1

    print(f"Processed {entry_count} Yomitan entries → {len(records)} index records")
    return records, title

# ── MDict(.mdx) ────────────────────────────────────────────────


def convert_mdict(mdx_path: str) -> list:
    """Convert an MDict .mdx file to index records.

    Requires: pip install readmdict
    Optional: pip install python-lzo  (for LZO-compressed dictionaries)
    """
    try:
        from readmdict import MDX
    except ImportError:
        print(
            "ERROR: readmdict is required for MDict conversion.\n"
            "Install it with: pip install readmdict\n"
            "For LZO support: pip install python-lzo",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Loading {mdx_path}...")
    mdx = MDX(mdx_path)

    records = []
    entry_count = 0
    skipped = 0

    for key_bytes, val_bytes in mdx.items():
        headword = key_bytes.decode("utf-8", errors="replace").strip()
        raw_def = val_bytes.decode("utf-8", errors="replace").strip()

        if not headword or not raw_def:
            skipped += 1
            continue

        if raw_def.startswith("@@@LINK="):
            skipped += 1
            continue

        definition = strip_html(raw_def)
        if not definition:
            skipped += 1
            continue

        hw_bytes = headword.encode("utf-8")
        if len(hw_bytes) >= HEADWORD_SIZE:
            skipped += 1
            continue

        def_bytes = definition.encode("utf-8")
        records.append((hw_bytes, def_bytes, 100, 0))
        entry_count += 1

    print(f"Processed {entry_count} MDict entries ({skipped} skipped) → {len(records)} index records")
    return records

# ── Chinese : pinyin and zhuyin ────────────────────────────────

_TONE_MARKS = {
    "a": "āáǎà", "e": "ēéěè", "i": "īíǐì", "o": "ōóǒò", "u": "ūúǔù", "ü": "ǖǘǚǜ",
    "A": "ĀÁǍÀ", "E": "ĒÉĚÈ", "I": "ĪÍǏÌ", "O": "ŌÓǑÒ", "U": "ŪÚǓÙ", "Ü": "ǕǗǙǛ",
}
_SYLLABLE_RE = re.compile(r"^([A-Za-zü:]+?)([1-5])$")


def pinyin_syllable_to_marks(syllable: str) -> str:
    """'ni3' -> 'nǐ', 'lu:4' -> 'lǜ', 'ma5' -> 'ma'. Anything else is returned unchanged."""
    m = _SYLLABLE_RE.match(syllable)
    if not m:
        return syllable
    base = m.group(1).replace("u:", "ü").replace("U:", "Ü")
    tone = int(m.group(2))
    if tone == 5:
        return base
    lower = base.lower()
    if "a" in lower:
        idx = lower.index("a")
    elif "e" in lower:
        idx = lower.index("e")
    elif "ou" in lower:
        idx = lower.index("ou")
    else:
        idx = -1
        for i, ch in enumerate(lower):
            if ch in "aeiouü":
                idx = i  # the LAST vowel takes the mark (iu -> iù, ui -> uì)
        if idx < 0:
            return base  # m2, ng2, hm5: no vowel to mark
    marked = _TONE_MARKS[base[idx]][tone - 1]
    return base[:idx] + marked + base[idx + 1:]


def pinyin_to_marks(numbered: str) -> str:
    """Convert a space-separated numbered-tone string ('ni3 hao3') to diacritics."""
    return " ".join(pinyin_syllable_to_marks(part) for part in numbered.split(" "))


_ZHUYIN_INITIALS = [
    ("zh", "ㄓ"), ("ch", "ㄔ"), ("sh", "ㄕ"), ("b", "ㄅ"), ("p", "ㄆ"), ("m", "ㄇ"), ("f", "ㄈ"),
    ("d", "ㄉ"), ("t", "ㄊ"), ("n", "ㄋ"), ("l", "ㄌ"), ("g", "ㄍ"), ("k", "ㄎ"), ("h", "ㄏ"),
    ("j", "ㄐ"), ("q", "ㄑ"), ("x", "ㄒ"), ("r", "ㄖ"), ("z", "ㄗ"), ("c", "ㄘ"), ("s", "ㄙ"),
]
_ZHUYIN_FINALS = {
    "a": "ㄚ", "o": "ㄛ", "e": "ㄜ", "ê": "ㄝ", "ai": "ㄞ", "ei": "ㄟ", "ao": "ㄠ", "ou": "ㄡ",
    "an": "ㄢ", "en": "ㄣ", "ang": "ㄤ", "eng": "ㄥ", "er": "ㄦ", "i": "ㄧ", "ia": "ㄧㄚ",
    "io": "ㄧㄛ", "ie": "ㄧㄝ", "iai": "ㄧㄞ", "iao": "ㄧㄠ", "iu": "ㄧㄡ", "ian": "ㄧㄢ",
    "in": "ㄧㄣ", "iang": "ㄧㄤ", "ing": "ㄧㄥ", "iong": "ㄩㄥ", "u": "ㄨ", "ua": "ㄨㄚ",
    "uo": "ㄨㄛ", "uai": "ㄨㄞ", "ui": "ㄨㄟ", "uan": "ㄨㄢ", "un": "ㄨㄣ", "uang": "ㄨㄤ",
    "ueng": "ㄨㄥ", "ong": "ㄨㄥ", "ü": "ㄩ", "üe": "ㄩㄝ", "üan": "ㄩㄢ", "ün": "ㄩㄣ",
}
# Syllables written with y/w carry the medial in the spelling, not in a separate initial.
_ZHUYIN_WHOLE = {
    "zhi": "ㄓ", "chi": "ㄔ", "shi": "ㄕ", "ri": "ㄖ", "zi": "ㄗ", "ci": "ㄘ", "si": "ㄙ",
    "yi": "ㄧ", "ya": "ㄧㄚ", "yo": "ㄧㄛ", "ye": "ㄧㄝ", "yai": "ㄧㄞ", "yao": "ㄧㄠ", "you": "ㄧㄡ",
    "yan": "ㄧㄢ", "yin": "ㄧㄣ", "yang": "ㄧㄤ", "ying": "ㄧㄥ", "yong": "ㄩㄥ",
    "wu": "ㄨ", "wa": "ㄨㄚ", "wo": "ㄨㄛ", "wai": "ㄨㄞ", "wei": "ㄨㄟ", "wan": "ㄨㄢ", "wen": "ㄨㄣ",
    "wang": "ㄨㄤ", "weng": "ㄨㄥ",
    "yu": "ㄩ", "yue": "ㄩㄝ", "yuan": "ㄩㄢ", "yun": "ㄩㄣ",
    "r": "ㄦ", "m": "ㄇ", "n": "ㄋ", "ng": "ㄫ", "hm": "ㄏㄇ", "hng": "ㄏㄫ",
}
_ZHUYIN_TONES = {1: "", 2: "ˊ", 3: "ˇ", 4: "ˋ"}


def pinyin_syllable_to_zhuyin(syllable: str) -> str:
    """'ni3' -> 'ㄋㄧˇ', 'lu:4' -> 'ㄌㄩˋ', 'ma5' -> '˙ㄇㄚ'. Unknown syllables come back unchanged."""
    m = _SYLLABLE_RE.match(syllable)
    if not m:
        return syllable
    base = m.group(1).lower().replace("u:", "ü").replace("v", "ü")
    tone = int(m.group(2))
    body = _ZHUYIN_WHOLE.get(base)
    if body is None:
        initial = ""
        rest = base
        for latin, bopomofo in _ZHUYIN_INITIALS:
            if base.startswith(latin):
                initial, rest = bopomofo, base[len(latin):]
                break
        # After j/q/x (and y, handled above) a written u is ü.
        if initial in ("ㄐ", "ㄑ", "ㄒ") and rest.startswith("u"):
            rest = "ü" + rest[1:]
        final = _ZHUYIN_FINALS.get(rest)
        if final is None or (not initial and rest != base):
            return syllable
        body = initial + final
    if tone == 5:
        return "˙" + body
    return body + _ZHUYIN_TONES[tone]


def pinyin_to_zhuyin(numbered: str) -> str:
    return " ".join(pinyin_syllable_to_zhuyin(part) for part in numbered.split(" "))


_BRACKETED_PINYIN_RE = re.compile(r"\[([A-Za-z0-9:\u00fc\u00dc ,]+)\]")


def prettify_cedict_gloss(gloss: str) -> str:
    """CEDICT glosses embed numbered pinyin in brackets (CL:個|个[ge4], see 你好[ni3 hao3])."""
    return _BRACKETED_PINYIN_RE.sub(lambda m: "[" + pinyin_to_marks(m.group(1)) + "]", gloss)


# ── Chinese : frequency ranking ────────────────────────────────

_HAN_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\U00020000-\U0003134f]")
UNRANKED_PRIORITY = 60  # entries absent from the frequency list


def rank_to_priority(rank: int) -> int:
    """1 -> 255, 10 -> 227, 1000 -> 171, 100000 -> 115: log-scaled so the common words spread."""
    return max(UNRANKED_PRIORITY + 1, min(255, 255 - int(round(28 * math.log10(rank)))))


def load_frequency(path: str, kind: str = "auto") -> dict:
    """Read a word list into {word: priority}.

    Accepts jieba dict.txt ("word count pos"), BCC/SUBTLEX exports ("word<TAB>count"), and
    graded lists such as HSK or TOCFL CSVs where the order of the rows IS the ranking. The word
    is the first Han field of each row; a count is the first number after it. kind=auto ranks by
    count when most rows carry one, else by row order; count/rank force either.
    """
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip().lstrip("\ufeff")
            if not line or line.startswith("#"):
                continue
            fields = [x.strip().strip('"') for x in re.split(r"[\t,]|\s+", line) if x.strip()]
            word = next((x for x in fields if _HAN_RE.search(x)), None)
            if not word:
                continue
            count = None
            for x in fields[fields.index(word) + 1:]:
                try:
                    count = float(x)
                    break
                except ValueError:
                    continue
            rows.append((word, count))
    if not rows:
        print(f"WARNING: no words found in frequency list {path}", file=sys.stderr)
        return {}
    with_count = sum(1 for _, c in rows if c is not None)
    # A graded list's level column is numeric too (HSK 1-9, TOCFL 1-7), but it is not a count:
    # auto treats small numbers as levels and keeps the file's own order.
    largest = max((c for _, c in rows if c is not None), default=0.0)
    use_count = kind == "count" or (kind == "auto" and with_count >= 0.8 * len(rows) and largest > 100)
    if use_count:
        rows.sort(key=lambda r: -(r[1] or 0.0))
    priorities = {}
    for rank, (word, _) in enumerate(rows, start=1):
        p = rank_to_priority(rank)
        if p > priorities.get(word, 0):
            priorities[word] = p
    print(f"Frequency list {path}: {len(priorities):,} words, ranked by {'count' if use_count else 'row order'}")
    return priorities


def apply_frequency(records: list, priorities: dict) -> list:
    if not priorities:
        return records
    return [(hw, d, priorities.get(hw.decode("utf-8"), UNRANKED_PRIORITY), pos) for hw, d, _, pos in records]


# ── CC - CEDICT(.u8 / .txt) ────────────────────────────────────

_CEDICT_LINE_RE = re.compile(r"^(\S+)\s+(\S+)\s+\[([^\]]*)\]\s+/(.*)/\s*$")


def format_definition_cedict(trad: str, simp: str, pinyin: str, glosses: list, zhuyin: bool) -> str:
    reading = pinyin_to_marks(pinyin)
    if zhuyin:
        reading += " " + pinyin_to_zhuyin(pinyin)
    parts = ["【" + reading + "】"]
    if trad != simp:
        parts.append(f"{trad} / {simp}")
    glosses = [prettify_cedict_gloss(g) for g in glosses if g][:12]
    if len(glosses) == 1:
        parts.append(glosses[0])
    else:
        parts.extend(f"{i + 1}. {g}" for i, g in enumerate(glosses))
    return "\n".join(parts)


def convert_cedict(path: str, zhuyin: bool = False) -> list:
    """Convert a raw CC-CEDICT file to index records, one per traditional and simplified form."""
    print(f"Loading {path}...")
    records = []
    entries = 0
    skipped = 0
    opener = __import__("gzip").open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            m = _CEDICT_LINE_RE.match(line.rstrip("\n"))
            if not m:
                skipped += 1
                continue
            trad, simp, pinyin, body = m.groups()
            glosses = [g.strip() for g in body.split("/")]
            definition = format_definition_cedict(trad, simp, pinyin, glosses, zhuyin)
            def_bytes = definition.encode("utf-8")
            entries += 1
            for hw in dict.fromkeys((trad, simp)):  # both forms, once each
                hw_bytes = hw.encode("utf-8")
                if len(hw_bytes) >= HEADWORD_SIZE:
                    skipped += 1
                    continue
                records.append((hw_bytes, def_bytes, 100, POS_OTHER))
    print(f"Processed {entries} CC-CEDICT entries ({skipped} skipped) → {len(records)} index records")
    return records


# ── MoE 重編國語辭典(g0v moedict JSON) ─────────────────────────

_MOE_GLYPH_REF_RE = re.compile(r"\{\[[0-9a-fA-F]+\]\}")


def _moe_text(value) -> str:
    if value is None:
        return ""
    if isinstance(value, list):
        return " ".join(_moe_text(v) for v in value)
    text = strip_html(str(value))
    return _MOE_GLYPH_REF_RE.sub("□", text).strip()


def format_definition_moedict(entry: dict) -> str:
    parts = []
    for heteronym in entry.get("heteronyms", [])[:3]:
        reading = " · ".join(x for x in (_moe_text(heteronym.get("bopomofo")), _moe_text(heteronym.get("pinyin"))) if x)
        if reading:
            parts.append("【" + reading + "】")
        definitions = heteronym.get("definitions", [])[:8]
        numbered = len(definitions) > 1
        for i, d in enumerate(definitions):
            line = ""
            kind = _moe_text(d.get("type"))
            if kind:
                line += f"[{kind}] "
            if numbered:
                line += f"{i + 1}. "
            line += _moe_text(d.get("def"))
            parts.append(line)
            for example in (d.get("example") or [])[:2]:
                parts.append("  " + _moe_text(example))
            for quote in (d.get("quote") or [])[:1]:
                parts.append("  " + _moe_text(quote))
    return "\n".join(p for p in parts if p.strip())


def convert_moedict(path: str) -> list:
    """Convert the g0v dict-revised.json (MoE 重編國語辭典修訂本) to index records."""
    print(f"Loading {path}...")
    if path.endswith(".xz"):
        import lzma
        with lzma.open(path, "rt", encoding="utf-8") as f:
            data = json.load(f)
    else:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    records = []
    skipped = 0
    for entry in data:
        title = _moe_text(entry.get("title", ""))
        if not title or "□" in title:
            skipped += 1
            continue
        definition = format_definition_moedict(entry)
        if not definition:
            skipped += 1
            continue
        hw_bytes = title.encode("utf-8")
        if len(hw_bytes) >= HEADWORD_SIZE:
            skipped += 1
            continue
        # Below CC-CEDICT's default so a merged file shows the bilingual entry first.
        records.append((hw_bytes, definition.encode("utf-8"), 90, POS_OTHER))
    print(f"Processed {len(records)} MoE entries ({skipped} skipped)")
    return records

# ── Format detection & main ─────────────────────────────────────


def detect_format(path: str, lang: str = "ja") -> str:
    """Detect input format from file extension (and, for Chinese, the file name)."""
    lower = os.path.basename(path.lower())
    if lower.endswith(".mdx"):
        return "mdict"
    if lower.endswith(".zip"):
        return "yomitan"
    if lower.endswith(".u8") or lower.endswith(".u8.gz") or "cedict" in lower:
        return "cedict"
    if lang == "zh" and (lower.endswith(".json") or lower.endswith(".json.xz")):
        return "moedict"
    if lower.endswith(".json") or lower.endswith(".json.tgz") or lower.endswith(".tgz"):
        return "jmdict"
    if lang == "zh":
        return "cedict"
    return "jmdict"


DEFAULT_TITLES = {"cedict": "CC-CEDICT", "moedict": "MoE 國語辭典", "jmdict": ""}


def main():
    parser = argparse.ArgumentParser(
        description="Convert dictionary files to CrossPoint binary index.\n\n"
        "Supported formats: JMdict JSON, Yomitan .zip, MDict .mdx, CC-CEDICT text, MoE (g0v) JSON",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--input",
        action="append",
        help="Path to an input dictionary file. Format auto-detected from extension: "
        ".json/.tgz → JMdict, .zip → Yomitan, .mdx → MDict, .u8/.txt → CC-CEDICT, "
        ".json(.xz) with --lang zh → MoE. Repeat to merge several dictionaries into one file; "
        "a word in both shows both entries. If omitted, downloads jmdict-simplified.",
    )
    parser.add_argument("--output-dir", default="output", help="Output directory (default: output)")
    parser.add_argument(
        "--lang",
        default="ja",
        choices=["ja", "zh"],
        help="Language of the dictionary: ja (default) or zh. Chinese skips the reading records "
        "(pinyin never appears in page text) and names the dictionary for the lookup footer.",
    )
    parser.add_argument(
        "--name",
        default="vocab",
        choices=["vocab", "names", "grammar"],
        help="Which dictionary slot the output files fill (default: vocab -> vocab.idx/"
        "vocab.dat). The device reads vocab (main dictionary), names (name dictionary), and "
        "grammar (grammar dictionary); pass --name names or --name grammar to fill those "
        "slots directly, no manual renaming needed.",
    )
    parser.add_argument(
        "--format",
        choices=["jmdict", "yomitan", "mdict", "cedict", "moedict"],
        help="Force input format for every --input (overrides auto-detection)",
    )
    parser.add_argument(
        "--title",
        help="Dictionary name shown in the lookup panel footer (written to <name>.title). "
        "Defaults to the Yomitan title, CC-CEDICT, or MoE 國語辭典 for Chinese inputs.",
    )
    parser.add_argument(
        "--zhuyin",
        action="store_true",
        help="CC-CEDICT: show zhuyin (bopomofo) beside the pinyin, for Taiwanese Mandarin.",
    )
    parser.add_argument(
        "--frequency",
        help="Word list that ranks the entries (jieba dict.txt, a BCC/SUBTLEX export, or a graded "
        "HSK/TOCFL CSV). Common words get a higher priority, which orders the entries shown "
        "for a word found in several dictionaries.",
    )
    parser.add_argument(
        "--frequency-kind",
        default="auto",
        choices=["auto", "count", "rank"],
        help="How to read --frequency: by the count column, by row order, or auto (default).",
    )
    args = parser.parse_args()

    if not args.input:
        if args.lang == "zh":
            print("Error: --lang zh needs --input (a CC-CEDICT file, MoE JSON, Yomitan zip or .mdx).",
                  file=sys.stderr)
            sys.exit(1)
        if args.name != "vocab":
            print(
                f"Error: --name {args.name} without --input would write the auto-downloaded "
                "JMdict (a vocabulary dictionary) into the "
                f"{args.name} slot. Pass --input with the actual "
                f"{args.name} dictionary instead.",
                file=sys.stderr,
            )
            sys.exit(1)
        args.input = [download_jmdict(os.path.join(args.output_dir, "jmdict-eng"))]

    records = []
    titles = []
    for path in args.input:
        fmt = args.format or detect_format(path, args.lang)
        print(f"{path}: format {fmt}")
        title = DEFAULT_TITLES.get(fmt, "")
        if fmt == "mdict":
            part = convert_mdict(path)
            title = os.path.splitext(os.path.basename(path))[0]
        elif fmt == "yomitan":
            part, title = convert_yomitan(path, reading_records=args.lang == "ja")
        elif fmt == "cedict":
            part = convert_cedict(path, zhuyin=args.zhuyin)
        elif fmt == "moedict":
            part = convert_moedict(path)
        else:
            part = convert_jmdict(path)
        records.extend(part)
        if title:
            titles.append(title)

    if args.frequency:
        records = apply_frequency(records, load_frequency(args.frequency, args.frequency_kind))

    title = args.title or ""
    if not title and args.lang == "zh" and titles:
        joined = " + ".join(dict.fromkeys(titles))
        title = joined if len(joined.encode("utf-8")) <= 36 else titles[0]
    write_binary(records, args.output_dir, args.name, title)
    if args.lang == "zh":
        print("Install under /dictionaries/zh/ on the SD card (or /.dictionaries/zh/).")


if __name__ == "__main__":
    main()
