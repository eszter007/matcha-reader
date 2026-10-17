#!/usr/bin/env python3
"""Generate the cat mascot bitmaps from the drawing in assets/images/cat-reading.png.

The drawing and everything generated from it is all rights reserved, not MIT: see
assets/images/LICENSE.

The drawing carries a light gray sticker edge around the cat: the dark sleep screen shows it as the
white rim that sets the black cat off the black screen; everywhere else it is dropped.

    python3 scripts/generate_mascot_assets.py [--stamp-fonts DIR]

Writes:
  src/images/CatLogo.h                      boot/sleep logo and a smaller copy, 4-level gray with the footprint the
                                            dark sleep screen keeps lit, run-length coded
  src/components/icons/endOfBookCat.h       end-of-book graphic, 1 bpp drawIcon format
  src/network/html/assets/matcha-icon.svg   web header and favicon
  src/components/icons/eobStamp.h           the end-of-book hanko, one per wording, run-length
                                            coded (only with --stamp-fonts: a folder holding
                                            NotoSerif{JP,SC,TC,HK}.ttf, SIL OFL)
"""
import argparse
import base64
import io
import random
import zlib
from pathlib import Path

from PIL import Image, ImageFilter, ImageOps

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "assets" / "images" / "cat-reading.png"

LOGO_WIDTH = 230  # sticker edge included; the poster-size cat of the boot and sleep screens
SMALL_LOGO_WIDTH = 130  # the Departures sleep screen's cat on screens too short for the full size
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


def build_logo(art, edged, width=LOGO_WIDTH):
    height = round(art.size[1] * width / art.size[0])
    coverage, _ = shrink(art, width, height)
    ink = bw(art, width, height)
    # The 1-bit boot logo draws levels 0-1 as ink, so ink/paper is decided by bw(); the level inside
    # each half only softens the sleep screen's grayscale rendering.
    levels = []
    for v, b in zip(coverage.tobytes(), ink.tobytes()):
        if b < 128:
            levels.append(0 if v < 64 else 1)
        else:
            levels.append(3 if v > 192 else 2)
    # Footprint: the cat, the books and the sticker edge around them.
    footprint = _fill_holes(edged.point(lambda v: 255 if v < 250 else 0)).resize((width, height), Image.BOX)
    inside = [v >= 128 for v in footprint.tobytes()]
    # Run-length coded, row-major and continuous across rows: each byte is a 3-bit symbol
    # (footprint << 2 | level) and a run of 1-32 pixels. The drawing is mostly flat black and
    # white, so this is about a quarter of 2 bpp plus a 1 bpp mask.
    symbols = [level | (int(f) << 2) for level, f in zip(levels, inside)]
    rle = bytearray()
    i = 0
    while i < len(symbols):
        run = 1
        while i + run < len(symbols) and symbols[i + run] == symbols[i] and run < 32:
            run += 1
        rle.append(symbols[i] << 5 | (run - 1))
        i += run
    return width, height, rle


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
        "<!-- The Matcha cat: all rights reserved, not MIT -- see assets/images/LICENSE. -->\n"
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


STAMP_SIZE = 180


# The stamp's wording per book language, each lettered in the Noto Serif cut for its region so the
# glyphs take that region's forms. Japanese and every non-Chinese book get 読了.
STAMPS = (
    ("EobStampJa", "読了", "NotoSerifJP.ttf"),
    ("EobStampZhHans", "读完", "NotoSerifSC.ttf"),
    ("EobStampZhHant", "讀完", "NotoSerifTC.ttf"),
    ("EobStampYue", "睇完", "NotoSerifHK.ttf"),
)


def build_stamp(font_path, text):
    """A hanko: a double ring and two characters, worn like real ink. The date under them is drawn
    by the firmware; the space for it is left empty. Returns the run-length coded bitmap: each byte
    is (ink << 7) | (run - 1), runs of 1-128 pixels in row-major order, continuing across rows."""
    from PIL import ImageDraw, ImageFont

    scale = 4
    size = STAMP_SIZE * scale
    img = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(img)
    draw.ellipse([4 * scale, 4 * scale, size - 4 * scale, size - 4 * scale], outline=255, width=6 * scale)
    inner = 17 * scale
    draw.ellipse([inner, inner, size - inner, size - inner], outline=255, width=2 * scale)
    font = ImageFont.truetype(str(font_path), int(size * 0.30))
    font.set_variation_by_axes([700])
    # Centred by the glyphs' ink, not the font box. With the date drawn under them (at 60% height),
    # the two lines sit as one block in the middle of the ring.
    left, top, right, bottom = draw.textbbox((0, 0), text, font=font)
    x = (size - (right - left)) / 2 - left
    y = size * 0.40 - (bottom - top) / 2 - top
    draw.text((x, y), text, fill=255, font=font)
    img = img.resize((STAMP_SIZE, STAMP_SIZE), Image.LANCZOS).point(lambda v: 255 if v > 110 else 0)
    # Worn ink: a fixed scatter of missing specks, the same on every build.
    rng = random.Random(3)
    px = img.load()
    for _ in range(STAMP_SIZE * STAMP_SIZE // 18):
        px[rng.randrange(STAMP_SIZE), rng.randrange(STAMP_SIZE)] = 0
    bits = [1 if v else 0 for v in img.tobytes()]
    rle = bytearray()
    i = 0
    while i < len(bits):
        run = 1
        while i + run < len(bits) and bits[i + run] == bits[i] and run < 128:
            run += 1
        rle.append(bits[i] << 7 | (run - 1))
        i += run
    return rle


def deflate(data):
    compressor = zlib.compressobj(level=9, wbits=-15)
    return compressor.compress(bytes(data)) + compressor.flush()


def c_array(data, per_line=19):
    lines = []
    for i in range(0, len(data), per_line):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i : i + per_line]) + ",")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stamp-fonts", type=Path, help="folder with NotoSerif{JP,SC,TC,HK}.ttf")
    args = parser.parse_args()
    if args.stamp_fonts:
        arrays = []
        for name, text, font in STAMPS:
            rle = build_stamp(args.stamp_fonts / font, text)
            arrays.append(f"// {text}, lettered in {font[:-4]}\nstatic const uint8_t {name}[] = {{\n{c_array(rle)}\n}};\n")
        (ROOT / "src" / "components" / "icons" / "eobStamp.h").write_text(
            "#pragma once\n#include <cstdint>\n\n"
            "// The end-of-book hanko, one per wording. Run-length coded, row-major and upright: each byte is\n"
            "// (ink << 7) | (run - 1), runs of 1-128 pixels continuing across rows. Lettered in Noto Serif\n"
            "// (SIL Open Font License); the date inside is drawn at runtime.\n"
            "// Generated by scripts/generate_mascot_assets.py --stamp-fonts.\n"
            f"static constexpr int EOB_STAMP_SIZE = {STAMP_SIZE};\n\n" + "\n".join(arrays)
        )
    edged = Image.open(SOURCE).convert("L")
    art = edged.point(lambda v: 255 if v >= EDGE_LEVEL else v)
    width, height, logo = build_logo(art, edged)
    small_width, small_height, small_logo = build_logo(art, edged, SMALL_LOGO_WIDTH)
    (ROOT / "src" / "images" / "CatLogo.h").write_text(
        "#pragma once\n#include <cstdint>\n\n"
        f"// {width}x{height} cat logo, run-length coded: each byte is (symbol << 5) | (run - 1), runs of 1-32\n"
        "// pixels in row-major order, continuing across rows. symbol = footprint << 2 | level, where\n"
        "// level 0 = black, 1 = dark gray, 2 = light gray, 3 = white, and footprint marks the cat, the books\n"
        "// and the sticker edge around them, which the dark sleep screen keeps lit.\n"
        "// Generated by scripts/generate_mascot_assets.py from assets/images/cat-reading.png.\n"
        "// The Matcha cat: all rights reserved, not MIT -- see assets/images/LICENSE.\n"
        f"static constexpr int CAT_LOGO_WIDTH = {width};\n"
        f"static constexpr int CAT_LOGO_HEIGHT = {height};\n"
        f"static const uint8_t CatLogoRle[] = {{\n{c_array(logo)}\n}};\n\n"
        "// The same cat at a smaller size, coded the same way and then raw-deflated: it is drawn only by\n"
        "// the Departures sleep screen, which inflates it into a CAT_LOGO_SMALL_RLE_SIZE buffer.\n"
        f"static constexpr int CAT_LOGO_SMALL_WIDTH = {small_width};\n"
        f"static constexpr int CAT_LOGO_SMALL_HEIGHT = {small_height};\n"
        f"static constexpr int CAT_LOGO_SMALL_RLE_SIZE = {len(small_logo)};\n"
        f"static const uint8_t CatLogoSmallDeflated[] = {{\n{c_array(deflate(small_logo))}\n}};\n"
    )
    (ROOT / "src" / "components" / "icons" / "endOfBookCat.h").write_text(
        "#pragma once\n#include <cstdint>\n\n"
        "// Reading cat for the end-of-book screen, 1 bpp, drawn with GfxRenderer::drawIcon.\n"
        "// Generated by scripts/generate_mascot_assets.py from assets/images/cat-reading.png.\n"
        "// The Matcha cat: all rights reserved, not MIT -- see assets/images/LICENSE.\n"
        f"static constexpr int END_OF_BOOK_CAT_SIZE = {EOB_SIZE};\n"
        f"static const uint8_t EndOfBookCat[] = {{\n{c_array(build_end_of_book(art))}\n}};\n"
    )
    (ROOT / "src" / "network" / "html" / "assets" / "matcha-icon.svg").write_text(build_web_icon(art))


if __name__ == "__main__":
    main()
