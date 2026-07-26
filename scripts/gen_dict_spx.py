#!/usr/bin/env python3
"""Generate two-tier sparse-index sidecars (.spx) for the dictionary .idx files.

Each dictionary index (jmdict.idx, grammar.idx, jmnedict.idx) is a flat array of
40-byte records sorted by a 32-byte headword field. A binary search over it costs
~log2(N) scattered SD reads (~20 for the 600K+ record dicts). On the ESP32-C3 each
SD read is a ~4ms transaction, so building the Word Lookup screen — which does
thousands of lookups per page — takes minutes.

The .spx sidecar is a coarse "checkpoint" layer: one 32-byte key per STRIDE records
(key = the headword of record i*STRIDE). At runtime the firmware keeps a sub-sample
of these keys in RAM (the coarse tier), binary-searches that to bracket the key into
a small on-disk window, reads that window from the .spx once, then reads a single
STRIDE-record block from the .idx — turning ~20 scattered reads into ~2.

The firmware validates idxRecordCount against the live .idx size and silently falls
back to a full binary search if the sidecar is missing or stale, so this is purely
additive: no behaviour change if the .spx files are absent.

Usage:
    python3 gen_dict_spx.py /path/to/sdcard/dict
"""

import os
import struct
import sys

RECORD_SIZE = 40
HEADWORD_SIZE = 32
STRIDE = 48  # idx records per fine-tier checkpoint

MAGIC = b"CPSPX1\0\0"          # 8 bytes
SPX_VERSION = 1
HEADER_SIZE = 32              # magic(8) + 5x uint32(20) + pad(4)

# Preferred names first, legacy names (jmdict/jmnedict) after -- whichever .idx files exist in
# the dict dir get a sidecar; the firmware derives the .spx name from the .idx it resolved.
DICTS = ["vocab", "names", "grammar", "jmdict", "jmnedict"]


def gen_one(idx_path, spx_path):
    size = os.path.getsize(idx_path)
    if size % RECORD_SIZE != 0:
        raise SystemExit(f"{idx_path}: size {size} not a multiple of {RECORD_SIZE}")
    count = size // RECORD_SIZE
    fine_count = (count + STRIDE - 1) // STRIDE

    header = MAGIC + struct.pack("<IIIII", SPX_VERSION, STRIDE, count, fine_count, 0)
    header += b"\0" * (HEADER_SIZE - len(header))
    assert len(header) == HEADER_SIZE, len(header)

    with open(idx_path, "rb") as fin, open(spx_path, "wb") as fout:
        fout.write(header)
        # Sequential scan: read the idx in big chunks, emit the headword of every
        # STRIDE-th record. Reading whole blocks and slicing is far faster than
        # seeking to each checkpoint individually.
        CHUNK_RECORDS = 8192
        rec_index = 0
        next_checkpoint = 0
        emitted = 0
        while rec_index < count:
            n = min(CHUNK_RECORDS, count - rec_index)
            buf = fin.read(n * RECORD_SIZE)
            if len(buf) != n * RECORD_SIZE:
                raise SystemExit(f"{idx_path}: short read at record {rec_index}")
            while next_checkpoint < rec_index + n:
                off = (next_checkpoint - rec_index) * RECORD_SIZE
                fout.write(buf[off:off + HEADWORD_SIZE])
                emitted += 1
                next_checkpoint += STRIDE
            rec_index += n
    assert emitted == fine_count, (emitted, fine_count)
    return count, fine_count


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: gen_dict_spx.py /path/to/sdcard/dict")
    dict_dir = sys.argv[1]
    for name in DICTS:
        idx_path = os.path.join(dict_dir, f"{name}.idx")
        if not os.path.exists(idx_path):
            print(f"skip {name}: no {idx_path}")
            continue
        spx_path = os.path.join(dict_dir, f"{name}.spx")
        count, fine_count = gen_one(idx_path, spx_path)
        spx_size = os.path.getsize(spx_path)
        print(f"{name}: {count} records -> {fine_count} checkpoints "
              f"({spx_size} bytes, stride={STRIDE})")


if __name__ == "__main__":
    main()
