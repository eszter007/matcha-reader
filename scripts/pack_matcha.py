#!/usr/bin/env python3
"""Packs a converted manga folder (panels.idx, panels.dat, page_*.jpg, panels/, ...) into one
.matcha file for the reader. A .matcha is a plain ZIP with every entry stored, not compressed:
renamed to .zip it unzips back into the same folder. See lib/MatchaContainer/MatchaContainer.h.

Usage: pack_matcha.py <manga folder> [output.matcha]
"""
import os
import struct
import sys
import time
import zlib

INDEX_NAME = "matcha.idx"
INDEX_VERSION = 1
DIRECTORY_SIZE = 0xFFFFFFFF
UTF8_FLAG = 0x0800


def fnv1a(name: bytes) -> int:
    h = 2166136261
    for b in name:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def dos_time(ts: float) -> tuple[int, int]:
    t = time.localtime(max(ts, 315532800))  # the ZIP epoch is 1980
    return (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2), ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday


def collect(folder: str) -> list[tuple[str, str | None]]:
    """(entry name, source path or None for a directory), sorted, hidden files skipped."""
    entries = []
    for root, dirs, files in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        rel = os.path.relpath(root, folder)
        if rel != ".":
            entries.append((rel.replace(os.sep, "/") + "/", None))
        for f in sorted(files):
            if f.startswith(".") or f == INDEX_NAME:
                continue
            entries.append((os.path.relpath(os.path.join(root, f), folder).replace(os.sep, "/"), os.path.join(root, f)))
    return entries


def pack(folder: str, out_path: str) -> None:
    """Writes folder as out_path. Raises ValueError when the folder cannot be packed."""
    if not os.path.isfile(os.path.join(folder, "panels.idx")):
        raise ValueError(f"{folder} has no panels.idx: not a converted manga folder")

    entries = collect(folder)
    if len(entries) >= 0xFFFF:
        raise ValueError(f"{len(entries)} files: a ZIP without ZIP64 holds at most 65534 besides the index")
    names = [n.encode("utf-8") for n, _ in entries]
    sizes = [0 if src is None else os.path.getsize(src) for _, src in entries]
    hashes = [fnv1a(n) for n in names]
    if len(set(hashes)) != len(hashes):
        raise ValueError("two entry names share a hash; rename one")

    # Every offset is known up front: stored entries with no extra field, the index first.
    index_size = 12 + 12 * len(entries)
    offset = 30 + len(INDEX_NAME) + index_size
    data_offsets = []
    for name, size in zip(names, sizes):
        data_offsets.append(offset + 30 + len(name))
        offset += 30 + len(name) + size
    if offset > 0xFFFFFFFF:
        raise ValueError("volume is over 4 GB")
    index = b"MTCH" + struct.pack("<II", INDEX_VERSION, len(entries))
    for h, off, size, (_, src) in sorted(zip(hashes, data_offsets, sizes, entries)):
        index += struct.pack("<III", h, 0 if src is None else off, DIRECTORY_SIZE if src is None else size)

    central = []

    def write_entry(out, name: bytes, data: bytes, mtime: float, is_dir: bool) -> None:
        local_offset = out.tell()
        crc = zlib.crc32(data) & 0xFFFFFFFF
        t, d = dos_time(mtime)
        out.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 10, UTF8_FLAG, 0, t, d, crc, len(data), len(data), len(name), 0))
        out.write(name)
        out.write(data)
        attrs = (0o40755 << 16) | 0x10 if is_dir else 0o100644 << 16
        central.append(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 0x031E, 10, UTF8_FLAG, 0, t, d, crc, len(data),
                                   len(data), len(name), 0, 0, 0, 0, attrs, local_offset) + name)

    tmp = out_path + ".tmp"
    with open(tmp, "wb") as out:
        write_entry(out, INDEX_NAME.encode(), index, time.time(), False)
        for (name_str, src), name, expected in zip(entries, names, data_offsets):
            data = b"" if src is None else open(src, "rb").read()
            write_entry(out, name, data, time.time() if src is None else os.path.getmtime(src), src is None)
            assert src is None or out.tell() - len(data) == expected, name_str
        cd_start = out.tell()
        for rec in central:
            out.write(rec)
        out.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(central), len(central), out.tell() - cd_start,
                              cd_start, 0))
    os.replace(tmp, out_path)
    print(f"{out_path}: {len(entries)} entries, {os.path.getsize(out_path)} bytes")


def main() -> None:
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    folder = sys.argv[1].rstrip("/")
    try:
        pack(folder, sys.argv[2] if len(sys.argv) == 3 else folder + ".matcha")
    except ValueError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
