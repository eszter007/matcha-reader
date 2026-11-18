#!/usr/bin/env python3
"""Self-check for the built-in text recognition's pure helpers (no models, no deps).
Run: python3 test_ppocr.py
"""

import ppocr
from convert_manga import mokuro_text_blocks

failures = 0


def check(name, ok, detail=""):
    global failures
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"  {detail}"))
    if not ok:
        failures += 1


check("page fits 960 on the long side in multiples of 32", ppocr.det_size(888, 1238) == (704, 960),
      ppocr.det_size(888, 1238))
sw, sh = ppocr.det_size(800, 6000)
check("a long strip gets a 3:4 page's pixel budget", sw % 32 == 0 and sw * sh < 0.8 * 960 * 960, (sw, sh))

# A 40x40 map with one confident 4x20 vertical stroke and one faint blob.
mw = mh = 40
pred = [0.0] * (mw * mh)
for y in range(10, 30):
    for x in range(18, 22):
        pred[y * mw + x] = 0.9
for y in range(2, 5):
    for x in range(2, 5):
        pred[y * mw + x] = 0.2
boxes = ppocr.db_boxes(pred, mw, mh)
check("one box for the stroke, the faint blob dropped", len(boxes) == 1, boxes)
x1, y1, x2, y2 = boxes[0]
check("box grown by the unclip offset", x1 < 18 and x2 > 21 and y1 < 10 and y2 > 29 and x1 > 15, boxes[0])

vocab = ["", "ね", "こ", " "]
check("CTC collapses repeats and blanks", ppocr.ctc_decode([1, 1, 0, 2, 2], vocab) == "ねこ")
check("CTC keeps a repeat split by a blank", ppocr.ctc_decode([1, 0, 1], vocab) == "ねね")
check("recogniser width follows the line's shape",
      ppocr.rec_width(300, 48) == 300 and ppocr.rec_width(5, 48) == 16 and ppocr.rec_width(1000, 48) == 1000
      and ppocr.rec_width(1999, 48) < ppocr.rec_width(2001, 48) and ppocr.rec_width(9000, 48) == 3200)

check("line text: ellipsis and full-width letters normalised", ppocr.line_text(" ・・・ＯＫ１ ") == "…OK1")

blocks = ppocr.bubbles([
    {"text": "それじゃあ", "box": [370, 110, 390, 200], "vertical": True},
    {"text": "お別れだ", "box": [347, 110, 367, 185], "vertical": True},
    {"text": "019", "box": [65, 1178, 119, 1188], "vertical": False},
    {"text": "漢字の読み", "box": [300, 300, 324, 420], "vertical": True},
    {"text": "かんじ", "box": [326, 300, 336, 340], "vertical": True},
])
check("columns of one bubble merged right to left, page number and furigana dropped",
      len(blocks) == 2 and any(b["lines"] == ["それじゃあ", "お別れだ"] for b in blocks)
      and any(b["lines"] == ["漢字の読み"] for b in blocks), [b["lines"] for b in blocks])
check("blocks carry Mokuro line quads", all(len(b["lines_coords"]) == len(b["lines"]) for b in blocks))
check("block box covers its lines", blocks[0]["box"] == [347, 110, 390, 200], blocks[0]["box"])

# The page record goes through the same mapping as a Mokuro page's, line boxes included.
mapped = mokuro_text_blocks(blocks, lambda x, y: (int(x / 2), int(y / 2)))
check("blocks map to the page with a box per line",
      len(mapped) == 2 and all(len(m["lines"]) == len(b["lines"]) and ("\n" in m["text"]) == (len(m["lines"]) > 1)
                               for m, b in zip(mapped, blocks)), mapped)

if failures:
    raise SystemExit(f"\n{failures} failure(s)")
print("\nall tests passed")
