#!/usr/bin/env python3
"""Convert manga (image folder / CBZ / EPUB) into CrossPoint Reader format.

Replaces the Mokuro-based pipeline (tools/mokuro_convert/). Mokuro infers
panel boundaries by clustering OCR text-box positions, which only
approximates real panels and produces nothing for panels without text. This
tool instead:

  1. Detects actual panel RECTANGLES geometrically (white-gutter grid
     detection -- no ML model required).
  2. Crops each panel and sends it to Gemini (gemini-2.5-flash) asking what
     text/dialogue appears in it, as JSON.
  3. Writes the same panels.idx/panels.dat binary format the device already
     reads, plus page images renamed to a canonical page_NNNN.<ext> sequence
     so the device's natural-sort-based page scan can never misorder pages
     (no dependency on a distributor's arbitrary source filenames).

Page images are copied as-is (JPG/PNG) -- the device renders them directly,
no BMP conversion needed.

Usage:
    export GEMINI_API_KEY=$(cat ~/path/to/gemini.key)
    python3 convert_manga.py --input ./manga_pages/ --output-dir /path/to/sd/manga/Book/

    # Or pass the key file directly (key is read at runtime, never embedded):
    python3 convert_manga.py --input ./manga_pages/ --output-dir ./out/ \\
        --gemini-key-file ./gemini.key

    # Explicit page order, for sources whose filenames don't sort correctly
    # (one source filename per line, in the order pages should be read):
    python3 convert_manga.py --input ./manga_pages/ --output-dir ./out/ \\
        --page-order-file ./order.txt

    # Skip the Gemini OCR pass entirely (panels only, no text/lookup data):
    python3 convert_manga.py --input ./manga_pages/ --output-dir ./out/ --no-ocr

Output (in --output-dir):
    page_0000.jpg, page_0001.jpg, ...   canonical, trivially-sortable page
                                         images (device scans these directly)
    p<page>_<panel>.jpg                 cropped panel images for panel-zoom
    panels.idx / panels.dat             panel layout data
    meta.bin                            book title + author (auto-extracted
                                         from source, or set via --title/--author)

Binary format (meta.bin):
    Header (8 bytes):
        uint32  version         (currently 1)
        uint16  titleLen        UTF-8 byte length of title
        uint16  authorLen       UTF-8 byte length of author
    char[]  title               UTF-8 title (titleLen bytes)
    char[]  author              UTF-8 author (authorLen bytes)

Binary format (panels.idx):
    Header:
        uint32  version         (currently 1)
        uint32  pageCount
    Per page (pageCount records, 12 bytes each):
        uint32  dataOffset      byte offset into panels.dat
        uint32  dataLength      byte length of this page's data
        uint16  imgWidth        source image width (pixels)
        uint16  imgHeight       source image height (pixels)

Binary format (panels.dat, per page at dataOffset):
    uint8   panelCount
    uint8   reserved
    Per panel (panelCount entries):
        uint16  x, y, w, h        panel bounding box (pixels)
        uint8   textCount         text blocks in this panel
        uint8   reserved
        uint16  translationLen    UTF-8 length of the panel's English translation
        bytes   translation[]     UTF-8 translation (translationLen bytes), empty if none
        Per text block (textCount entries):
            uint16  x, y, w, h    text block bounding box (pixels)
            uint16  textLen       UTF-8 text length
            bytes   text[]        UTF-8 text (textLen bytes, not null-terminated)
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

FORMAT_VERSION = 2  # v2 adds a per-panel translation string after the panel header

IDX_HEADER = "<II"  # version(4) + pageCount(4) = 8 bytes
IDX_RECORD = "<IIHH"  # dataOffset(4) + dataLength(4) + imgWidth(2) + imgHeight(2) = 12 bytes
PANEL_BOX = "<HHHHBBH"  # x(2)+y(2)+w(2)+h(2)+textCount(1)+pad(1)+translationLen(2) = 12 bytes
TEXT_BLOCK = "<HHHHH"  # x(2) + y(2) + w(2) + h(2) + textLen(2) = 10 bytes

TOC_FORMAT_VERSION = 1
TOC_HEADER = "<II"  # version(4) + entryCount(4) = 8 bytes
TOC_ENTRY_HEADER = "<IH"  # pageIndex(4) + titleLen(2) = 6 bytes

IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}

GEMINI_MODEL = "gemini-2.5-flash"
GEMINI_URL = f"https://generativelanguage.googleapis.com/v1beta/models/{GEMINI_MODEL}:generateContent"

PANEL_OCR_PROMPT = """This image is a single panel cropped from a Japanese manga page.
List every piece of text/dialogue visible in this panel, in the order a
reader would read them (top-to-bottom, right-to-left for manga). Then give
a single natural English translation of all of it combined, in the same
reading order, as it would read in an English localization of this manga.

Return ONLY a JSON object, no other text:
{"blocks": [{"text": "<the Japanese text, line breaks as \\n>",
             "bbox_2d": [ymin, xmin, ymax, xmax]}, ...],
 "translation": "<natural English translation of all the panel's text combined, in reading order>"}

bbox_2d is each text region's bounding box normalized to a 0-1000 scale
(0,0 = top-left of the panel image, 1000,1000 = bottom-right). If you
cannot determine a precise box, omit bbox_2d for that entry.
If there is no text in the panel, return {"blocks": [], "translation": ""}."""


# ── Page collection / ordering ───────────────────────────────────


def is_image(path: str) -> bool:
    return Path(path).suffix.lower() in IMAGE_EXTS


def _natural_sort_key(path: str):
    """Natural sort key matching FsHelpers::sortFileList() on the device.

    Case-insensitive; numeric substrings compared by integer value.

    Cover and copyright pages are pinned to the very front (positions 0
    and 1) regardless of their filename digits, since digital manga
    exports commonly use a distributor product-code prefix (a long
    unbroken digit blob, NOT a page-sequence number) that pure natural
    sort pushes to the very end. Standard book structure is always:
    cover first, copyright second, then all other content.
    """
    name = os.path.basename(path)
    lower = name.lower()
    if 'cover' in lower:
        return [(-2, 0, '')]
    if 'copyright' in lower:
        return [(-1, 0, '')]
    parts: list[tuple] = []
    i = 0
    while i < len(name):
        if name[i].isdigit():
            j = i
            while j < len(name) and name[j].isdigit():
                j += 1
            num_str = name[i:j].lstrip("0") or ""
            parts.append((0, len(num_str), num_str))
            i = j
        else:
            parts.append((1, 0, name[i].lower()))
            i += 1
    return parts


def collect_pages(input_path: str, work_dir: str, page_order_file: str | None) -> list[str]:
    """Return an ordered list of source page image paths."""
    p = Path(input_path)

    if p.is_dir():
        images = [str(f) for f in p.iterdir() if f.is_file() and is_image(str(f))]
    elif p.suffix.lower() in (".cbz", ".zip"):
        extract_dir = os.path.join(work_dir, "extracted")
        os.makedirs(extract_dir, exist_ok=True)
        with zipfile.ZipFile(str(p), "r") as zf:
            for info in zf.infolist():
                if info.is_dir() or not is_image(info.filename):
                    continue
                target = os.path.join(extract_dir, os.path.basename(info.filename))
                with zf.open(info) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst)
        images = [str(f) for f in Path(extract_dir).iterdir() if is_image(str(f))]
    elif p.suffix.lower() == ".epub":
        images = _extract_epub_pages(str(p), work_dir)
    elif p.suffix.lower() == ".pdf":
        images = _extract_pdf_pages(str(p), work_dir)
    else:
        print(f"Error: unsupported input: {p}", file=sys.stderr)
        sys.exit(1)

    if not images:
        print(f"Error: no image files found in {p}", file=sys.stderr)
        sys.exit(1)

    if page_order_file:
        with open(page_order_file, "r", encoding="utf-8") as f:
            order_names = [line.strip() for line in f if line.strip()]
        by_name = {os.path.basename(img): img for img in images}
        ordered = []
        for name in order_names:
            if name not in by_name:
                print(f"Warning: {name} from --page-order-file not found among extracted images", file=sys.stderr)
                continue
            ordered.append(by_name[name])
        missing = [img for img in images if os.path.basename(img) not in order_names]
        if missing:
            print(f"Warning: {len(missing)} images not listed in --page-order-file are dropped", file=sys.stderr)
        return ordered

    images.sort(key=_natural_sort_key)
    return images


def _extract_epub_pages(epub_path: str, work_dir: str) -> list[str]:
    """Extract page images from an EPUB in true spine reading order."""
    extract_dir = os.path.join(work_dir, "epub_extracted")
    os.makedirs(extract_dir, exist_ok=True)

    with zipfile.ZipFile(epub_path, "r") as zf:
        container = zf.read("META-INF/container.xml").decode("utf-8")
        m = re.search(r'full-path="([^"]+)"', container)
        if not m:
            print("Error: could not find OPF in EPUB container.xml", file=sys.stderr)
            sys.exit(1)
        opf_path = m.group(1)
        opf_dir = os.path.dirname(opf_path)
        opf = zf.read(opf_path).decode("utf-8")

        manifest = dict(re.findall(r'<item[^>]*id="([^"]+)"[^>]*href="([^"]+)"', opf))
        # href may appear before id -- also try the reverse attribute order
        manifest.update(dict((b, a) for a, b in re.findall(r'<item[^>]*href="([^"]+)"[^>]*id="([^"]+)"', opf)))
        spine_ids = re.findall(r'<itemref[^>]*idref="([^"]+)"', opf)

        images = []
        spine_map = []  # (extracted_basename, spine_item_href) -- for TOC resolution
        for idx, item_id in enumerate(spine_ids):
            href = manifest.get(item_id)
            if not href:
                continue
            full_href = os.path.normpath(os.path.join(opf_dir, href)) if opf_dir else href
            full_href = full_href.replace(os.sep, "/")
            if is_image(full_href):
                src_in_zip = full_href
            else:
                # Spine item is an XHTML wrapper page -- find the embedded image.
                try:
                    xhtml = zf.read(full_href).decode("utf-8", "ignore")
                except KeyError:
                    continue
                img_m = re.search(r'(?:src|xlink:href)="([^"]+)"', xhtml)
                if not img_m:
                    continue
                img_href = img_m.group(1)
                xhtml_dir = os.path.dirname(full_href)
                src_in_zip = os.path.normpath(os.path.join(xhtml_dir, img_href)).replace(os.sep, "/")

            try:
                data = zf.read(src_in_zip)
            except KeyError:
                print(f"Warning: image not found in EPUB: {src_in_zip}", file=sys.stderr)
                continue
            target_basename = f"spine_{idx:04d}_{os.path.basename(src_in_zip)}"
            # TOC entries reference the SPINE item's own href (the XHTML
            # wrapper, when there is one) -- record that, not the resolved
            # embedded-image href, so _extract_epub_native_toc can match.
            spine_map.append((target_basename, full_href))
            target = os.path.join(extract_dir, target_basename)
            with open(target, "wb") as f:
                f.write(data)
            images.append(target)

        # Sidecar map for _extract_epub_native_toc to resolve TOC hrefs
        # against extracted filenames without changing this function's
        # return type (still a plain list of image paths).
        with open(os.path.join(extract_dir, "_spine_map.tsv"), "w", encoding="utf-8") as f:
            for basename, spine_href in spine_map:
                f.write(f"{basename}\t{spine_href}\n")

    return images


def _extract_pdf_pages(pdf_path: str, work_dir: str) -> list[str]:
    """Rasterize each PDF page to a PNG, in document order (page 1 first)."""
    try:
        import fitz  # PyMuPDF
    except ImportError:
        print(
            "Error: PDF input requires PyMuPDF. Run: pip install pymupdf",
            file=sys.stderr,
        )
        sys.exit(1)

    extract_dir = os.path.join(work_dir, "pdf_extracted")
    os.makedirs(extract_dir, exist_ok=True)

    images = []
    doc = fitz.open(pdf_path)
    try:
        # 2x zoom for reasonable resolution -- most manga PDFs embed pages
        # around 72-150 DPI; this brings them closer to typical e-ink
        # screen resolution without an excessive file size.
        matrix = fitz.Matrix(2, 2)
        for i, page in enumerate(doc):
            pix = page.get_pixmap(matrix=matrix)
            target = os.path.join(extract_dir, f"pdfpage_{i:04d}.png")
            pix.save(target)
            images.append(target)
    finally:
        doc.close()

    return images


# ── Panel detection ────────────────────────────────────────────
#
# Primary: a YOLO26-nano model fine-tuned on Manga109-s for panel/text
# detection (leoxs22/manga-panel-detector-yolo26n, mAP50 0.985 for panels).
# Falls back to a pure-Pillow white-gutter grid heuristic if `ultralytics`
# isn't installed, so this tool still runs with zero extra dependencies
# when ML deps aren't available -- just with lower detection quality.

_YOLO_MODEL = None
_YOLO_REPO = "leoxs22/manga-panel-detector-yolo26n"
_YOLO_FILENAME = "manga_panel_detector_fp32.pt"


def _load_yolo_model():
    global _YOLO_MODEL
    if _YOLO_MODEL is not None:
        return _YOLO_MODEL
    try:
        from huggingface_hub import hf_hub_download
        from ultralytics import YOLO
    except ImportError:
        return None
    try:
        weights_path = hf_hub_download(repo_id=_YOLO_REPO, filename=_YOLO_FILENAME)
        _YOLO_MODEL = YOLO(weights_path)
    except Exception as e:
        print(f"Warning: could not load YOLO panel detector ({e}); falling back to grid heuristic", file=sys.stderr)
        _YOLO_MODEL = False
    return _YOLO_MODEL if _YOLO_MODEL else None


def _box_area(b: list[int]) -> int:
    return max(0, b[2] - b[0]) * max(0, b[3] - b[1])


def _overlap_area(a: list[int], b: list[int]) -> int:
    ox1, oy1 = max(a[0], b[0]), max(a[1], b[1])
    ox2, oy2 = min(a[2], b[2]), min(a[3], b[3])
    return max(0, ox2 - ox1) * max(0, oy2 - oy1)


def _union_box(a: list[int], b: list[int]) -> list[int]:
    return [min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3])]


def _dedupe_boxes(boxes_with_conf: list[tuple], overlap_thresh: float = 0.6) -> list[list[int]]:
    """Collapse boxes that substantially overlap into their union.

    The detector sometimes emits two overlapping boxes for the same panel
    region at different confidences/scales. Simply dropping the
    lower-confidence one can lose real page coverage when that box was
    actually larger and extended into area the kept box didn't cover (e.g.
    a small high-confidence box fully inside a larger, lower-confidence
    one that also reaches further) -- merging into the bounding union
    keeps all detected panel area while still collapsing the duplicate.

    Overlap is measured relative to the SMALLER of the two boxes so
    containment is caught regardless of which box gets processed first.
    """
    ordered = sorted(boxes_with_conf, key=lambda bc: -bc[1])
    kept: list[list[int]] = []
    for box, _conf in ordered:
        area = _box_area(box)
        if area == 0:
            continue
        merged_into = None
        for i, k in enumerate(kept):
            k_area = _box_area(k)
            if k_area > 0 and _overlap_area(box, k) / min(area, k_area) > overlap_thresh:
                merged_into = i
                break
        if merged_into is not None:
            kept[merged_into] = _union_box(kept[merged_into], box)
        else:
            kept.append(box)
    return kept


def is_full_page_panel(box: list[int], page_w: int, page_h: int, threshold: float = 0.95) -> bool:
    """True when the panel covers almost the entire page in both dimensions,
    making a pre-cropped panel image essentially identical to the full page.
    No panel-crop file is generated for these -- the renderer falls back to
    full-page display, which is already identical, avoiding a redundant file."""
    w = max(1, box[2] - box[0])
    h = max(1, box[3] - box[1])
    return (w / max(1, page_w)) >= threshold and (h / max(1, page_h)) >= threshold




def is_sliver_panel(box: list[int], page_w: int, page_h: int) -> bool:
    """True for degenerate detections: a thin strip cropped from a panel
    edge/corner rather than a real panel. These are small relative to the
    page AND extremely elongated -- a legitimate small reaction-shot panel
    is usually close to square, while a false detection from a border line
    or torn-off panel edge is a long thin sliver."""
    w = max(1, box[2] - box[0])
    h = max(1, box[3] - box[1])
    area_frac = (w * h) / max(1, page_w * page_h)
    aspect = max(w / h, h / w)
    return area_frac < 0.025 and aspect > 4.0


def _detect_panels_yolo(img, conf: float = 0.4) -> list[list[int]] | None:
    """Detect panels with the YOLO26-nano Manga109 model. Returns None if
    the model isn't available (caller should fall back to the grid
    heuristic)."""
    model = _load_yolo_model()
    if model is None:
        return None

    results = model.predict(img, conf=conf, iou=0.5, verbose=False)
    boxes_with_conf = []
    for box in results[0].boxes:
        if int(box.cls) != 0:  # 0=panel, 1=text -- we only want panels here
            continue
        x1, y1, x2, y2 = box.xyxy[0].tolist()
        xy_box = [int(x1), int(y1), int(x2), int(y2)]
        if is_sliver_panel(xy_box, img.width, img.height):
            continue
        boxes_with_conf.append((xy_box, float(box.conf)))

    boxes = _dedupe_boxes(boxes_with_conf)
    if not boxes:
        return [[0, 0, img.width, img.height]]
    return boxes


def _snap_to_unclaimed_edges(boxes: list[list[int]], page_w: int, page_h: int,
                             max_gap_frac: float = 0.15) -> list[list[int]]:
    """Extend a panel's edge to the page boundary when it falls short by a
    plausible amount AND no other detected panel claims that space.

    The detector sometimes underestimates a panel's true extent near the
    page edge (e.g. missing a speech bubble that reaches close to the
    border), leaving a gap that should belong to that panel rather than
    being a deliberate gutter. Only snap small gaps (<15% of the page
    dimension) and only when nothing else occupies the overlapping range,
    so real gutters between adjacent panels are left alone.
    """
    original = [tuple(b) for b in boxes]
    result = [list(b) for b in boxes]

    def claimed_beyond(i: int, axis: str, beyond) -> bool:
        ox1, oy1, ox2, oy2 = original[i]
        for j, (jx1, jy1, jx2, jy2) in enumerate(original):
            if j == i:
                continue
            if axis == "x":
                overlaps = not (jy2 <= oy1 or jy1 >= oy2)
                if overlaps and beyond(jx1, jx2, ox1, ox2):
                    return True
            else:
                overlaps = not (jx2 <= ox1 or jx1 >= ox2)
                if overlaps and beyond(jy1, jy2, oy1, oy2):
                    return True
        return False

    for i, (x1, y1, x2, y2) in enumerate(original):
        if 0 < page_w - x2 < page_w * max_gap_frac and \
           not claimed_beyond(i, "x", lambda j1, j2, o1, o2: j2 > o2):
            result[i][2] = page_w
        if 0 < x1 < page_w * max_gap_frac and \
           not claimed_beyond(i, "x", lambda j1, j2, o1, o2: j1 < o1):
            result[i][0] = 0
        if 0 < page_h - y2 < page_h * max_gap_frac and \
           not claimed_beyond(i, "y", lambda j1, j2, o1, o2: j2 > o2):
            result[i][3] = page_h
        if 0 < y1 < page_h * max_gap_frac and \
           not claimed_beyond(i, "y", lambda j1, j2, o1, o2: j1 < o1):
            result[i][1] = 0

    return result


def _merge_small_gaps(splits: list[int], min_size: int) -> list[int]:
    """Collapse boundary points that would create a too-small segment.

    Walks left to right keeping a boundary only if it's far enough from the
    last kept one; a rejected boundary isn't a content drop -- the segment
    it would have started simply gets absorbed into its neighbor. The final
    page-edge boundary is always preserved.
    """
    if len(splits) <= 2:
        return splits
    merged = [splits[0]]
    for s in splits[1:]:
        if s - merged[-1] < min_size:
            continue
        merged.append(s)
    if merged[-1] != splits[-1]:
        merged[-1] = splits[-1]
    return merged


def _detect_panels_grid(img) -> list[list[int]]:
    """Detect panel rectangles by finding solid white gutter bands.

    Pure-Pillow grid detection: scans for rows/columns that are almost
    perfectly white (background gutters between panels), splitting the page
    into bands and then columns within each band. Requires near-total
    whiteness and a substantial minimum gutter/segment size so that
    incidental white space around a speech bubble -- which sits *inside* a
    panel, not between panels -- doesn't get mistaken for a panel boundary;
    candidate splits too close together are merged into their neighbor
    rather than dropped, so no page content is silently lost. Degrades
    gracefully (whole page as one panel) for free-form/borderless layouts.
    """
    gray = img.convert("L")
    w, h = gray.size
    pixels = gray.load()

    threshold = 215
    purity = 0.95
    min_gutter = max(6, int(h * 0.013))
    min_band_h = max(int(h * 0.05), 60)
    min_band_w = max(int(w * 0.06), 60)

    def is_white_row(y: int) -> bool:
        white = sum(1 for x in range(0, w, 2) if pixels[x, y] > threshold)
        return white > (w // 2) * purity

    h_splits = [0]
    in_gutter = False
    gutter_start = 0
    for y in range(h):
        white_row = is_white_row(y)
        if white_row and not in_gutter:
            in_gutter = True
            gutter_start = y
        elif not white_row and in_gutter:
            if y - gutter_start >= min_gutter:
                h_splits.append((gutter_start + y) // 2)
            in_gutter = False
    h_splits.append(h)
    h_splits = _merge_small_gaps(h_splits, min_band_h)

    panels = []
    for band_idx in range(len(h_splits) - 1):
        y1, y2 = h_splits[band_idx], h_splits[band_idx + 1]

        def is_white_col(x: int) -> bool:
            white = sum(1 for y in range(y1, y2, 2) if pixels[x, y] > threshold)
            return white > ((y2 - y1) // 2) * purity

        v_splits = [0]
        in_gutter = False
        gutter_start = 0
        for x in range(w):
            white_col = is_white_col(x)
            if white_col and not in_gutter:
                in_gutter = True
                gutter_start = x
            elif not white_col and in_gutter:
                if x - gutter_start >= min_gutter:
                    v_splits.append((gutter_start + x) // 2)
                in_gutter = False
        v_splits.append(w)
        v_splits = _merge_small_gaps(v_splits, min_band_w)

        for col_idx in range(len(v_splits) - 1):
            x1, x2 = v_splits[col_idx], v_splits[col_idx + 1]
            panels.append([x1, y1, x2, y2])

    if not panels:
        panels.append([0, 0, w, h])

    return panels


def detect_panels(img) -> list[list[int]]:
    """Detect panel rectangles -- YOLO model if available, else grid heuristic."""
    boxes = _detect_panels_yolo(img)
    if boxes is not None:
        return boxes
    return _detect_panels_grid(img)


def _y_overlap_frac(a: list[int], b: list[int]) -> float:
    """Fraction of the shorter panel's height that the two panels' vertical
    extents overlap. Used to decide whether two panels are in the same
    reading "tier" -- center-distance clustering breaks down when one tall
    panel spans the same vertical range as two shorter stacked panels (a
    very common manga layout); overlap is the geometrically correct test."""
    overlap = min(a[3], b[3]) - max(a[1], b[1])
    min_h = min(a[3] - a[1], b[3] - b[1])
    return max(0.0, overlap) / max(1, min_h)


def sort_panels_manga_order(panels: list[list[int]]) -> list[list[int]]:
    """Sort panel boxes in manga reading order via a "reads-before" graph,
    then a topological sort -- robust to mixed-size grids (e.g. one tall
    panel beside two stacked shorter ones), which simple row-clustering by
    Y-center gets wrong.

    For every pair of panels: if their vertical extents overlap
    substantially, they're in the same tier and read right-to-left; if not,
    whichever is higher up reads first (the other dimension doesn't matter
    once there's no vertical overlap). This produces a partial order;
    topological sort resolves the full reading sequence, with same-rank
    ties broken top-to-bottom then right-to-left.
    """
    n = len(panels)
    if n <= 1:
        return panels

    OVERLAP_THRESHOLD = 0.3
    edges: list[list[int]] = [[] for _ in range(n)]
    in_degree = [0] * n

    for i in range(n):
        for j in range(n):
            if i == j:
                continue
            a, b = panels[i], panels[j]
            if _y_overlap_frac(a, b) > OVERLAP_THRESHOLD:
                a_cx, b_cx = (a[0] + a[2]) / 2, (b[0] + b[2]) / 2
                if a_cx > b_cx:  # same tier: right-to-left
                    edges[i].append(j)
                    in_degree[j] += 1
            else:
                a_cy, b_cy = (a[1] + a[3]) / 2, (b[1] + b[3]) / 2
                if a_cy < b_cy:  # different tiers: top-to-bottom
                    edges[i].append(j)
                    in_degree[j] += 1

    def tie_break_key(i: int):
        x1, y1, x2, y2 = panels[i]
        return ((y1 + y2) / 2, -(x1 + x2) / 2)

    available = [i for i in range(n) if in_degree[i] == 0]
    result: list[int] = []
    while available:
        available.sort(key=tie_break_key)
        node = available.pop(0)
        result.append(node)
        for j in edges[node]:
            in_degree[j] -= 1
            if in_degree[j] == 0:
                available.append(j)

    if len(result) != n:
        # Inconsistent/cyclic constraints (shouldn't happen with these two
        # simple rules, but don't silently drop panels if it does).
        return panels

    return [panels[i] for i in result]


# ── Gemini OCR (invoked via curl, key never embedded in code) ───


def call_gemini_panel_ocr(image_path: str, api_key: str, timeout: int = 60, retries: int = 3) -> dict:
    """Ask Gemini what text appears in a panel image, plus an English
    translation of it. Returns {"blocks": [{"text", "bbox_2d"}, ...],
    "translation": str}. Retries on transient errors (503/429/network) with
    exponential backoff; returns {"blocks": [], "translation": ""} if all
    attempts fail, so callers fall back to a panel with no text/translation
    rather than aborting the whole run.
    """
    for attempt in range(retries):
        result = _call_gemini_panel_ocr_once(image_path, api_key, timeout)
        if result is not None:
            return result
        if attempt < retries - 1:
            time.sleep(2 ** attempt)
    return {"blocks": [], "translation": ""}


def _call_gemini_panel_ocr_once(image_path: str, api_key: str, timeout: int) -> dict | None:
    """Single attempt. Returns None (not the empty dict) on a transient
    failure so the retry loop above can distinguish "retry" from "this
    panel genuinely has no text" (the latter is a successful empty result)."""
    with open(image_path, "rb") as f:
        image_b64 = base64.b64encode(f.read()).decode("ascii")

    mime = "image/png" if image_path.lower().endswith(".png") else "image/jpeg"

    payload = {
        "contents": [{
            "parts": [
                {"text": PANEL_OCR_PROMPT},
                {"inline_data": {"mime_type": mime, "data": image_b64}},
            ]
        }],
        "generationConfig": {"responseMimeType": "application/json"},
    }

    with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8") as tf:
        json.dump(payload, tf)
        payload_path = tf.name

    try:
        result = subprocess.run(
            [
                "curl", "-s", "-X", "POST", GEMINI_URL,
                "-H", "Content-Type: application/json",
                "-H", f"x-goog-api-key: {api_key}",
                "-d", f"@{payload_path}",
            ],
            capture_output=True, text=True, timeout=timeout,
        )
    except (subprocess.TimeoutExpired, subprocess.SubprocessError, OSError):
        return None  # network-level failure -- retry
    finally:
        os.unlink(payload_path)

    if result.returncode != 0:
        return None  # network-level failure -- retry

    try:
        response = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None  # malformed response -- retry

    if "error" in response:
        status = response["error"].get("status", "")
        if status in ("UNAVAILABLE", "RESOURCE_EXHAUSTED", "DEADLINE_EXCEEDED", "INTERNAL"):
            return None  # transient API error -- retry
        print(f"  Warning: Gemini error: {response['error'].get('message', status)[:200]}", file=sys.stderr)
        return {"blocks": [], "translation": ""}  # non-transient -- give up on this panel

    try:
        text_out = response["candidates"][0]["content"]["parts"][0]["text"]
        parsed = json.loads(text_out)
        if not isinstance(parsed, dict):
            return {"blocks": [], "translation": ""}
        blocks = parsed.get("blocks", [])
        if not isinstance(blocks, list):
            blocks = []
        blocks = [b for b in blocks if isinstance(b, dict) and "text" in b]
        translation = parsed.get("translation", "")
        if not isinstance(translation, str):
            translation = ""
        return {"blocks": blocks, "translation": translation}
    except (KeyError, IndexError, json.JSONDecodeError) as e:
        snippet = result.stdout[:200]
        print(f"  Warning: could not parse Gemini response ({e}): {snippet}", file=sys.stderr)
        return {"blocks": [], "translation": ""}


# ── Binary output (same format the device already reads) ────────


def encode_page(panels_with_text: list[dict]) -> bytes:
    """Encode one page's panel+text data to binary."""
    buf = bytearray()
    panel_count = min(len(panels_with_text), 255)
    buf += struct.pack("BB", panel_count, 0)

    for panel in panels_with_text[:panel_count]:
        x1, y1, x2, y2 = panel["box"]
        w, h = x2 - x1, y2 - y1
        text_blocks = panel.get("text_blocks", [])
        text_count = min(len(text_blocks), 255)

        translation_bytes = panel.get("translation", "").encode("utf-8")
        if len(translation_bytes) > 0xFFFF:
            translation_bytes = translation_bytes[:0xFFFF]

        buf += struct.pack(
            PANEL_BOX, max(0, x1), max(0, y1), max(0, w), max(0, h), text_count, 0, len(translation_bytes)
        )
        buf += translation_bytes

        for tb in text_blocks[:text_count]:
            tx, ty, tw, th = tb["box"]
            text_bytes = tb["text"].encode("utf-8")
            if len(text_bytes) > 0xFFFF:
                text_bytes = text_bytes[:0xFFFF]
            buf += struct.pack(TEXT_BLOCK, max(0, tx), max(0, ty), max(0, tw), max(0, th), len(text_bytes))
            buf += text_bytes

    return bytes(buf)


def _write_panel_index(output_dir: str, idx_records: list[tuple], dat_chunks: list[bytes]):
    """Write panels.idx + panels.dat covering exactly the pages processed so
    far. Called after every page during conversion (not just once at the
    end) so a crash partway through never loses already-completed pages."""
    idx_path = os.path.join(output_dir, "panels.idx")
    dat_path = os.path.join(output_dir, "panels.dat")
    with open(idx_path, "wb") as f:
        f.write(struct.pack(IDX_HEADER, FORMAT_VERSION, len(idx_records)))
        for off, length, w, h in idx_records:
            f.write(struct.pack(IDX_RECORD, off, length, w, h))
    with open(dat_path, "wb") as f:
        for chunk in dat_chunks:
            f.write(chunk)


def write_toc(output_dir: str, entries: list[tuple], add_cover: bool = True):
    """Write toc.idx: a simple (pageIndex, title) chapter list.

    entries: list of (page_index: int, title: str), in any order -- sorted
    by page_index before writing. Device-side: lib/MangaPanel/MangaPanel.cpp
    loadToc(). Optional -- a manga folder without toc.idx just falls back
    to percent-based navigation (see MangaReaderActivity::SELECT_CHAPTER).

    Binary format (toc.idx):
        uint32  version       (currently 1)
        uint32  entryCount
        Per entry (entryCount records):
            uint32  pageIndex
            uint16  titleLen
            bytes   title[titleLen]   UTF-8, not null-terminated
    """
    entries = sorted(entries, key=lambda e: e[0])
    # Always prepend a Cover entry at page 0 unless one already exists there.
    if add_cover and (not entries or entries[0][0] != 0):
        entries = [(0, "Cover")] + entries
    toc_path = os.path.join(output_dir, "toc.idx")
    with open(toc_path, "wb") as f:
        f.write(struct.pack(TOC_HEADER, TOC_FORMAT_VERSION, len(entries)))
        for page_index, title in entries:
            title_bytes = title.encode("utf-8")
            if len(title_bytes) > 0xFFFF:
                title_bytes = title_bytes[:0xFFFF]
            f.write(struct.pack(TOC_ENTRY_HEADER, page_index, len(title_bytes)))
            f.write(title_bytes)
    print(f"  {toc_path}: {len(entries)} chapter(s)")


META_FORMAT_VERSION = 1
META_HEADER = "<IHH"  # version(4) + titleLen(2) + authorLen(2) = 8 bytes


def write_meta(output_dir: str, title: str, author: str) -> None:
    """Write meta.bin: book title and author for the CrossPoint library."""
    if not title and not author:
        return
    title_bytes = title.encode("utf-8")[:0xFFFF]
    author_bytes = author.encode("utf-8")[:0xFFFF]
    meta_path = os.path.join(output_dir, "meta.bin")
    with open(meta_path, "wb") as f:
        f.write(struct.pack(META_HEADER, META_FORMAT_VERSION, len(title_bytes), len(author_bytes)))
        f.write(title_bytes)
        f.write(author_bytes)
    print(f"  {meta_path}: title={title!r}, author={author!r}")


def extract_metadata(input_path: str, work_dir: str) -> tuple[str, str]:
    """Best-effort extraction of (title, author) from EPUB OPF, CBZ ComicInfo.xml, or PDF metadata."""
    p = Path(input_path)
    title, author = "", ""

    if p.suffix.lower() == ".epub":
        try:
            with zipfile.ZipFile(str(p), "r") as zf:
                container = zf.read("META-INF/container.xml").decode("utf-8")
                m = re.search(r'full-path="([^"]+)"', container)
                if m:
                    opf = zf.read(m.group(1)).decode("utf-8", "ignore")
                    t = re.search(r'<dc:title[^>]*>([^<]+)</dc:title>', opf)
                    if t:
                        title = t.group(1).strip()
                    a = re.search(r'<dc:creator[^>]*>([^<]+)</dc:creator>', opf)
                    if a:
                        author = a.group(1).strip()
        except Exception:
            pass

    elif p.suffix.lower() in (".cbz", ".zip"):
        try:
            with zipfile.ZipFile(str(p), "r") as zf:
                names = [n for n in zf.namelist() if os.path.basename(n).lower() == "comicinfo.xml"]
                if names:
                    xml = zf.read(names[0]).decode("utf-8", "ignore")
                    t = re.search(r'<Title>([^<]+)</Title>', xml)
                    if t:
                        title = t.group(1).strip()
                    a = re.search(r'<Writer>([^<]+)</Writer>', xml)
                    if a:
                        author = a.group(1).strip()
        except Exception:
            pass

    elif p.suffix.lower() == ".pdf":
        try:
            import fitz  # PyMuPDF
            doc = fitz.open(str(p))
            meta = doc.metadata
            title = (meta.get("title") or "").strip()
            author = (meta.get("author") or "").strip()
        except Exception:
            pass

    return title, author


def parse_toc_file(toc_file: str) -> list[tuple]:
    """Parse a --toc-file: one chapter per line, "<page_index><TAB><title>".
    page_index is 0-based, referring to the final digital page order (the
    page_NNNN.<ext> numbering this tool produces) -- not a source filename
    or a printed page number, since those vary by source format and don't
    necessarily match the final page sequence (e.g. front matter, dropped
    duplicate/blank pages).
    """
    entries = []
    with open(toc_file, "r", encoding="utf-8") as f:
        for line_no, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip():
                continue
            parts = line.split("\t", 1)
            if len(parts) != 2:
                print(f"Warning: --toc-file line {line_no} not in '<page_index>\\t<title>' format, skipping",
                     file=sys.stderr)
                continue
            try:
                page_index = int(parts[0].strip())
            except ValueError:
                print(f"Warning: --toc-file line {line_no} has a non-integer page index, skipping", file=sys.stderr)
                continue
            entries.append((page_index, parts[1].strip()))
    return entries


def _extract_epub_native_toc(epub_path: str, pages: list[str], work_dir: str) -> list[tuple]:
    """Best-effort: read an EPUB's native table of contents (EPUB3 nav.xhtml
    or EPUB2 toc.ncx) and map each entry to a final digital page index.

    TOC entries reference each spine item's OWN href (the XHTML wrapper
    page, when there is one) -- not the embedded image inside it. Resolve
    against the "_spine_map.tsv" sidecar _extract_epub_pages writes
    (extracted_basename -> original spine href), not the image's own
    basename, which would never match.

    Returns [] if the EPUB has no nav/ncx, or the sidecar map is missing
    (e.g. input wasn't an EPUB), or nothing could be resolved -- callers
    should fall back to --toc-file in that case.
    """
    spine_map_path = os.path.join(work_dir, "epub_extracted", "_spine_map.tsv")
    if not os.path.exists(spine_map_path):
        return []
    href_to_basename = {}
    with open(spine_map_path, "r", encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t", 1)
            if len(parts) == 2:
                basename, spine_href = parts
                href_to_basename[spine_href] = basename
    try:
        with zipfile.ZipFile(epub_path, "r") as zf:
            container = zf.read("META-INF/container.xml").decode("utf-8")
            m = re.search(r'full-path="([^"]+)"', container)
            if not m:
                return []
            opf_path = m.group(1)
            opf_dir = os.path.dirname(opf_path)
            opf = zf.read(opf_path).decode("utf-8")

            # EPUB3: <item properties="nav" href="...">
            nav_href = None
            nav_m = re.search(r'<item[^>]*properties="[^"]*\bnav\b[^"]*"[^>]*href="([^"]+)"', opf)
            if nav_m:
                nav_href = nav_m.group(1)
            else:
                rev_nav_m = re.search(r'<item[^>]*href="([^"]+)"[^>]*properties="[^"]*\bnav\b[^"]*"', opf)
                if rev_nav_m:
                    nav_href = rev_nav_m.group(1)

            toc_entries_raw = []  # list of (href_with_optional_anchor, title)

            if nav_href:
                nav_path = os.path.normpath(os.path.join(opf_dir, nav_href)).replace(os.sep, "/")
                nav_xhtml = zf.read(nav_path).decode("utf-8", "ignore")
                nav_dir = os.path.dirname(nav_path)
                toc_m = re.search(r'<nav[^>]*epub:type="toc"[^>]*>(.*?)</nav>', nav_xhtml, re.DOTALL)
                if toc_m:
                    for a_m in re.finditer(r'<a[^>]*href="([^"]+)"[^>]*>(.*?)</a>', toc_m.group(1), re.DOTALL):
                        href = os.path.normpath(os.path.join(nav_dir, a_m.group(1))).replace(os.sep, "/")
                        title = re.sub(r'<[^>]+>', '', a_m.group(2)).strip()
                        if title:
                            toc_entries_raw.append((href, title))
            else:
                # EPUB2: <spine toc="ncx-id"> + <item id="ncx-id" href="...">
                ncx_m = re.search(r'<spine[^>]*toc="([^"]+)"', opf)
                if ncx_m:
                    ncx_id = ncx_m.group(1)
                    href_m = re.search(rf'<item[^>]*id="{re.escape(ncx_id)}"[^>]*href="([^"]+)"', opf)
                    if not href_m:
                        href_m = re.search(rf'<item[^>]*href="([^"]+)"[^>]*id="{re.escape(ncx_id)}"', opf)
                    if href_m:
                        ncx_path = os.path.normpath(os.path.join(opf_dir, href_m.group(1))).replace(os.sep, "/")
                        ncx = zf.read(ncx_path).decode("utf-8", "ignore")
                        ncx_dir = os.path.dirname(ncx_path)
                        for np_m in re.finditer(r'<navPoint\b.*?</navPoint>', ncx, re.DOTALL):
                            block = np_m.group(0)
                            text_m = re.search(r'<text>(.*?)</text>', block, re.DOTALL)
                            src_m = re.search(r'<content[^>]*src="([^"]+)"', block)
                            if text_m and src_m:
                                href = os.path.normpath(os.path.join(ncx_dir, src_m.group(1))).replace(os.sep, "/")
                                title = text_m.group(1).strip()
                                if title:
                                    toc_entries_raw.append((href, title))

            if not toc_entries_raw:
                return []

            basename_to_index = {os.path.basename(p): idx for idx, p in enumerate(pages)}

            resolved = []
            for href, title in toc_entries_raw:
                # TOC entries may include a #fragment (anchor within the
                # page) -- we can only point at a whole page, so drop it.
                href_no_anchor = href.split("#", 1)[0]
                extracted_basename = href_to_basename.get(href_no_anchor)
                if extracted_basename and extracted_basename in basename_to_index:
                    resolved.append((basename_to_index[extracted_basename], title))

            return resolved
    except (KeyError, zipfile.BadZipFile, OSError):
        return []


# ── Main pipeline ─────────────────────────────────────────────────


# Device screen sizes (portrait width x height) for --x3 / --x4 downscaling.
DEVICE_TARGETS = {"x3": (528, 792), "x4": (480, 800)}


def fit_to_device(img, target):
    """Downscale img to fit the device screen box; never upscale, never change aspect.

    The firmware rotates a page/panel whose aspect doesn't match the screen so it fills the
    display (a landscape image on the portrait screen renders rotated), so landscape images are
    fitted against the swapped box. The result keeps every pixel the device can actually show and
    drops the ones it never could -- the ESP32-C3 then decodes far fewer pixels per page/panel.

    Returns img unchanged when target is None (default: keep original resolution) or the image
    already fits.
    """
    if target is None:
        return img
    from PIL import Image  # deferred like main()'s import, so --help works without Pillow

    tw, th = target
    w, h = img.size
    if w > h:
        tw, th = th, tw
    scale = min(tw / w, th / h)
    if scale >= 1.0:
        return img
    # Palette/1-bit/alpha modes resize poorly (palette indices get interpolated); normalize the
    # same way the JPEG save path does. Grayscale sources stay grayscale.
    if img.mode not in ("RGB", "L"):
        img = img.convert("RGB")
    return img.resize((max(1, round(w * scale)), max(1, round(h * scale))), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser(
        description="Convert manga (image folder / CBZ / EPUB) into CrossPoint Reader format.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--input", required=True, help="Image folder, .cbz/.zip, or .epub")
    parser.add_argument("--output-dir", required=True, help="Directory to write pages, panels, and panels.idx/dat")
    parser.add_argument("--page-order-file", help="Text file listing source filenames in reading order, one per line")
    parser.add_argument("--gemini-key-file", help="Path to a file containing the Gemini API key")
    parser.add_argument("--no-ocr", action="store_true", help="Skip Gemini OCR -- panel boxes only, no text")
    parser.add_argument("--panel-margin", type=int, default=10, help="Pixels of margin added around cropped panels")
    parser.add_argument("--max-pages", type=int, help="Only process the first N pages (for testing)")
    parser.add_argument(
        "--toc-file",
        help='Chapter list: one per line, "<page_index>\\t<title>" (0-based, referring to the FINAL '
             "page order this tool produces -- not a source filename or printed page number). "
             "For EPUB input, the EPUB's own table of contents is used automatically when present; "
             "--toc-file overrides/supplements that.",
    )
    parser.add_argument("--title", help="Book title written to meta.bin (overrides value auto-detected from source)")
    parser.add_argument("--author", help="Book author written to meta.bin (overrides value auto-detected from source)")
    parser.add_argument(
        "--mono",
        action="store_true",
        help="Write pages and panel crops as 1-bit (black/white) Floyd-Steinberg-dithered BMP instead of JPEG. "
             "The device renders 1-bit BMP with a single fast black-and-white refresh (no 4-level gray pass), so "
             "pages and panels paint noticeably faster. Best for pure line-art manga; screentone gradients become "
             "dither patterns. Pairs naturally with --no-ocr: when OCR is enabled the (dithered) panel crop is what "
             "gets sent to Gemini, so text recognition on toned pages is less accurate than from a JPEG crop.",
    )
    size_group = parser.add_mutually_exclusive_group()
    size_group.add_argument(
        "--x3",
        action="store_true",
        help="Downscale pages and panel crops to fit the Xteink X3 screen (528x792; landscape images fit the "
             "rotated box). The device never displays more pixels than its screen, so this shrinks files and "
             "makes on-device decoding much faster with no visible quality loss. Never upscales. Applies to "
             "every output format (JPEG/PNG/BMP). Default without --x3/--x4: keep original resolution.",
    )
    size_group.add_argument(
        "--x4",
        action="store_true",
        help="Downscale pages and panel crops to fit the Xteink X4 screen (480x800). See --x3.",
    )
    args = parser.parse_args()

    device_target = DEVICE_TARGETS["x3"] if args.x3 else DEVICE_TARGETS["x4"] if args.x4 else None

    api_key = None
    if not args.no_ocr:
        if args.gemini_key_file:
            with open(args.gemini_key_file, "r", encoding="utf-8") as f:
                api_key = f.read().strip()
        else:
            api_key = os.environ.get("GEMINI_API_KEY")
        if not api_key:
            print(
                "Error: no Gemini API key. Pass --gemini-key-file or set GEMINI_API_KEY, "
                "or pass --no-ocr to skip text extraction.",
                file=sys.stderr,
            )
            sys.exit(1)

    from PIL import Image

    work_dir = tempfile.mkdtemp(prefix="manga_convert_")
    try:
        print(f"Collecting pages from: {args.input}")
        pages = collect_pages(args.input, work_dir, args.page_order_file)

        # Resolve a table of contents before any --max-pages truncation, so
        # chapter page indices refer to the full book even when testing
        # with a truncated run (entries past the truncated range just won't
        # be reachable, but the index numbering stays correct).
        toc_entries = []
        if Path(args.input).suffix.lower() == ".epub":
            toc_entries = _extract_epub_native_toc(args.input, pages, work_dir)
            if toc_entries:
                print(f"Found {len(toc_entries)} chapter(s) in the EPUB's table of contents")
        if args.toc_file:
            toc_entries = parse_toc_file(args.toc_file)
            print(f"Using {len(toc_entries)} chapter(s) from --toc-file")

        if args.max_pages:
            pages = pages[: args.max_pages]
        print(f"Found {len(pages)} pages")

        os.makedirs(args.output_dir, exist_ok=True)

        # Extract and write book metadata (title + author).
        auto_title, auto_author = extract_metadata(args.input, work_dir)
        meta_title = args.title if args.title else auto_title
        meta_author = args.author if args.author else auto_author
        if meta_title or meta_author:
            write_meta(args.output_dir, meta_title, meta_author)

        idx_records = []
        dat_chunks = []
        dat_offset = 0
        total_panels = 0
        total_text_blocks = 0

        for page_idx, src_path in enumerate(pages):
            print(f"[{page_idx + 1}/{len(pages)}] {os.path.basename(src_path)}")

            img = Image.open(src_path)
            # Downscale FIRST, before panel detection: every coordinate downstream (panel boxes,
            # crop rects, OCR text boxes, the page dims in panels.idx) then lives in the resized
            # space, matching the page/crop files actually written -- nothing needs rescaling.
            orig_size = img.size
            img = fit_to_device(img, device_target)
            was_resized = img.size != orig_size
            img_w, img_h = img.size

            # Write the page to a canonical, trivially-sortable filename.
            if args.mono:
                # 1-bit Floyd-Steinberg-dithered BMP (convert("1") defaults to FS dithering). The
                # device renders these BW-only, in a single fast refresh.
                img.convert("L").convert("1").save(os.path.join(args.output_dir, f"page_{page_idx:04d}.bmp"), "BMP")
            else:
                ext = Path(src_path).suffix.lower()
                if ext not in (".jpg", ".jpeg", ".png"):
                    ext = ".jpg"
                    img.convert("RGB").save(
                        os.path.join(args.output_dir, f"page_{page_idx:04d}{ext}"), "JPEG", quality=92
                    )
                elif was_resized:
                    # Resized: the source file no longer matches -- re-encode in the source's own
                    # format so the output keeps its extension (PNG stays PNG, JPEG stays JPEG).
                    if ext == ".png":
                        img.save(os.path.join(args.output_dir, f"page_{page_idx:04d}{ext}"), "PNG", optimize=True)
                    else:
                        img.convert("RGB").save(
                            os.path.join(args.output_dir, f"page_{page_idx:04d}{ext}"), "JPEG", quality=92
                        )
                else:
                    shutil.copy(src_path, os.path.join(args.output_dir, f"page_{page_idx:04d}{ext}"))

            boxes = detect_panels(img)
            boxes = sort_panels_manga_order(boxes)

            # Crop and save every panel first (fast, local) before dispatching
            # the slow network calls concurrently -- OCR is I/O-bound (network
            # latency dominated), so running a page's panels in parallel turns
            # ~N x call_latency into ~call_latency per page.
            panel_paths = []
            panel_rects = []
            for panel_idx, box in enumerate(boxes):
                x1, y1, x2, y2 = box
                mx1 = max(0, x1 - args.panel_margin)
                my1 = max(0, y1 - args.panel_margin)
                mx2 = min(img_w, x2 + args.panel_margin)
                my2 = min(img_h, y2 + args.panel_margin)

                # Skip saving a panel crop when it covers essentially the
                # whole page -- the renderer falls back to displaying the
                # full-page image anyway, so the crop is a redundant copy.
                panel_path = None
                if not is_full_page_panel(box, img_w, img_h):
                    cropped = img.crop((mx1, my1, mx2, my2))
                    if args.mono:
                        panel_path = os.path.join(args.output_dir, f"p{page_idx}_{panel_idx}.bmp")
                        cropped.convert("L").convert("1").save(panel_path, "BMP")
                    else:
                        panel_path = os.path.join(args.output_dir, f"p{page_idx}_{panel_idx}.jpg")
                        cropped.convert("RGB").save(panel_path, "JPEG", quality=90)
                panel_paths.append(panel_path)
                panel_rects.append((mx1, my1, mx2, my2))

            if api_key:
                # Only call Gemini for panels that have a crop file;
                # full-page panels (no crop) get an empty result directly.
                def _ocr_or_empty(p):
                    return call_gemini_panel_ocr(p, api_key) if p else {"blocks": [], "translation": ""}
                with ThreadPoolExecutor(max_workers=min(8, max(1, len(panel_paths)))) as pool:
                    ocr_results = list(pool.map(_ocr_or_empty, panel_paths))
            else:
                ocr_results = [{"blocks": [], "translation": ""} for _ in panel_paths]

            panels_with_text = []
            for panel_idx, box in enumerate(boxes):
                x1, y1, x2, y2 = box
                mx1, my1, mx2, my2 = panel_rects[panel_idx]
                ocr_result = ocr_results[panel_idx]
                translation = ocr_result.get("translation", "")
                panel_w, panel_h = mx2 - mx1, my2 - my1

                text_blocks = []
                for b in ocr_result.get("blocks", []):
                    text = b.get("text", "").strip()
                    if not text:
                        continue
                    bbox = b.get("bbox_2d")
                    if bbox and len(bbox) == 4:
                        ymin, xmin, ymax, xmax = bbox
                        tx1 = x1 + int(xmin / 1000 * panel_w)
                        ty1 = y1 + int(ymin / 1000 * panel_h)
                        tx2 = x1 + int(xmax / 1000 * panel_w)
                        ty2 = y1 + int(ymax / 1000 * panel_h)
                    else:
                        tx1, ty1, tx2, ty2 = x1, y1, x2, y2
                    text_blocks.append({"box": [tx1, ty1, tx2, ty2], "text": text})

                panels_with_text.append({"box": box, "text_blocks": text_blocks, "translation": translation})
                total_panels += 1
                total_text_blocks += len(text_blocks)

            page_data = encode_page(panels_with_text)
            idx_records.append((dat_offset, len(page_data), min(img_w, 0xFFFF), min(img_h, 0xFFFF)))
            dat_chunks.append(page_data)
            dat_offset += len(page_data)

            # Rewrite panels.idx/panels.dat after every page so a crash (or a
            # single panel's API call hanging) never loses already-completed
            # pages' work -- each rewrite is a small, self-consistent index
            # covering exactly the pages processed so far.
            _write_panel_index(args.output_dir, idx_records, dat_chunks)

        idx_path = os.path.join(args.output_dir, "panels.idx")
        dat_path = os.path.join(args.output_dir, "panels.dat")
        idx_size = os.path.getsize(idx_path)
        dat_size = os.path.getsize(dat_path)
        print(f"\nOutput in {args.output_dir}:")
        print(f"  {idx_path}: {idx_size:,} bytes ({len(pages)} pages)")
        print(f"  {dat_path}: {dat_size:,} bytes")
        print(f"  Total: {(idx_size + dat_size) / 1024:.1f} KB")
        print(f"  Panels: {total_panels} ({total_panels / max(len(pages), 1):.1f}/page avg)")
        print(f"  Text blocks: {total_text_blocks}")
        if toc_entries:
            write_toc(args.output_dir, toc_entries)
        print("Done.")
    finally:
        shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
