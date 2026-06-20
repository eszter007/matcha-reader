#!/usr/bin/env python3
"""Convert jmdict-simplified JSON to binary index files for CrossPoint Reader.

Downloads jmdict-eng-3.5.0.json from the jmdict-simplified releases,
then emits:
  jmdict.idx  -- sorted array of 40-byte records (headword + offset + length + priority)
  jmdict.dat  -- variable-length definition text blob

Usage:
    python3 convert_jmdict.py [--input jmdict-eng-3.5.0.json] [--output-dir ./output]

If --input is not given, downloads the latest jmdict-simplified English release.
"""

import argparse
import json
import os
import struct
import sys
import urllib.request

JMDICT_URL = "https://github.com/scriptin/jmdict-simplified/releases/latest/download/jmdict-eng-3.5.0.json.tgz"
HEADWORD_SIZE = 32
RECORD_FORMAT = f"<{HEADWORD_SIZE}sIHBB"  # headword(32) + offset(4) + length(2) + priority(1) + pad(1)
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)

assert RECORD_SIZE == 40, f"Record size mismatch: {RECORD_SIZE}"


def download_jmdict(output_path: str) -> str:
    """Download and extract jmdict-simplified JSON."""
    import tarfile
    import io

    tgz_path = output_path + ".tgz"
    if not os.path.exists(tgz_path):
        print(f"Downloading {JMDICT_URL}...")
        urllib.request.urlretrieve(JMDICT_URL, tgz_path)
        print(f"Downloaded to {tgz_path}")

    # Extract the JSON from the tarball
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


def compute_priority(entry: dict) -> int:
    """Map JMdict priority tags to a 0-255 score (higher = more common)."""
    priorities = set()
    for kanji in entry.get("kanji", []):
        for p in kanji.get("common", []) if isinstance(kanji.get("common"), list) else []:
            priorities.add(p)
    for kana in entry.get("kana", []):
        for p in kana.get("common", []) if isinstance(kana.get("common"), list) else []:
            priorities.add(p)

    # jmdict-simplified uses a boolean "common" field
    is_common = False
    for kanji in entry.get("kanji", []):
        if kanji.get("common", False):
            is_common = True
            break
    for kana in entry.get("kana", []):
        if kana.get("common", False):
            is_common = True
            break

    return 200 if is_common else 100


def format_definition(entry: dict) -> str:
    """Format an entry's readings and glosses into a compact display string."""
    parts = []

    # Readings (kana)
    readings = [k["text"] for k in entry.get("kana", [])]
    if readings:
        parts.append("【" + "、".join(readings[:3]) + "】")

    # Senses (glosses)
    senses = entry.get("sense", [])
    for i, sense in enumerate(senses[:3]):
        glosses = [g["text"] for g in sense.get("gloss", [])]
        if glosses:
            prefix = f"{i+1}. " if len(senses) > 1 else ""
            parts.append(prefix + "; ".join(glosses[:4]))

    return "\n".join(parts)


def convert(json_path: str, output_dir: str):
    """Convert JMdict JSON to binary index + data files."""
    print(f"Loading {json_path}...")
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    words = data.get("words", [])
    print(f"Processing {len(words)} entries...")

    # Build (headword, definition_bytes, priority) tuples.
    # Each kanji form and kana form becomes a separate index entry pointing
    # to the same definition blob.
    records = []  # (headword_bytes, definition_bytes, priority)

    for entry in words:
        definition = format_definition(entry)
        def_bytes = definition.encode("utf-8")
        priority = compute_priority(entry)

        # Index by kanji forms
        seen_headwords = set()
        for kanji in entry.get("kanji", []):
            hw = kanji["text"]
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE or hw_bytes in seen_headwords:
                continue
            seen_headwords.add(hw_bytes)
            records.append((hw_bytes, def_bytes, priority))

        # Index by kana forms
        for kana in entry.get("kana", []):
            hw = kana["text"]
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE or hw_bytes in seen_headwords:
                continue
            seen_headwords.add(hw_bytes)
            records.append((hw_bytes, def_bytes, priority))

    print(f"Generated {len(records)} index records")

    # Sort by headword bytes (lexicographic, matches memcmp on device)
    records.sort(key=lambda r: r[0])

    # Write dat file (variable-length definitions)
    os.makedirs(output_dir, exist_ok=True)
    dat_path = os.path.join(output_dir, "jmdict.dat")
    idx_path = os.path.join(output_dir, "jmdict.idx")

    dat_offset = 0
    index_entries = []

    with open(dat_path, "wb") as dat_f:
        # Deduplicate: if consecutive records have the same definition bytes,
        # reuse the same dat offset.
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

            # Pad headword to fixed size
            padded_hw = hw_bytes + b"\x00" * (HEADWORD_SIZE - len(hw_bytes))
            index_entries.append((padded_hw, offset, length, priority))

    # Write idx file (sorted, fixed-size records)
    with open(idx_path, "wb") as idx_f:
        for padded_hw, offset, length, priority in index_entries:
            idx_f.write(struct.pack(RECORD_FORMAT, padded_hw, offset, length, priority, 0))

    dat_size = os.path.getsize(dat_path)
    idx_size = os.path.getsize(idx_path)
    print(f"Output:")
    print(f"  {idx_path}: {idx_size:,} bytes ({len(index_entries):,} records)")
    print(f"  {dat_path}: {dat_size:,} bytes")
    print(f"  Total: {(idx_size + dat_size) / 1024 / 1024:.1f} MB")


def main():
    parser = argparse.ArgumentParser(description="Convert JMdict to CrossPoint binary index")
    parser.add_argument("--input", help="Path to jmdict-eng-3.5.0.json (downloads if not given)")
    parser.add_argument("--output-dir", default="output", help="Output directory (default: output)")
    args = parser.parse_args()

    if args.input:
        json_path = args.input
    else:
        json_path = download_jmdict(os.path.join(args.output_dir, "jmdict-eng"))

    convert(json_path, args.output_dir)


if __name__ == "__main__":
    main()
