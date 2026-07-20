#!/ usr / bin / env python3
"""Convert dictionary files to binary index files for CrossPoint Reader.

Supported input formats:
  - jmdict-simplified JSON (.json / .json.tgz)
  - Yomitan/Yomichan (.zip containing term_bank_N.json)
  - MDict (.mdx) — requires: pip install readmdict

Emits:
  jmdict.idx  -- sorted array of 40-byte records (headword + offset + length + priority)
  jmdict.dat  -- variable-length definition text blob

Usage:
#JMdict(default — downloads if no-- input given)
    python3 convert_jmdict.py [--input jmdict-eng-3.5.0.json] [--output-dir ./output]

#Yomitan dictionary zip
    python3 convert_jmdict.py --input jmdict-yomitan.zip --output-dir ./output

#MDict.mdx file
    python3 convert_jmdict.py --input dictionary.mdx --output-dir ./output
"""

import argparse
import json
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


def write_binary(records: list, output_dir: str):
    """Write (headword_bytes, definition_bytes, priority, pos_flags) tuples to idx+dat files.

    Expects records to be pre-validated (headword_bytes < HEADWORD_SIZE).
    """
    records.sort(key=lambda r: r[0])

    os.makedirs(output_dir, exist_ok=True)
    dat_path = os.path.join(output_dir, "jmdict.dat")
    idx_path = os.path.join(output_dir, "jmdict.idx")

    dat_offset = 0
    index_entries = []

    with open(dat_path, "wb") as dat_f:
        prev_def = None
        prev_offset = 0
        prev_length = 0

        for hw_bytes, def_bytes, priority, pos_flags in records:
            if def_bytes == prev_def:
                offset = prev_offset
                length = prev_length
            else:
                offset = dat_offset
                length = len(def_bytes)
                if length > 0xFFFF:
                    length = 0xFFFF
                    def_bytes = def_bytes[:0xFFFF]
                dat_f.write(def_bytes)
                dat_offset += len(def_bytes)
                prev_def = def_bytes
                prev_offset = offset
                prev_length = length

            padded_hw = hw_bytes + b"\x00" * (HEADWORD_SIZE - len(hw_bytes))
            index_entries.append((padded_hw, offset, length, priority, pos_flags))

    with open(idx_path, "wb") as idx_f:
        for padded_hw, offset, length, priority, pos_flags in index_entries:
            idx_f.write(struct.pack(RECORD_FORMAT, padded_hw, offset, length, priority, pos_flags))

    dat_size = os.path.getsize(dat_path)
    idx_size = os.path.getsize(idx_path)
    print(f"Output:")
    print(f"  {idx_path}: {idx_size:,} bytes ({len(index_entries):,} records)")
    print(f"  {dat_path}: {dat_size:,} bytes")
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


def convert_jmdict(json_path: str, output_dir: str):
    """Convert JMdict JSON to binary index + data files."""
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
    write_binary(records, output_dir)

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


def convert_yomitan(zip_path: str, output_dir: str):
    """Convert a Yomitan/Yomichan .zip dictionary to binary index + data files."""
    import zipfile

    print(f"Loading {zip_path}...")

    with zipfile.ZipFile(zip_path, "r") as z:
        names = z.namelist()

        if "index.json" in names:
            with z.open("index.json") as f:
                meta = json.load(f)
            print(f"  Dictionary: {meta.get('title', '(unknown)')}")
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

            if reading and reading != headword and not redirect:
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
    write_binary(records, output_dir)

# ── MDict(.mdx) ────────────────────────────────────────────────


def convert_mdict(mdx_path: str, output_dir: str):
    """Convert an MDict .mdx file to binary index + data files.

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
    write_binary(records, output_dir)

# ── Format detection & main ─────────────────────────────────────


def detect_format(path: str) -> str:
    """Detect input format from file extension."""
    lower = path.lower()
    if lower.endswith(".mdx"):
        return "mdict"
    if lower.endswith(".zip"):
        return "yomitan"
    if lower.endswith(".json") or lower.endswith(".json.tgz") or lower.endswith(".tgz"):
        return "jmdict"
    return "jmdict"


def main():
    parser = argparse.ArgumentParser(
        description="Convert dictionary files to CrossPoint binary index.\n\n"
        "Supported formats: JMdict JSON, Yomitan .zip, MDict .mdx",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--input",
        help="Path to input dictionary file. Format auto-detected from extension: "
        ".json/.tgz → JMdict, .zip → Yomitan, .mdx → MDict. "
        "If omitted, downloads jmdict-simplified.",
    )
    parser.add_argument("--output-dir", default="output", help="Output directory (default: output)")
    parser.add_argument(
        "--format",
        choices=["jmdict", "yomitan", "mdict"],
        help="Force input format (overrides auto-detection)",
    )
    args = parser.parse_args()

    if args.input:
        fmt = args.format or detect_format(args.input)
        print(f"Detected format: {fmt}")

        if fmt == "mdict":
            convert_mdict(args.input, args.output_dir)
        elif fmt == "yomitan":
            convert_yomitan(args.input, args.output_dir)
        else:
            convert_jmdict(args.input, args.output_dir)
    else:
        json_path = download_jmdict(os.path.join(args.output_dir, "jmdict-eng"))
        convert_jmdict(json_path, args.output_dir)


if __name__ == "__main__":
    main()
