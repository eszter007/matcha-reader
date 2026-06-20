#!/usr/bin/env python3
"""Convert dictionary files to binary index files for CrossPoint Reader.

Supported input formats:
  - jmdict-simplified JSON (.json / .json.tgz)
  - Yomitan/Yomichan (.zip containing term_bank_N.json)
  - MDict (.mdx) — requires: pip install readmdict

Emits:
  jmdict.idx  -- sorted array of 40-byte records (headword + offset + length + priority)
  jmdict.dat  -- variable-length definition text blob

Usage:
    # JMdict (default — downloads if no --input given)
    python3 convert_jmdict.py [--input jmdict-eng-3.5.0.json] [--output-dir ./output]

    # Yomitan dictionary zip
    python3 convert_jmdict.py --input jmdict-yomitan.zip --output-dir ./output

    # MDict .mdx file
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
    """Write (headword_bytes, definition_bytes, priority) triples to idx+dat files.

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

        for hw_bytes, def_bytes, priority in records:
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
            index_entries.append((padded_hw, offset, length, priority))

    with open(idx_path, "wb") as idx_f:
        for padded_hw, offset, length, priority in index_entries:
            idx_f.write(struct.pack(RECORD_FORMAT, padded_hw, offset, length, priority, 0))

    dat_size = os.path.getsize(dat_path)
    idx_size = os.path.getsize(idx_path)
    print(f"Output:")
    print(f"  {idx_path}: {idx_size:,} bytes ({len(index_entries):,} records)")
    print(f"  {dat_path}: {dat_size:,} bytes")
    print(f"  Total: {(idx_size + dat_size) / 1024 / 1024:.1f} MB")


# ── JMdict (jmdict-simplified JSON) ─────────────────────────────


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

        seen_headwords = set()
        for kanji in entry.get("kanji", []):
            hw = kanji["text"]
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE or hw_bytes in seen_headwords:
                continue
            seen_headwords.add(hw_bytes)
            records.append((hw_bytes, def_bytes, priority))

        for kana in entry.get("kana", []):
            hw = kana["text"]
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE or hw_bytes in seen_headwords:
                continue
            seen_headwords.add(hw_bytes)
            records.append((hw_bytes, def_bytes, priority))

    print(f"Generated {len(records)} index records")
    write_binary(records, output_dir)


# ── Yomitan / Yomichan (.zip) ───────────────────────────────────


def flatten_structured_content(content) -> str:
    """Recursively extract plain text from Yomitan structured content."""
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
        text = flatten_structured_content(inner)
        if tag == "br":
            return "\n"
        if tag in ("li",):
            return text + "\n"
        return text
    return str(content)


def format_definition_yomitan(reading: str, definitions) -> str:
    """Format a Yomitan entry's reading + definitions into display string."""
    parts = []
    if reading:
        parts.append(f"【{reading}】")

    if isinstance(definitions, list):
        for i, defn in enumerate(definitions[:5]):
            if isinstance(defn, str):
                text = defn
            elif isinstance(defn, dict):
                text = flatten_structured_content(defn)
            else:
                text = str(defn)
            text = text.strip()
            if not text:
                continue
            prefix = f"{i+1}. " if len(definitions) > 1 else ""
            parts.append(prefix + text)

    return "\n".join(parts)


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

        records = []
        entry_count = 0

        for bank_name in term_banks:
            with z.open(bank_name) as f:
                entries = json.load(f)

            for entry in entries:
                if not isinstance(entry, list) or len(entry) < 6:
                    continue

                headword = entry[0]
                reading = entry[1] if len(entry) > 1 else ""
                score = entry[4] if len(entry) > 4 else 0
                definitions = entry[5] if len(entry) > 5 else []

                if not headword or not isinstance(headword, str):
                    continue

                definition = format_definition_yomitan(reading, definitions)
                if not definition:
                    continue

                def_bytes = definition.encode("utf-8")
                priority = max(0, min(255, int(score) + 128)) if isinstance(score, (int, float)) else 100

                seen_headwords = set()
                hw_bytes = headword.encode("utf-8")
                if len(hw_bytes) < HEADWORD_SIZE:
                    seen_headwords.add(hw_bytes)
                    records.append((hw_bytes, def_bytes, priority))

                if reading and reading != headword:
                    r_bytes = reading.encode("utf-8")
                    if len(r_bytes) < HEADWORD_SIZE and r_bytes not in seen_headwords:
                        records.append((r_bytes, def_bytes, priority))

                entry_count += 1

    print(f"Processed {entry_count} Yomitan entries → {len(records)} index records")
    write_binary(records, output_dir)


# ── MDict (.mdx) ────────────────────────────────────────────────


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
        records.append((hw_bytes, def_bytes, 100))
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
