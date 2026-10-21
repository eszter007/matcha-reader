"""Regroup the compressed glyph groups of a generated built-in font header in place.

Used for headers whose source font is not in the repository (the Joyo CJK fallbacks), so
fontconvert.py cannot regenerate them. Glyph bitmaps and metrics are unchanged; only the group
boundaries, the group table and the compressed stream are rewritten, with the same caps
fontconvert.py applies. Every glyph is checked to round-trip before the file is written.

Usage: python regroup_font_header.py ../builtinFonts/notosansjp_joyo_12_regular.h
"""

import re
import sys
import zlib

import zopfli.zlib

# Must match SCRIPT_GROUP_RANGES and the group caps in fontconvert.py (2-bit fonts).
SCRIPT_GROUP_RANGES = [
    (0x0000, 0x007F), (0x0080, 0x00FF), (0x0100, 0x017F), (0x0180, 0x024F), (0x0300, 0x036F),
    (0x0400, 0x04FF), (0x1EA0, 0x1EF9), (0x2000, 0x206F), (0x2070, 0x209F), (0x20A0, 0x20CF),
    (0x2190, 0x21FF), (0x2200, 0x22FF), (0xFB00, 0xFB06), (0xFFFD, 0xFFFD),
]
GROUP_MAX_UNCOMPRESSED_BYTES = 16384
OTHER_SCRIPT_GROUP_MAX_BYTES = 4096


def script_group(cp):
    return next((i for i, (a, b) in enumerate(SCRIPT_GROUP_RANGES) if a <= cp <= b), -1)


def main(path):
    src = open(path, encoding="utf-8").read()
    name = re.search(r"static const uint8_t (\w+)Bitmaps\[", src).group(1)

    def block(tag):
        return re.search(r"(static const \w+ " + name + tag + r"\[\d*\] = \{)(.*?)(\n\};)", src, re.S)

    bm = block("Bitmaps")
    bitmaps = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", bm.group(2)))
    gm = block("Glyphs")
    glines = gm.group(2).strip("\n").split("\n")
    glyphs = [list(map(int, re.match(r"\s*\{ ([^}]*) \}", l).group(1).split(","))) for l in glines]
    cps = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\b\d+\b", block("Codepoints").group(2))]
    grm = block("Groups")
    groups = [list(map(int, r.split(","))) for r in re.findall(r"\{ ([^}]*) \}", grm.group(2))]
    if len(cps) != len(glyphs):
        sys.exit(f"{path}: {len(cps)} codepoints but {len(glyphs)} glyphs")

    # Glyph fields: width, height, ..., dataLength (5), dataOffset (6). Groups hold row-aligned rows.
    def aligned(g):
        return ((g[0] + 3) // 4) * g[1]

    per = [None] * len(glyphs)
    for off, csz, usz, cnt, first in groups:
        data = zlib.decompress(bitmaps[off:off + csz], -15)
        p = 0
        for gi in range(first, first + cnt):
            per[gi] = data[p:p + aligned(glyphs[gi])]
            p += len(per[gi])
        if p != usz or len(data) != usz:
            sys.exit(f"{path}: group at {off} does not match its glyphs")

    new_groups, cur, start, size = [], None, 0, 0
    for i, cp in enumerate(cps):
        sg = script_group(cp)
        cap = OTHER_SCRIPT_GROUP_MAX_BYTES if sg == -1 else GROUP_MAX_UNCOMPRESSED_BYTES
        if i == 0 or sg != cur or size + len(per[i]) > cap:
            if i > 0:
                new_groups.append((start, i - start))
            cur, start, size = sg, i, 0
        size += len(per[i])
    new_groups.append((start, len(cps) - start))

    out_bm, out_groups = bytearray(), []
    for first, cnt in new_groups:
        raw = b"".join(per[first:first + cnt])
        packed = 0
        for gi in range(first, first + cnt):
            glyphs[gi][6] = packed
            packed += glyphs[gi][5]
        comp = zopfli.zlib.compress(raw)[2:-4]  # strip the zlib header and Adler-32: raw DEFLATE
        if zlib.decompress(comp, -15) != raw:
            sys.exit(f"{path}: raw DEFLATE round-trip failed")
        out_groups.append((len(out_bm), len(comp), len(raw), cnt, first))
        out_bm += comp

    bm_text = "\n" + "\n".join(
        "    " + " ".join(f"0x{b:02X}," for b in out_bm[i:i + 16]) for i in range(0, len(out_bm), 16))
    gl_text = "\n" + "\n".join(
        re.sub(r"\{ [^}]* \}", "{ " + ", ".join(map(str, g)) + " }", l, count=1) for l, g in zip(glines, glyphs))
    gr_text = "\n" + "\n".join("    { " + ", ".join(map(str, g)) + " }," for g in out_groups)
    out = src.replace(bm.group(0), f"static const uint8_t {name}Bitmaps[{len(out_bm)}] = {{" + bm_text + "\n};")
    out = out.replace(gm.group(0), gm.group(1) + gl_text + gm.group(3))
    out = out.replace(grm.group(0), grm.group(1) + gr_text + grm.group(3))
    count = f"    {name}Groups,\n    {len(groups)},"
    if out.count(count) != 1:
        sys.exit(f"{path}: group count field not found")
    out = out.replace(count, f"    {name}Groups,\n    {len(out_groups)},")
    open(path, "w", encoding="utf-8").write(out)
    print(f"{name}: {len(groups)} -> {len(out_groups)} groups, "
          f"largest {max(g[2] for g in groups)} -> {max(g[2] for g in out_groups)} B, "
          f"compressed {len(bitmaps)} -> {len(out_bm)} B")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
