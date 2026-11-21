#!/usr/bin/env python3
"""Write a .cphyph hyphenation pack for every language not built into the firmware.

Reads the tries from lib/Epub/Epub/hyphenation/generated/hyph-<tag>.trie.h, and with --fetch also
the languages that have no header, from hypher's tries at the commit licenses/hyphenation.txt
names. The format is described in docs/file-formats.md and read by
lib/Epub/Epub/hyphenation/HyphenationPack.cpp.
"""

import argparse
import pathlib
import re
import struct
import sys
import urllib.request

FORMAT_VERSION = 1
BUILT_IN = {"en"}
REPO = pathlib.Path(__file__).resolve().parent.parent
GENERATED = REPO / "lib/Epub/Epub/hyphenation/generated"
HYPHER_TRIES = "https://raw.githubusercontent.com/typst/hypher/d81cc506416bffef2a75ee2dc969d4b41bc36613/tries/"
# Pack languages without a generated header; each needs an entry in LanguageRegistry.cpp.
FETCHED = ["af", "be", "bg", "ca", "cs", "da", "el", "et", "gl", "hr", "hu", "is", "ka", "ku", "la", "lt",
           "mn", "nl", "no", "sk", "sl", "sq", "sr", "tk", "tr"]


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def read_trie(header: pathlib.Path):
    text = header.read_text()
    array = re.search(r"_trie_data\[\] = \{(.*?)\};", text, re.S)
    root = re.search(r"_patterns = \{\s*(0x[0-9A-Fa-f]+)u?,", text)
    if not array or not root:
        raise ValueError(f"{header}: no trie data or root offset")
    data = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})\b", array.group(1)))
    return data, int(root.group(1), 16)


def read_hypher_bin(blob: bytes):
    """hypher's .bin: a big-endian root address, then the trie the address points into."""
    root = struct.unpack(">I", blob[:4])[0]
    if root < 4 or root > len(blob):
        raise ValueError("bad root address")
    return blob[4:], root - 4


def pack(tag: str, data: bytes, root: int) -> bytes:
    key = fnv1a(data) | 1
    tag_bytes = tag.encode("ascii").ljust(4, b"\0")
    if len(tag_bytes) != 4 or root >= len(data):
        raise ValueError(f"{tag}: bad tag or root offset")
    header = b"CPHY" + bytes([FORMAT_VERSION, 0, 0, 0]) + tag_bytes + struct.pack("<III", root, len(data), key)
    return header + data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=pathlib.Path, help="directory for the .cphyph files")
    parser.add_argument("--fetch", action="store_true", help="also download the languages without a header")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for header in sorted(GENERATED.glob("hyph-*.trie.h")):
        tag = header.name[len("hyph-"):-len(".trie.h")]
        if tag in BUILT_IN:
            continue
        data, root = read_trie(header)
        out = args.out / f"{tag}.cphyph"
        out.write_bytes(pack(tag, data, root))
        print(f"{out.name}: {len(data)} bytes")
    if args.fetch:
        for tag in FETCHED:
            with urllib.request.urlopen(HYPHER_TRIES + f"{tag}.bin") as response:
                data, root = read_hypher_bin(response.read())
            out = args.out / f"{tag}.cphyph"
            out.write_bytes(pack(tag, data, root))
            print(f"{out.name}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
