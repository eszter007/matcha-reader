"""PP-OCRv6_manga: manga text detection and recognition, Japanese and Chinese, run locally.

Models by Kellenok (https://huggingface.co/Kellenok/PP-OCRv6_manga, Apache-2.0), fine-tuned from
PaddlePaddle's PP-OCRv6 on manga (revision v0.3): a 0.9 MB line detector and a 10.7 MB recogniser
in FP16 ONNX, downloaded once into the user cache and checked against their SHA-256. Needs
onnxruntime and numpy; nothing is sent anywhere.

This is the same pipeline as the browser converter's js/ppocr.js in matcha-reader-tools, so both
produce the same text: the detector input has the long side at 960, DB post-processing uses
thresh 0.15, box_thresh 0.25 and unclip 1.5, each line is cropped, vertical ones turned 90 degrees
anticlockwise, resized to 48 px high and CTC-decoded. Manga lettering is axis-aligned, so connected
regions and their bounding boxes stand in for OpenCV's contours and rotated rectangles. The lines
are then grouped into speech bubbles and returned as a Mokuro page record, so the Mokuro mapping
and panel assignment in convert_manga.py take it from there.
"""

from __future__ import annotations

import hashlib
import math
import os
import re
import sys
import urllib.request
from pathlib import Path

PPOCR_REVISION = "v0.3"
PPOCR_URL = "https://huggingface.co/Kellenok/PP-OCRv6_manga/resolve/" + PPOCR_REVISION + "/"
# Hub path -> SHA-256, as published at the pinned revision.
PPOCR_FILES = {
    "det/manga_det_fp16.onnx": "6cfa057e437fe981810d234eb4761bbd1df35162811b30a53642dd450d587a17",
    "rec/manga_rec_fp16.onnx": "ecf4156045b2e87d45158768852afeb210c5f2e8fc4dbfdb4bb4e5836b136c8d",
    "ppocrv6_dict.txt": "4f0f8971ecb95e3a4d278985d6eaac154413e81fda0b4c941db17389580e9e9b",
}
DET_LONG_SIDE = 960
THRESH = 0.15
BOX_THRESH = 0.25
UNCLIP = 1.5  # the model card's recommended setting (its example script uses 1.4)
REC_HEIGHT = 48
REC_MAX_WIDTH = 3200
# Detector normalisation, per channel in the model's B, G, R order.
DET_MEAN = (0.485, 0.456, 0.406)
DET_STD = (0.229, 0.224, 0.225)


def _round(v: float) -> int:
    """JavaScript's Math.round (half up), so boxes match the browser converter's."""
    return int(math.floor(v + 0.5))


# ── Pure helpers (unit-tested) ───────────────────────────────────────

def det_size(w: int, h: int) -> tuple[int, int]:
    """Detector input size: long side 960 in multiples of 32; a strip longer than 2:1 gets the
    pixel budget of a 3:4 page at 960 instead."""
    if max(w, h) / min(w, h) > 2:
        ratio = min(1.0, math.sqrt((0.75 * 960 * 960) / (w * h)))
    else:
        ratio = DET_LONG_SIDE / max(w, h)
    return max(_round(w * ratio / 32) * 32, 32), max(_round(h * ratio / 32) * 32, 32)


def db_boxes(pred, mw: int, mh: int, thresh: float = THRESH, box_thresh: float = BOX_THRESH,
             unclip: float = UNCLIP) -> list[list[float]]:
    """DB post-processing on the detector's probability map (a flat row-major sequence): one
    [x1, y1, x2, y2] (map pixels) per confident text region, grown by the unclip distance the DB
    kernel shrank it by."""
    seen = bytearray(mw * mh)
    boxes = []
    for start in range(mw * mh):
        if seen[start] or pred[start] <= thresh:
            continue
        stack = [start]
        seen[start] = 1
        n, total = 0, 0.0
        x1, y1, x2, y2 = mw, mh, -1, -1
        while stack:
            i = stack.pop()
            y, x = divmod(i, mw)
            n += 1
            total += pred[i]
            x1, x2 = min(x1, x), max(x2, x)
            y1, y2 = min(y1, y), max(y2, y)
            for ny in (y - 1, y, y + 1):
                if ny < 0 or ny >= mh:
                    continue
                row = ny * mw
                for nx in (x - 1, x, x + 1):
                    if nx < 0 or nx >= mw:
                        continue
                    j = row + nx
                    if not seen[j] and pred[j] > thresh:
                        seen[j] = 1
                        stack.append(j)
        if n < 4 or total / n < box_thresh:
            continue
        bw, bh = x2 - x1, y2 - y1
        if bw <= 0 or bh <= 0:
            continue
        # PaddleOCR's unclip: a constant offset of area * ratio / perimeter.
        d = n * unclip / (2 * (bw + bh))
        box = [x1 - d, y1 - d, x2 + d, y2 + d]
        if min(box[2] - box[0], box[3] - box[1]) < 3:
            continue
        boxes.append(box)
    return boxes


def ctc_decode(best: list[int], vocab: list[str]) -> str:
    """Greedy CTC over the most likely symbol per step: repeats and blanks (index 0) dropped."""
    text = []
    prev = -1
    for c in best:
        if c != 0 and c != prev and c < len(vocab):
            text.append(vocab[c])
        prev = c
    return "".join(text)


def rec_width(w: int, h: int) -> int:
    """The recogniser's input width for a line w x h (after turning vertical lines): its natural
    width at 48 px high, capped so a page-long stroke cannot make a huge tensor."""
    natural = _round(REC_HEIGHT * w / max(1, h))
    return max(16, min(REC_MAX_WIDTH, natural))


# ── Lines into speech bubbles ────────────────────────────────────────
# The detector finds printed lines; the device wants one text block per bubble (a saved sentence
# is a block's text). The columns of a bubble share orientation and size, sit side by side with a
# gap narrower than a column and start near the same height. Furigana is the same shape at half
# the size and is dropped, as are page numbers and stray marks.

_HAS_CJK = re.compile("[぀-ヿ㐀-䶿一-鿿豈-﫿ｦ-ﾟ々〆ヵヶ]")
_KANA_ONLY = re.compile("^[぀-ヿー・]+$")
_ELLIPSIS = re.compile(r"(?:･･･|．．．|・・・|\.\.\.)")
_FULLWIDTH_ALNUM = re.compile("[０-９Ａ-Ｚａ-ｚ]")


def line_text(text: str) -> str:
    text = _ELLIPSIS.sub("…", str(text).strip())
    return _FULLWIDTH_ALNUM.sub(lambda m: chr(ord(m.group(0)) - 0xFEE0), text)


def _thickness(line: dict) -> float:
    b = line["box"]
    return b[2] - b[0] if line["vertical"] else b[3] - b[1]


def _overlap(a1, a2, b1, b2) -> float:
    return max(0, min(a2, b2) - max(a1, b1))


def _is_furigana(line: dict, others: list[dict]) -> bool:
    if not _KANA_ONLY.match(line["text"].replace(" ", "")):
        return False
    t = _thickness(line)
    lx1, ly1, lx2, ly2 = line["box"]
    for o in others:
        if o is line or o["vertical"] != line["vertical"] or _thickness(o) < 1.6 * t:
            continue
        ox1, oy1, ox2, oy2 = o["box"]
        if line["vertical"]:
            if (_overlap(ly1, ly2, oy1, oy2) > 0.5 * (ly2 - ly1)
                    and (abs(lx1 - ox2) < 0.6 * t or abs(ox1 - lx2) < 0.6 * t or _overlap(lx1, lx2, ox1, ox2) > 0)):
                return True
        elif (_overlap(lx1, lx2, ox1, ox2) > 0.5 * (lx2 - lx1)
              and (abs(ly2 - oy1) < 0.6 * t or abs(oy2 - ly1) < 0.6 * t or _overlap(ly1, ly2, oy1, oy2) > 0)):
            return True
    return False


def _same_bubble(a: dict, b: dict) -> bool:
    if a["vertical"] != b["vertical"]:
        return False
    ta, tb = _thickness(a), _thickness(b)
    if tb <= 0 or not 0.7 <= ta / tb <= 1.43:
        return False
    t = max(ta, tb)
    ax1, ay1, ax2, ay2 = a["box"]
    bx1, by1, bx2, by2 = b["box"]
    if a["vertical"]:
        return (max(bx1 - ax2, ax1 - bx2) < 0.75 * t and abs(ay1 - by1) < 1.5 * t
                and _overlap(ay1, ay2, by1, by2) > 0.3 * min(ay2 - ay1, by2 - by1))
    return (max(by1 - ay2, ay1 - by2) < 0.75 * t and abs(ax1 - bx1) < 1.5 * t
            and _overlap(ax1, ax2, bx1, bx2) > 0.3 * min(ax2 - ax1, bx2 - bx1))


def _same_column(a: dict, b: dict) -> bool:
    """Pieces of one printed line, split where the detector lost a mark such as 〝 〟: same
    orientation and size, one in line with the other, and close along it."""
    if a["vertical"] != b["vertical"]:
        return False
    ta, tb = _thickness(a), _thickness(b)
    if tb <= 0 or not 0.7 <= ta / tb <= 1.43:
        return False
    t = max(ta, tb)
    ax1, ay1, ax2, ay2 = a["box"]
    bx1, by1, bx2, by2 = b["box"]
    if a["vertical"]:
        return _overlap(ax1, ax2, bx1, bx2) > 0.6 * min(ta, tb) and max(by1 - ay2, ay1 - by2) < 0.6 * t
    return _overlap(ay1, ay2, by1, by2) > 0.6 * min(ta, tb) and max(bx1 - ax2, ax1 - bx2) < 0.6 * t


def join_columns(lines: list[dict]) -> list[dict]:
    """Merge the pieces of a printed line back into one line, read top to bottom (or left to
    right)."""
    lines = [dict(l) for l in lines]
    merged = True
    while merged:
        merged = False
        for i in range(len(lines)):
            for j in range(i + 1, len(lines)):
                a, b = lines[i], lines[j]
                if not _same_column(a, b):
                    continue
                axis = 1 if a["vertical"] else 0
                first, second = (a, b) if a["box"][axis] <= b["box"][axis] else (b, a)
                lines[i] = {"text": first["text"] + second["text"], "vertical": a["vertical"],
                            "box": [min(a["box"][0], b["box"][0]), min(a["box"][1], b["box"][1]),
                                    max(a["box"][2], b["box"][2]), max(a["box"][3], b["box"][3])]}
                del lines[j]
                merged = True
                break
            if merged:
                break
    return lines


def reading_order(blocks: list[dict], rtl: bool = True) -> list[dict]:
    """Blocks in reading order: rows of blocks that sit side by side, top to bottom, each row
    right to left (left to right when not rtl)."""
    rows: list[list] = []  # [top, bottom, blocks]
    for b in sorted(blocks, key=lambda b: b["box"][1]):
        y1, y2 = b["box"][1], b["box"][3]
        for row in rows:
            if _overlap(y1, y2, row[0], row[1]) > 0.5 * min(y2 - y1, row[1] - row[0]):
                row[0], row[1] = min(row[0], y1), max(row[1], y2)
                row[2].append(b)
                break
        else:
            rows.append([y1, y2, [b]])
    out = []
    for _, _, row in sorted(rows, key=lambda r: r[0]):
        out.extend(sorted(row, key=(lambda b: -b["box"][2]) if rtl else (lambda b: b["box"][0])))
    return out


def bubbles(lines: list[dict], rtl: bool = True) -> list[dict]:
    """lines: [{"text", "box": [x1, y1, x2, y2], "vertical"}] -> a Mokuro page record's blocks, in
    reading order (right to left unless rtl is False)."""
    kept = join_columns([dict(l, text=line_text(l["text"])) for l in lines])
    kept = [l for l in kept if l["text"] and (_HAS_CJK.search(l["text"]) or len(l["text"]) >= 4)]
    rows = [l for l in kept if not _is_furigana(l, kept)]
    parent = list(range(len(rows)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i in range(len(rows)):
        for j in range(i + 1, len(rows)):
            if _same_bubble(rows[i], rows[j]):
                parent[find(i)] = find(j)
    groups: dict[int, list[dict]] = {}
    for i, l in enumerate(rows):
        groups.setdefault(find(i), []).append(l)
    blocks = []
    for group in groups.values():
        vertical = group[0]["vertical"]
        # Columns right to left; rows top to bottom.
        group.sort(key=(lambda l: -l["box"][2]) if vertical else (lambda l: l["box"][1]))
        blocks.append({
            "box": [_round(min(l["box"][0] for l in group)), _round(min(l["box"][1] for l in group)),
                    _round(max(l["box"][2] for l in group)), _round(max(l["box"][3] for l in group))],
            "vertical": vertical,
            "lines": [l["text"] for l in group],
            "lines_coords": [[[x1, y1], [x2, y1], [x2, y2], [x1, y2]] for x1, y1, x2, y2 in (l["box"] for l in group)],
        })
    return reading_order(blocks, rtl)


# ── Models ───────────────────────────────────────────────────────────

def _cache_dir() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or os.path.join(Path.home(), ".cache")
    return Path(base) / "matcha-reader" / ("ppocr-" + PPOCR_REVISION)


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _fetch(hub_path: str, sha: str, cache: Path) -> Path:
    dest = cache / Path(hub_path).name
    if dest.is_file() and _sha256(dest) == sha:
        return dest
    cache.mkdir(parents=True, exist_ok=True)
    print(f"Downloading text recognition model {Path(hub_path).name} ...", file=sys.stderr)
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(PPOCR_URL + hub_path, timeout=120) as resp, open(tmp, "wb") as f:
        while True:
            chunk = resp.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    got = _sha256(tmp)
    if got != sha:
        tmp.unlink()
        raise RuntimeError(f"{hub_path}: checksum mismatch (got {got})")
    tmp.replace(dest)
    return dest


class Engine:
    """The two ONNX sessions and the recogniser's vocabulary."""

    def __init__(self, cache: Path | None = None):
        import onnxruntime as ort

        cache = cache or _cache_dir()
        paths = {p: _fetch(p, sha, cache) for p, sha in PPOCR_FILES.items()}
        opts = ort.SessionOptions()
        opts.log_severity_level = 3
        self.det = ort.InferenceSession(str(paths["det/manga_det_fp16.onnx"]), opts,
                                        providers=["CPUExecutionProvider"])
        self.rec = ort.InferenceSession(str(paths["rec/manga_rec_fp16.onnx"]), opts,
                                        providers=["CPUExecutionProvider"])
        lines = paths["ppocrv6_dict.txt"].read_text(encoding="utf-8").split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        # Index 0 is the CTC blank; the dictionary follows, then a space.
        self.vocab = [""] + [l.rstrip("\r") for l in lines] + [" "]
        self.det_half = self.det.get_inputs()[0].type == "tensor(float16)"
        self.rec_half = self.rec.get_inputs()[0].type == "tensor(float16)"


def load_engine() -> Engine:
    """Load the models, downloading them on first use. Raises on a missing dependency or a failed
    download, so the caller can say why text recognition is unavailable."""
    return Engine()


# ── Inference ────────────────────────────────────────────────────────

def _on_white(img):
    from PIL import Image

    if img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info):
        rgba = img.convert("RGBA")
        out = Image.new("RGB", rgba.size, (255, 255, 255))
        out.paste(rgba, mask=rgba.getchannel("A"))
        return out
    return img.convert("RGB")


def _tensor(img, mean, std, half: bool):
    """An RGB image as a [1, 3, h, w] tensor in B, G, R plane order."""
    import numpy as np

    rgb = np.asarray(img, dtype=np.float32) / 255.0
    bgr = rgb[:, :, ::-1]
    out = ((bgr - np.array(mean, dtype=np.float32)) / np.array(std, dtype=np.float32)).transpose(2, 0, 1)[None]
    return np.ascontiguousarray(out, dtype=np.float16 if half else np.float32)


def read_lines(engine: Engine, image) -> list[dict]:
    """Lines on a PIL image, in its pixels."""
    import numpy as np
    from PIL import Image

    image = _on_white(image)
    w, h = image.size
    dw, dh = det_size(w, h)
    det_in = _tensor(image.resize((dw, dh), Image.BILINEAR), DET_MEAN, DET_STD, engine.det_half)
    pred = engine.det.run(None, {engine.det.get_inputs()[0].name: det_in})[0]
    pred = np.asarray(pred, dtype=np.float32).reshape(dh, dw)
    sx, sy = w / dw, h / dh
    rec_name = engine.rec.get_inputs()[0].name

    lines = []
    for bx1, by1, bx2, by2 in db_boxes(pred.ravel().tolist(), dw, dh):
        x1, y1 = max(0, _round(bx1 * sx)), max(0, _round(by1 * sy))
        x2, y2 = min(w, _round(bx2 * sx)), min(h, _round(by2 * sy))
        cw, ch = x2 - x1, y2 - y1
        if cw < 2 or ch < 2:
            continue
        vertical = ch > cw
        crop = image.crop((x1, y1, x2, y2))
        if vertical:
            # Turned 90 degrees anticlockwise so it reads left to right.
            crop = crop.transpose(Image.ROTATE_90)
        tw = rec_width(crop.width, crop.height)
        rec_in = _tensor(crop.resize((tw, REC_HEIGHT), Image.BICUBIC), (0.5, 0.5, 0.5), (0.5, 0.5, 0.5),
                         engine.rec_half)
        logits = engine.rec.run(None, {rec_name: rec_in})[0]
        best = np.asarray(logits[0], dtype=np.float32).argmax(axis=-1).tolist()
        text = ctc_decode(best, engine.vocab)
        if text:
            lines.append({"text": text, "box": [x1, y1, x2, y2], "vertical": vertical})
    return lines


def read_page(engine: Engine, image, rtl: bool = True) -> dict:
    """One page as a Mokuro page record, measured on the image given."""
    return {"img_width": image.width, "img_height": image.height,
            "blocks": bubbles(read_lines(engine, image), rtl)}
