#!/usr/bin/env python3
"""Generate the cat mascot bitmaps from the drawing in assets/images/cat-reading.png.

The drawing carries a light gray sticker edge around the cat: the dark sleep screen shows it as the
white rim that sets the black cat off the black screen; everywhere else it is dropped.

    python3 scripts/generate_mascot_assets.py

Writes:
  src/images/CatLogo.h                      boot/sleep logo, 4-level gray, 2 bpp, plus the
                                            1 bpp footprint the dark sleep screen keeps lit
  src/components/icons/endOfBookCat.h       end-of-book graphic, 1 bpp drawIcon format
  src/network/html/assets/matcha-icon.svg   web header and favicon
"""
import base64
import io
from pathlib import Path

from PIL import Image, ImageFilter, ImageOps

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "assets" / "images" / "cat-reading.png"

LOGO_WIDTH = 137  # the cat itself is 130 px wide; the rest is room for the sticker edge
EOB_SIZE = 152  # drawIcon is square; the cat is centred in it at full height
WEB_SIZE = 128
# The sticker edge is drawn at gray 238; the cat's own whites are 255 and its ink near 0.
EDGE_LEVEL = 230


def shrink(art, width, height):
    """Per-pixel ink coverage, and whether each pixel lies on the cat's black body or on paper."""
    coverage = art.resize((width, height), Image.BOX)
    # Blurring by a few output pixels erases the thin lines and leaves only large dark areas.
    blur = art.size[0] / width * 1.5
    body = art.filter(ImageFilter.GaussianBlur(blur)).resize((width, height), Image.BOX)
    return coverage, body


def is_ink(value, on_body):
    # A single threshold loses either the thin white lines on the body (eye ring, claws) or the thin
    # black ones on paper (page lines, kana). Each area keeps the lines that contrast with it.
    return value < 70 if on_body else value < 215


def atkinson(gray):
    """1-bit Atkinson dither: smooth edges without the grain Floyd-Steinberg leaves on line art."""
    w, h = gray.size
    px = [float(v) for v in gray.tobytes()]
    out = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            old = px[y * w + x]
            new = 0 if old < 128 else 255
            out[y * w + x] = new
            err = (old - new) / 8
            for dx, dy in ((1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)):
                if 0 <= x + dx < w and y + dy < h:
                    px[(y + dy) * w + x + dx] += err
    return Image.frombytes("L", (w, h), bytes(out))


def bw(art, width, height):
    """The drawing at 1 bpp: dithered outline, detail rules inside it."""
    coverage, body = shrink(art, width, height)
    inside = Image.new("L", (width, height), 255)
    for i, (v, b) in enumerate(zip(coverage.tobytes(), body.tobytes())):
        if is_ink(v, b < 128):
            inside.putpixel((i % width, i // width), 0)
    # Per-pixel rules make the outer outline bumpy where body and paper meet; there the dither of
    # the plain shrink follows the curve. 2 px inside the traced silhouette the detail rules take
    # over again.
    edge = atkinson(art.resize((width, height), Image.LANCZOS))
    scale = art.size[0] / width
    silhouette = _fill_holes(art.point(lambda v: 255 if v < 128 else 0))
    silhouette = silhouette.filter(ImageFilter.GaussianBlur(scale * 0.5)).resize((width, height), Image.BOX)
    interior = silhouette.point(lambda v: 255 if v >= 128 else 0).filter(ImageFilter.MinFilter(5))
    return Image.composite(inside, edge, interior)


def build_logo(art, edged):
    height = round(art.size[1] * LOGO_WIDTH / art.size[0])
    coverage, _ = shrink(art, LOGO_WIDTH, height)
    ink = bw(art, LOGO_WIDTH, height)
    # The 1-bit boot logo draws levels 0-1 as ink, so ink/paper is decided by bw(); the level inside
    # each half only softens the sleep screen's grayscale rendering.
    levels = []
    for v, b in zip(coverage.tobytes(), ink.tobytes()):
        if b < 128:
            levels.append(0 if v < 64 else 1)
        else:
            levels.append(3 if v > 192 else 2)
    packed = bytearray()
    for i in range(0, len(levels), 4):
        chunk = levels[i : i + 4] + [3] * (4 - len(levels[i : i + 4]))
        packed.append(chunk[0] << 6 | chunk[1] << 4 | chunk[2] << 2 | chunk[3])
    # Footprint: the cat, the books and the sticker edge around them.
    footprint = _fill_holes(edged.point(lambda v: 255 if v < 250 else 0)).resize((LOGO_WIDTH, height), Image.BOX)
    footprint = footprint.point(lambda v: 255 if v >= 128 else 0)
    row_bytes = (LOGO_WIDTH + 7) // 8
    mask = bytearray(row_bytes * height)
    for y in range(height):
        for x in range(LOGO_WIDTH):
            if footprint.getpixel((x, y)):
                mask[y * row_bytes + (x >> 3)] |= 0x80 >> (x & 7)
    return LOGO_WIDTH, height, packed, mask


def build_end_of_book(art):
    width = round(art.size[0] * EOB_SIZE / art.size[1])
    small = bw(art, width, EOB_SIZE)
    square = Image.new("L", (EOB_SIZE, EOB_SIZE), 255)
    square.paste(small, ((EOB_SIZE - width) // 2, 0))
    # drawIcon plots stored (row, col) at screen (size - 1 - row, col): rows run right to left.
    row_bytes = (EOB_SIZE + 7) // 8
    packed = bytearray([0xFF] * (row_bytes * EOB_SIZE))
    for row in range(EOB_SIZE):
        for col in range(EOB_SIZE):
            if square.getpixel((EOB_SIZE - 1 - row, col)) < 128:
                packed[row * row_bytes + (col >> 3)] &= ~(0x80 >> (col & 7)) & 0xFF
    return packed


def build_web_icon(art):
    scale = WEB_SIZE / max(art.size)
    small = art.resize((round(art.size[0] * scale), round(art.size[1] * scale)), Image.LANCZOS)
    # Black ink on a transparent background; the white book and face stay opaque white.
    rgba = Image.new("RGBA", small.size)
    rgba.putdata([(v, v, v, 255) for v in small.tobytes()])
    mask = ImageOps.invert(small).point(lambda v: 255 if v > 8 else 0)
    outside = Image.new("L", small.size, 0)
    outside.paste(255, mask=_fill_holes(mask))
    rgba.putalpha(outside)
    icon = Image.new("RGBA", (WEB_SIZE, WEB_SIZE), (0, 0, 0, 0))
    icon.paste(rgba, ((WEB_SIZE - small.size[0]) // 2, (WEB_SIZE - small.size[1]) // 2))
    buf = io.BytesIO()
    icon.save(buf, "PNG", optimize=True)
    data = base64.b64encode(buf.getvalue()).decode()
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{WEB_SIZE}" height="{WEB_SIZE}" '
        f'viewBox="0 0 {WEB_SIZE} {WEB_SIZE}"><image width="{WEB_SIZE}" height="{WEB_SIZE}" '
        f'href="data:image/png;base64,{data}"/></svg>\n'
    )


def _fill_holes(ink):
    """The drawing's footprint: ink plus everything it encloses (the white book, face, ears)."""
    w, h = ink.size
    px = ink.load()
    outside = bytearray(w * h)
    stack = [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while stack:
        x, y = stack.pop()
        if 0 <= x < w and 0 <= y < h and not outside[y * w + x] and px[x, y] == 0:
            outside[y * w + x] = 1
            stack.extend(((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)))
    return Image.frombytes("L", (w, h), bytes(0 if o else 255 for o in outside))


def c_array(data, per_line=19):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i : i + per_line]) + ",")
    return "\n".join(lines)


def main():
    edged = Image.open(SOURCE).convert("L")
    art = edged.point(lambda v: 255 if v >= EDGE_LEVEL else v)
    width, height, logo, mask = build_logo(art, edged)
    (ROOT / "src" / "images" / "CatLogo.h").write_text(
        "#pragma once\n#include <cstdint>\n\n"
        f"// {width}x{height} cat logo, 4-level grayscale, 2 bits/px packed MSB-first (4 px/byte),\n"
        "// row-major, stored upright. Levels: 0 = black, 1 = dark gray, 2 = light gray, 3 = white.\n"
        "// Generated by scripts/generate_mascot_assets.py from assets/images/cat-reading.png.\n"
        f"static constexpr int CAT_LOGO_WIDTH = {width};\n"
        f"static constexpr int CAT_LOGO_HEIGHT = {height};\n"
        f"static const uint8_t CatLogoGray[] = {{\n{c_array(logo)}\n}};\n\n"
        "// The cat and books plus a white edge around them, 1 bit/px MSB-first, rows padded to bytes.\n"
        f"static const uint8_t CatLogoFootprint[] = {{\n{c_array(mask)}\n}};\n"
    )
    (ROOT / "src" / "components" / "icons" / "endOfBookCat.h").write_text(
        "#pragma once\n#include <cstdint>\n\n"
        "// Reading cat for the end-of-book screen, 1 bpp, drawn with GfxRenderer::drawIcon.\n"
        "// Generated by scripts/generate_mascot_assets.py from assets/images/cat-reading.png.\n"
        f"static constexpr int END_OF_BOOK_CAT_SIZE = {EOB_SIZE};\n"
        f"static const uint8_t EndOfBookCat[] = {{\n{c_array(build_end_of_book(art))}\n}};\n"
    )
    (ROOT / "src" / "network" / "html" / "assets" / "matcha-icon.svg").write_text(build_web_icon(art))


if __name__ == "__main__":
    main()
