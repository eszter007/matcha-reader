#!/usr/bin/env python3
"""Writes the clip action bar's icon sources (*-clip.svg) from Tabler outline icons.

Each icon is cropped to its ink and scaled so all four share one optical size, with a stroke that
renders STROKE_PX wide at PX. Then run gen_icons.py (see clipActionIcons.manifest).

Usage: balance_clip_icons.py <tabler>/icons/outline <out dir>
Requires: Pillow and resvg-py.
"""
import io
import os
import re
import sys

import resvg_py
from PIL import Image

PX = 32
STROKE_PX = 3
# Largest ink dimension as a share of the canvas. Round and wide shapes read bigger, so they get less.
FILL = {"search": 0.80, "language": 0.80, "quote": 0.70, "bookmark": 0.84}


def paths(svg_dir, name):
    s = open(os.path.join(svg_dir, name + ".svg")).read()
    return "".join(p for p in re.findall(r"<path [^>]*/>", s) if 'stroke="none"' not in p)


def main():
    svg_dir = sys.argv[1]
    out_dir = sys.argv[2]
    for name, fill in FILL.items():
        body = paths(svg_dir, name)
        # Ink box of the centre lines, from a hairline render at 40x.
        probe = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="960" height="960" '
                 f'fill="none" stroke="#000" stroke-width="0.05">{body}</svg>')
        img = Image.open(io.BytesIO(bytes(resvg_py.svg_to_bytes(svg_string=probe)))).convert("RGBA")
        x0, y0, x1, y1 = (v / 40 for v in img.split()[3].point(lambda v: 255 if v > 20 else 0).getbbox())
        side = 24.0
        for _ in range(20):  # the stroke's own width depends on the scale
            side = (max(x1 - x0, y1 - y0) + STROKE_PX * side / PX) / fill
        stroke = STROKE_PX * side / PX
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        with open(os.path.join(out_dir, name + "-clip.svg"), "w") as f:
            f.write(f'<!-- Tabler "{name}" (MIT), made by scripts/balance_clip_icons.py; regenerate rather than edit. -->\n')
            f.write(f'<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" '
                    f'viewBox="{cx - side / 2:.3f} {cy - side / 2:.3f} {side:.3f} {side:.3f}" fill="none" '
                    f'stroke="currentColor" stroke-width="{stroke:.3f}" stroke-linecap="round" '
                    f'stroke-linejoin="round">\n  {body}\n</svg>\n')


if __name__ == "__main__":
    main()
