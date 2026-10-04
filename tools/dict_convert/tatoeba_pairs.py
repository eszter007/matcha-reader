#!/usr/bin/env python3
"""Join Tatoeba's per-language exports into the sentence-pair file convert_jmdict.py --examples reads.

Tatoeba publishes sentences per language and links between them; the ready-made "sentence pairs"
download needs a browser form. This joins the three public files instead:

    https://downloads.tatoeba.org/exports/per_language/cmn/cmn_sentences.tsv.bz2
    https://downloads.tatoeba.org/exports/per_language/eng/eng_sentences.tsv.bz2
    https://downloads.tatoeba.org/exports/per_language/cmn/cmn-eng_links.tsv.bz2

Usage:
    python3 tatoeba_pairs.py cmn_sentences.tsv.bz2 eng_sentences.tsv.bz2 cmn-eng_links.tsv.bz2 pairs.tsv

Output: one "sentence<TAB>translation" line per linked pair, Chinese first, the shortest English
translation when several exist. Licence: CC BY 2.0 FR (https://tatoeba.org).
"""

import bz2
import sys


def open_text(path):
    return bz2.open(path, "rt", encoding="utf-8") if path.endswith(".bz2") else open(path, "r", encoding="utf-8")


def load_sentences(path: str) -> dict:
    out = {}
    with open_text(path) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                out[parts[0]] = parts[2]
    return out


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    cmn_path, eng_path, links_path, out_path = sys.argv[1:]
    cmn = load_sentences(cmn_path)
    print(f"{len(cmn):,} Chinese sentences")
    eng = load_sentences(eng_path)
    print(f"{len(eng):,} English sentences")
    best = {}
    with open_text(links_path) as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2:
                continue
            zh, en = cmn.get(parts[0]), eng.get(parts[1])
            if not zh or not en:
                continue
            if zh not in best or len(en) < len(best[zh]):
                best[zh] = en
    with open(out_path, "w", encoding="utf-8") as f:
        for zh, en in best.items():
            f.write(f"{zh}\t{en}\n")
    print(f"Wrote {len(best):,} pairs to {out_path}")


if __name__ == "__main__":
    main()
