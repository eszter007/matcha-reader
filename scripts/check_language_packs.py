#!/usr/bin/env python3
"""
Validate generated .cplang language packs against the firmware's key table.

A pack indexes strings by key *position*, so one built against a different set of
keys would not fail loudly on the device -- it would return the wrong strings.
I18n::loadPack() therefore refuses any pack whose key count differs from
StrId::_COUNT, and this script asserts the same thing at build time, where a
mismatch is a fixable CI failure rather than a language that silently refuses to
load on every device.

Usage:
    python3 check_language_packs.py lib/I18n/I18nKeys.h dist/lang
"""

import re
import struct
import sys
from pathlib import Path

MAGIC = b"CPLANG\0\0"
VERSION = 1
HEADER_BYTES = 18


def read_key_count(keys_header: Path) -> int:
    """Count the StrId enumerators in the generated I18nKeys.h."""
    text = keys_header.read_text(encoding="utf-8")
    match = re.search(r"enum\s+class\s+StrId\s*:[^{]*\{(.*?)\}", text, re.S)
    if not match:
        raise SystemExit(f"{keys_header}: no StrId enum found")
    body = match.group(1)
    keys = re.findall(r"^\s*(STR_[A-Z0-9_]+)\s*(?:=|,)", body, re.M)
    if not keys:
        raise SystemExit(f"{keys_header}: StrId enum has no STR_* entries")
    return len(keys)


def check_pack(path: Path, expected_keys: int) -> None:
    data = path.read_bytes()
    if len(data) < HEADER_BYTES:
        raise SystemExit(f"{path}: shorter than a pack header")
    if data[:8] != MAGIC:
        raise SystemExit(f"{path}: bad magic {data[:8]!r}")

    version, key_count, blob_len = struct.unpack_from("<HHH", data, 8)
    code = data[14:18].rstrip(b"\0").decode("ascii", "replace")
    if version != VERSION:
        raise SystemExit(f"{path}: version {version}, expected {VERSION}")
    if key_count != expected_keys:
        raise SystemExit(
            f"{path}: {key_count} keys, firmware has {expected_keys} -- "
            "regenerate the packs alongside the firmware"
        )
    if code != path.stem:
        raise SystemExit(f"{path}: carries code {code!r}, expected {path.stem!r}")

    expected_size = HEADER_BYTES + key_count * 2 + blob_len
    if len(data) != expected_size:
        raise SystemExit(f"{path}: {len(data)} bytes, header describes {expected_size}")

    # Every offset must land inside its own blob, or inside the English blob when bit 15
    # marks the entry as "same as English" -- the device resolves those without a bounds check.
    offsets = struct.unpack_from(f"<{key_count}H", data, HEADER_BYTES)
    for index, off in enumerate(offsets):
        if not (off & 0x8000) and (off & 0x7FFF) >= blob_len:
            raise SystemExit(f"{path}: offset {off} for key {index} is past the {blob_len}B blob")


def main(argv: list) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    keys_header, pack_dir = Path(argv[1]), Path(argv[2])
    expected = read_key_count(keys_header)

    packs = sorted(pack_dir.glob("*.cplang"))
    if not packs:
        raise SystemExit(f"{pack_dir}: no .cplang packs to check")

    for pack in packs:
        check_pack(pack, expected)

    total = sum(p.stat().st_size for p in packs)
    print(f"{len(packs)} packs OK against {expected} keys ({total:,} bytes total)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
