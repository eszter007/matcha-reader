#!/usr/bin/env python3
"""Convert dictionary files to binary index files for CrossPoint Reader.

Supported input formats:
  Japanese (--lang ja, the default)
  - jmdict-simplified JSON (.json / .json.tgz)
  - Yomitan/Yomichan (.zip containing term_bank_N.json)
  - MDict (.mdx) — requires: pip install readmdict
  Chinese (--lang zh)
  - CC-CEDICT raw text (.u8 / .txt: "繁體 简体 [pin1 yin1] /gloss/gloss/")
  - MoE 重編國語辭典 as the g0v moedict JSON (dict-revised.json, also .json.xz)
  - Yomitan .zip (e.g. the CC-CEDICT Yomitan build) and MDict .mdx, as above

Emits (basename set by --name, default "vocab"):
  vocab.idx   -- sorted array of 40-byte records (headword + offset + length + priority)
  vocab.dat   -- variable-length definition text blob
  vocab.title -- the dictionary's display name (the input's title, or --title)

Usage:
#JMdict(default — downloads if no-- input given)
    python3 convert_jmdict.py [--input jmdict-eng-3.5.0.json] [--output-dir ./output]

#JMdict with per-kanji furigana (a local JmdictFurigana .json/.zip, or "auto" to download)
    python3 convert_jmdict.py --furigana auto --output-dir ./output

#Yomitan dictionary zip
    python3 convert_jmdict.py --input jmdict-yomitan.zip --output-dir ./output

#MDict.mdx file
    python3 convert_jmdict.py --input dictionary.mdx --output-dir ./output

#Chinese : CC - CEDICT, ranked by a frequency list, with zhuyin beside the pinyin
    python3 convert_jmdict.py --lang zh --input cedict_1_0_ts_utf-8_mdbg.txt \
        --frequency dict.txt --zhuyin --output-dir /sd/dictionaries/zh/

#Chinese : CC - CEDICT and the MoE dictionary merged into one vocabulary file
    python3 convert_jmdict.py --lang zh --input cedict_ts.u8 --input dict-revised.json.xz \
        --output-dir /sd/dictionaries/zh/
"""

import argparse
import json
import math
import os
import re
import struct
import sys
import urllib.request

# The sparse-index generator lives with the firmware scripts; the device reads its .spx beside
# each .idx to answer a lookup in two SD reads instead of twenty.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts"))
try:
    from gen_dict_spx import gen_one as _gen_spx
except ImportError:  # running from a copy of this file alone
    _gen_spx = None

# The release file name carries the version, so the latest one is looked up rather than named.
JMDICT_RELEASES_API = "https://api.github.com/repos/scriptin/jmdict-simplified/releases/latest"
JMDICT_ASSET_RE = re.compile(r"^jmdict-eng-\d[^/]*\.json\.tgz$")
FURIGANA_URL = "https://github.com/Doublevil/JmdictFurigana/releases/latest/download/JmdictFurigana.json.zip"
HEADWORD_SIZE = 32
RECORD_FORMAT = f"<{HEADWORD_SIZE}sIHBB"  # headword(32) + offset(4) + length(2) + priority(1) + pad(1)
RECORD_SIZE = struct.calcsize(RECORD_FORMAT)

assert RECORD_SIZE == 40, f"Record size mismatch: {RECORD_SIZE}"

# ── Shared helpers ──────────────────────────────────────────────


def _zip_open(z, name):
    """Opens a zip member without the CRC check: some published Yomitan zips carry a wrong CRC
    for data that is intact, and zipfile then fails at the end of the member."""
    f = z.open(name)
    f._expected_crc = None
    return f


def strip_html(html: str) -> str:
    """Strip HTML tags and decode common entities to plain text."""
    text = re.sub(r"<br\s*/?>", "\n", html, flags=re.IGNORECASE)
    text = re.sub(r"<[^>]+>", "", text)
    text = text.replace("&amp;", "&")
    text = text.replace("&lt;", "<")
    text = text.replace("&gt;", ">")
    text = text.replace("&quot;", '"')
    text = text.replace("&nbsp;", " ")
    text = text.replace("&#x27;", "'")
    text = text.replace("&#39;", "'")
    lines = [line.strip() for line in text.split("\n")]
    return "\n".join(line for line in lines if line)


_ECHO_RE = re.compile(r"^([\u3041-\u30ff・‐\-ー\d０-９\s]+)〖([^〗]+)〗(.*)$")


def _shorten_related(text: str) -> str:
    """Pixiv's related-article lines list dozens of titles; keep the first twelve."""
    def short(m):
        items = m.group(2).split("・")
        return m.group(1) + "・".join(items[:12]) + (f" …（他{len(items) - 12}件）" if len(items) > 12 else "")
    return re.sub(r"(?m)^(・?(?:関連記事|子記事|親記事|兄弟記事): ?)(.+)$", short, text)


def _drop_headword_echo(text: str, headword: str) -> str:
    """Monolingual dictionaries open with their own heading, "いぬ〖犬・狗〗", under the reading the
    panel already shows. Drop it when it says nothing more; keep only the spellings otherwise."""
    m = re.match(r"【([^】]*)】[^\n]*\n+", text)
    if not m:
        return text
    reading = re.sub(r"[^\u3041-\u30ff]", "", m.group(1))
    rest = text[m.end():]
    line, nl, after = rest.partition("\n")
    e = _ECHO_RE.match(line.strip())
    if not e or re.sub(r"[^\u3041-\u30ff]", "", e.group(1)) != reading:
        return text
    spellings, tail = e.group(2), e.group(3).strip()
    if spellings == headword and not tail:
        return text[:m.end()] + after.lstrip("\n")
    return text[:m.end()] + "〖" + spellings + "〗" + (" " + tail if tail else "") + nl + after


def _short_title(title: str) -> str:
    """The device reads the first 39 bytes of a title. Keep whole characters, and drop a bracket
    the cut left open: "違いがわかる事典 (chigai.sit" becomes "違いがわかる事典"."""
    full = title.strip()
    title = full.encode("utf-8")[:39].decode("utf-8", errors="ignore").strip()
    if title != full:
        for open_, close in (("(", ")"), ("（", "）"), ("[", "]"), ("【", "】")):
            cut = title.rfind(open_)
            if cut > 0 and close not in title[cut:]:
                title = title[:cut].strip()
    return title


def write_binary(records: list, output_dir: str, name: str = "vocab", title: str = ""):
    """Write (headword_bytes, definition_bytes, priority, pos_flags) tuples to idx+dat files.

    Expects records to be pre-validated (headword_bytes < HEADWORD_SIZE). A non-empty title is
    written to <name>.title, which the device shows in the lookup panel's footer.
    """
    records.sort(key=lambda r: r[0])

    os.makedirs(output_dir, exist_ok=True)
    dat_path = os.path.join(output_dir, f"{name}.dat")
    idx_path = os.path.join(output_dir, f"{name}.idx")

    dat_offset = 0
    index_entries = []

    with open(dat_path, "wb") as dat_f:
        # One copy per distinct definition: a Chinese entry is indexed under both its traditional
        # and simplified form, which sort far apart, and a Japanese one under kanji and kana.
        stored = {}

        for hw_bytes, def_bytes, priority, pos_flags in records:
            if len(def_bytes) > 0xFFFF:
                def_bytes = def_bytes[:0xFFFF]
            hit = stored.get(def_bytes)
            if hit is None:
                hit = (dat_offset, len(def_bytes))
                dat_f.write(def_bytes)
                dat_offset += len(def_bytes)
                stored[def_bytes] = hit
            offset, length = hit

            padded_hw = hw_bytes + b"\x00" * (HEADWORD_SIZE - len(hw_bytes))
            index_entries.append((padded_hw, offset, length, priority, pos_flags))

    with open(idx_path, "wb") as idx_f:
        for padded_hw, offset, length, priority, pos_flags in index_entries:
            idx_f.write(struct.pack(RECORD_FORMAT, padded_hw, offset, length, priority, pos_flags))

    title_path = os.path.join(output_dir, f"{name}.title")
    title = _short_title(title)
    if title:
        with open(title_path, "w", encoding="utf-8") as title_f:
            title_f.write(title + "\n")
    elif os.path.exists(title_path):
        os.remove(title_path)  # a stale name from an earlier conversion would mislabel this one

    dat_size = os.path.getsize(dat_path)
    idx_size = os.path.getsize(idx_path)
    print(f"Output:")
    print(f"  {idx_path}: {idx_size:,} bytes ({len(index_entries):,} records)")
    print(f"  {dat_path}: {dat_size:,} bytes")
    if title:
        print(f"  {title_path}: {title}")
    spx_path = os.path.join(output_dir, f"{name}.spx")
    if _gen_spx is not None and index_entries:
        _gen_spx(idx_path, spx_path)
        print(f"  {spx_path}: {os.path.getsize(spx_path):,} bytes (sparse index)")
    elif os.path.exists(spx_path):
        os.remove(spx_path)  # a stale sidecar for an older .idx would be ignored, but keep it tidy
        print(f"  {name}.spx: not generated (scripts/gen_dict_spx.py not found); run it on the folder")
    print(f"  Total: {(idx_size + dat_size) / 1024 / 1024:.1f} MB")

# ── JMdict(jmdict - simplified JSON) ─────────────────────────────


def latest_jmdict_url() -> str:
    """URL of the full English JMdict (not the common-words subset) in the latest release."""
    with urllib.request.urlopen(JMDICT_RELEASES_API) as response:
        release = json.load(response)
    for asset in release.get("assets", []):
        if JMDICT_ASSET_RE.match(asset.get("name", "")):
            return asset["browser_download_url"]
    raise RuntimeError("No jmdict-eng .json.tgz in the latest jmdict-simplified release")


def download_jmdict(output_path: str) -> str:
    """Download and extract jmdict-simplified JSON."""
    import tarfile

    tgz_path = output_path + ".tgz"
    if not os.path.exists(tgz_path):
        url = latest_jmdict_url()
        print(f"Downloading {url}...")
        urllib.request.urlretrieve(url, tgz_path)
        print(f"Downloaded to {tgz_path}")

    json_path = None
    with tarfile.open(tgz_path, "r:gz") as tar:
        for member in tar.getmembers():
            if member.name.endswith(".json"):
                tar.extract(member, os.path.dirname(output_path))
                json_path = os.path.join(os.path.dirname(output_path), member.name)
                break

    if not json_path:
        print("ERROR: No JSON file found in tarball", file=sys.stderr)
        sys.exit(1)

    return json_path


# ── JmdictFurigana ─────────────────────────────────────────────


def furigana_to_anki(segments) -> str:
    """Anki furigana syntax (木[こ]漏[も]れ 日[び]) for JmdictFurigana segments; "" to skip."""
    out = ""
    has_rt = False
    for seg in segments:
        ruby = seg.get("ruby", "")
        rt = seg.get("rt")
        if rt:
            # Anki takes the base as everything back to the previous space or "]".
            if out and not out.endswith("]"):
                out += " "
            out += f"{ruby}[{rt}]"
            has_rt = True
        else:
            out += ruby
    if not has_rt or "｝" in out:
        return ""
    return out


def load_furigana(path: str) -> dict:
    """Load a JmdictFurigana .json (or a .zip holding it) into {(text, reading): anki_string}."""
    import zipfile

    print(f"Loading furigana {path}...")
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            name = next((n for n in z.namelist() if n.endswith(".json")), None)
            if not name:
                print("ERROR: No .json file found in furigana zip", file=sys.stderr)
                sys.exit(1)
            with _zip_open(z, name) as f:
                raw = f.read()
    else:
        with open(path, "rb") as f:
            raw = f.read()
    data = json.loads(raw.decode("utf-8-sig"))
    furigana = {}
    for item in data:
        anki = furigana_to_anki(item.get("furigana", []))
        if anki:
            furigana[(item.get("text", ""), item.get("reading", ""))] = anki
    print(f"  {len(furigana)} furigana entries")
    return furigana


def download_furigana(output_dir: str) -> str:
    """Download the latest JmdictFurigana .json.zip into output_dir (once)."""
    os.makedirs(output_dir, exist_ok=True)
    path = os.path.join(output_dir, "JmdictFurigana.json.zip")
    if not os.path.exists(path):
        print(f"Downloading {FURIGANA_URL}...")
        urllib.request.urlretrieve(FURIGANA_URL, path)
        print(f"Downloaded to {path}")
    return path


def compute_priority_jmdict(entry: dict) -> int:
    """Map JMdict priority tags to a 0-255 score (higher = more common)."""
    is_common = False
    for kanji in entry.get("kanji", []):
        if kanji.get("common", False):
            is_common = True
            break
    if not is_common:
        for kana in entry.get("kana", []):
            if kana.get("common", False):
                is_common = True
                break
    return 200 if is_common else 100

#Part - of - speech flag bits-- must mirror DictIndexRecord::POS_* in lib / Dict / DictIndex.h.
# 0 means "no POS data" and the firmware then accepts every deinflection candidate, so leaving
#flags unset is always safe(fail open).
POS_V1 = 0x01     # ichidan verb
POS_V5 = 0x02     # godan verb
POS_VS = 0x04     # suru verb
POS_VK = 0x08     # kuru verb
POS_ADJ_I = 0x10  # i-adjective
POS_OTHER = 0x20  # tagged, but none of the above (noun, particle, na-adjective, ...)
POS_READING = 0x40  # kana READING record of an entry that has kanji headwords (not a kana lemma)
POS_ANY_VERB = POS_V1 | POS_V5 | POS_VS | POS_VK


def pos_flags_from_tags(tags) -> int:
    """Map JMdict partOfSpeech tags / Yomitan rules to POS flag bits.

    Prefix-matches the verb classes so subtags stay covered (v5k-s, v5aru, v1-s, vs-i, adj-ix).
    A verb-ish tag with an unrecognized class fails OPEN (all verb bits) rather than closed --
    a wrongly-rejected real conjugation is worse than letting a rare archaic verb through.
    Transitivity tags (vt/vi) say nothing about conjugation class and are ignored.
    """
    flags = 0
    for t in tags:
        if not t:
            continue
        if t in ("vt", "vi", "aux", "aux-adj", "exp"):
            continue  # not a conjugation class
        if t.startswith("v1"):
            flags |= POS_V1
        elif t.startswith("v5") or t.startswith("v4") or t.startswith("iv"):
            flags |= POS_V5  # v4* (archaic yodan) conjugates closest to godan for our rule set
        elif t.startswith("vs"):
            flags |= POS_VS
        elif t.startswith("vk"):
            flags |= POS_VK
        elif t.startswith("adj-i"):
            flags |= POS_ADJ_I
        elif t.startswith("v") or t == "aux-v":
            flags |= POS_ANY_VERB  # unknown verb subtype: fail open across verb classes
        else:
            flags |= POS_OTHER
    return flags


def pos_flags_jmdict(entry: dict) -> int:
    tags = []
    for sense in entry.get("sense", []):
        tags.extend(sense.get("partOfSpeech", []))
    return pos_flags_from_tags(tags)

def format_definition_jmdict(entry: dict, furigana: str = "") -> str:
    """Format an entry's readings and glosses into a compact display string.

    furigana (Anki syntax) is appended to the reading line as ｛...｝; old firmware drops the line.
    """
    parts = []

    readings = [k["text"] for k in entry.get("kana", [])]
    if readings:
        line = "【" + "、".join(readings[:3]) + "】"
        if furigana:
            line += "｛" + furigana + "｝"
        parts.append(line)

    senses = entry.get("sense", [])
    for i, sense in enumerate(senses[:3]):
        glosses = [g["text"] for g in sense.get("gloss", [])]
        if glosses:
            prefix = f"{i+1}. " if len(senses) > 1 else ""
            parts.append(prefix + "; ".join(glosses[:4]))

    return "\n".join(parts)


def furigana_for_kanji(entry: dict, kanji: str, furigana: dict) -> str:
    """Furigana for the first kana reading that applies to kanji and has a dataset entry."""
    for kana in entry.get("kana", []):
        applies = kana.get("appliesToKanji", ["*"])
        if "*" in applies or kanji in applies:
            anki = furigana.get((kanji, kana["text"]))
            if anki:
                return anki
    return ""


def convert_jmdict(json_path: str, furigana: dict = None) -> list:
    """Convert JMdict JSON to index records."""
    print(f"Loading {json_path}...")
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    words = data.get("words", [])
    print(f"Processing {len(words)} JMdict entries...")

    records = []
    furigana_count = 0
    for entry in words:
        definition = format_definition_jmdict(entry)
        def_bytes = definition.encode("utf-8")
        priority = compute_priority_jmdict(entry)
        pos_flags = pos_flags_jmdict(entry)

        seen_headwords = set()
        for kanji in entry.get("kanji", []):
            hw = kanji["text"]
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE or hw_bytes in seen_headwords:
                continue
            seen_headwords.add(hw_bytes)
            anki = furigana_for_kanji(entry, hw, furigana) if furigana else ""
            if anki:
                furigana_count += 1
                records.append((hw_bytes, format_definition_jmdict(entry, anki).encode("utf-8"), priority,
                                pos_flags))
            else:
                records.append((hw_bytes, def_bytes, priority, pos_flags))

#Kana records of an entry that also has kanji headwords are READING records : text that
#matches them is usually conjugation morphology, not the word(しながら vs 品柄).The
#firmware suppresses uncommon flagged records in hiragana segmentation.Entries with no
#kanji form at all are kana LEMMAS-- their kana record is the word itself, unflagged.
        kana_flags = pos_flags | (POS_READING if entry.get("kanji") else 0)
        for kana in entry.get("kana", []):
            hw = kana["text"]
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE or hw_bytes in seen_headwords:
                continue
            seen_headwords.add(hw_bytes)
            records.append((hw_bytes, def_bytes, priority, kana_flags))

    print(f"Generated {len(records)} index records")
    if furigana is not None:
        print(f"Furigana attached to {furigana_count} headwords")
    return records

# ── Yomitan / Yomichan(.zip) ───────────────────────────────────


def _is_example_list(node: dict) -> bool:
    """A list whose data attributes name it as example sentences (surasura "examples",
    CantoDict "sentences"): its items print as plain lines, the device's example layout."""
    data = node.get("data")
    if not isinstance(data, dict):
        return False
    return any(isinstance(v, str) and ("example" in v.lower() or "sentence" in v.lower()) for v in data.values())


def _list_style_none(node) -> bool:
    style = node.get("style") if isinstance(node, dict) else None
    return isinstance(style, dict) and style.get("listStyleType") == "none"


def _flatten_list(node: dict) -> str:
    """A ul/ol: "• " per item, plain lines for example lists and unmarked items, and no line at
    all for an item left empty (a dropped web link)."""
    plain = _is_example_list(node)
    items = node.get("content", "")
    items = items if isinstance(items, list) else [items]
    # Onomatoproject wraps each li in its own array: [[li], [li], ...].
    items = [x for it in items for x in (it if isinstance(it, list) and all(
        isinstance(y, dict) and y.get("tag") == "li" for y in it) else [it])]
    marker = "• " if _is_gloss_list(node, items) else "・"
    lines = []
    for item in items:
        if isinstance(item, dict) and item.get("tag") == "li":
            text = flatten_structured_content(item.get("content", "")).strip()
            if not text:
                continue
            lines.append(text if plain or _list_style_none(item) else marker + text)
        else:
            text = flatten_structured_content(item).strip()
            if text:
                lines.append(text)
    if not plain and _alternates_with_translation([l[len(marker):] if l.startswith(marker) else l for l in lines]):
        lines = [l[len(marker):] if l.startswith(marker) else l for l in lines]
    return "\n" + "\n".join(lines) + "\n" if lines else ""


def _is_gloss_list(node: dict, items: list) -> bool:
    """A list of glosses, which the device joins into one numbered sense: Jitendex marks it
    data-content "glossary"; a list of foreign-language glosses has no Japanese in it. Other lists
    (類語例解's usage notes) keep their own lines."""
    data = node.get("data")
    if isinstance(data, dict) and data.get("content") == "glossary":
        return True
    text = flatten_structured_content(items)
    return not re.search(r"[\u3040-\u30ff\u3400-\u9fff]", text)


def _alternates_with_translation(lines: list) -> bool:
    """Items that pair a Japanese sentence with its Latin-script translation (斎藤's 犬がほえる /
    A dog barks.) are examples, whatever the list is called."""
    if len(lines) < 2 or len(lines) % 2:
        return False
    jp = re.compile(r"[\u3040-\u30ff\u3400-\u9fff々〜～（「]")
    latin = re.compile(r"[\"'“‘(\[]?[A-Za-z\u00c0-\u04ff]")
    return all(jp.match(lines[i]) and latin.match(lines[i + 1]) for i in range(0, len(lines), 2))


# Style suffixes in label-icon file names: 名-rect.svg, 季-default.svg, ショ-rect-red.svg.
_IMAGE_STYLE_SUFFIXES = ("rect", "fill", "default", "red", "redfill", "round", "circle", "box", "black", "white")
_FILLED_DIGITS = {c: chr(0x2460 + i) for i, c in enumerate("１２３４５６７８９")}
_FILLED_DIGITS.update({c: chr(0x2460 + i) for i, c in enumerate("一二三四五六七八九")})


# Pictures in the panel: the zip being converted (set while its banks are read) and the BMPs made
# from it, written to <output>/images/ beside the dictionary files.
_IMAGE_ZIP = None
_IMAGES: dict = {}
IMAGES_ENABLED = False  # --images
IMAGE_MAX_WIDTH = 400
IMAGE_MAX_HEIGHT = 320
IMAGE_MARKER = "\ue000"  # a definition line "\ue000<w>x<h> <file>" is a picture (private use)
# First line of an entry from a merged file: the name of the dictionary it came from.
SOURCE_MARKER = "\ue001"


def _bmp_2bit(img) -> bytes:
    """A 4-level grayscale picture (Pillow "L" image of 0/85/170/255) as a 2-bit palette BMP, the
    depth the reader draws straight from the card."""
    import struct

    w, h = img.size
    row_bytes = ((w * 2 + 31) // 32) * 4
    px = img.load()
    rows = []
    for y in range(h - 1, -1, -1):  # bottom-up
        row = bytearray(row_bytes)
        for x in range(w):
            level = px[x, y] // 85
            row[x >> 2] |= level << (6 - 2 * (x & 3))
        rows.append(bytes(row))
    palette = b"".join(bytes((v, v, v, 0)) for v in (0, 85, 170, 255))
    data_offset = 14 + 40 + len(palette)
    size = data_offset + row_bytes * h
    header = b"BM" + struct.pack("<IHHI", size, 0, 0, data_offset)
    info = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 2, 0, row_bytes * h, 2835, 2835, 4, 4)
    return header + info + palette + b"".join(rows)


def _picture_marker(path: str, min_side: int = 0) -> str:
    """Converts a picture of the zip being read to a BMP and returns its marker line, or "" when
    Pillow is missing or the image cannot be read."""
    if _IMAGE_ZIP is None or not IMAGES_ENABLED:
        return ""
    try:
        from PIL import Image
    except ImportError:
        return ""
    import hashlib
    import io

    name = hashlib.sha1(path.encode("utf-8")).hexdigest()[:16] + ".bmp"
    cached = _IMAGES.get(name)
    if cached is None:
        try:
            with _zip_open(_IMAGE_ZIP, path) as f:
                img = Image.open(io.BytesIO(f.read()))
                img.load()
        except Exception:
            return ""
        if min(img.size) < min_side:
            return ""  # an inline glyph image (gaiji), not a picture
        if img.mode in ("RGBA", "LA", "P"):
            img = img.convert("RGBA")
            backdrop = Image.new("RGBA", img.size, (255, 255, 255, 255))
            img = Image.alpha_composite(backdrop, img)
        img = img.convert("L")
        img.thumbnail((IMAGE_MAX_WIDTH, IMAGE_MAX_HEIGHT), Image.LANCZOS)
        four = Image.new("P", (1, 1))
        four.putpalette([v for g in (0, 85, 170, 255) for v in (g, g, g)] + [0] * (256 - 4) * 3)
        indices = img.convert("RGB").quantize(palette=four, dither=Image.Dither.FLOYDSTEINBERG)
        img = Image.new("L", indices.size)
        img.putdata([(0, 85, 170, 255)[i] if i < 4 else 255 for i in indices.getdata()])
        cached = (img.size, _bmp_2bit(img))
        _IMAGES[name] = cached
    (w, h), _ = cached
    return f"\n{IMAGE_MARKER}{w}x{h} {name}\n"


def write_images(output_dir: str) -> None:
    if not _IMAGES:
        return
    folder = os.path.join(output_dir, "images")
    os.makedirs(folder, exist_ok=True)
    total = 0
    for name, (_, data) in _IMAGES.items():
        with open(os.path.join(folder, name), "wb") as f:
            f.write(data)
        total += len(data)
    print(f"  {folder}: {len(_IMAGES)} pictures, {total / 1e6:.1f} MB")


def _image_text(node: dict) -> str:
    """Text for a structured-content image. Label icons are named after their label (名-rect.svg,
    文法-red.svg, １-fill.svg, red_（.svg) and become that label; other images keep their alt or
    title text, and pictures without one are dropped."""
    path = node.get("path", "")
    path = path if isinstance(path, str) else ""
    folder, _, fname = path.rpartition("/")
    stem, dot, ext = fname.rpartition(".")
    if dot and ext.lower() in ("jpg", "jpeg", "webp", "gif", "bmp"):
        return _picture_marker(path)  # photos and drawings; their alt text is a caption or credit
    data = node.get("data") if isinstance(node.get("data"), dict) else {}
    for value in (node.get("alt"), node.get("title"), data.get("alt")):
        if isinstance(value, str) and value.strip() and "://" not in value:
            # 明鏡 describes its accent marks in words ("3分の2"); in "gaiji" they are noise.
            return "" if data.get("class") == "gaiji" and folder == "gaiji" else value.strip()
    if folder == "gaiji" and re.match(r"[A-Za-z]{1,3}[^A-Za-z]", stem):
        return ""  # a gaiji code with a kanji suffix (bs一) is a mark, not text
    if not dot:
        return ""
    if "accent" in folder:
        return ""  # accent glyphs; pitch comes from the pitch dictionaries
    if stem.startswith("red_"):
        return stem[4:]
    parts = stem.split("-")
    while len(parts) > 1 and parts[-1].lower() in _IMAGE_STYLE_SUFFIXES:
        parts.pop()
    label = "-".join(parts)
    if not label or all(c in "0123456789ABCDEFabcdefSb" for c in label) or label.isascii():
        # Gaiji codes (B92D, AC6E, b327) have no text; a larger PNG is a drawing.
        return _picture_marker(path, min_side=48) if ext.lower() == "png" else ""
    if "fill" in stem.split("-")[1:] and label in _FILLED_DIGITS:
        return _FILLED_DIGITS[label]
    if len(label) <= 6:
        return f"〔{label}〕"
    return label



def flatten_structured_content(content) -> str:
    """Recursively extract display text from Yomitan structured content,
    using the semantic data-content attributes from Jitendex."""
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        return "".join(flatten_structured_content(item) for item in content)
    if isinstance(content, dict):
        ctype = content.get("type", "")
        if ctype == "text":
            return content.get("text", "")
        if ctype == "image" or content.get("tag") == "img":
            return _image_text(content)
        if ctype == "structured-content":
            return flatten_structured_content(content.get("content", ""))

        inner = content.get("content", "")
        tag = content.get("tag", "")
        data = content.get("data", {}) if isinstance(content.get("data"), dict) else {
}
        dc = data.get("content", "")
        cls = data.get("class", "")
        text = flatten_structured_content(inner)

        if tag == "br":
            return "\n"
        # A web link cannot be followed on the device (Pixiv's 続きを読む, Nico/Pixiv's site names).
        href = content.get("href", "")
        if tag == "a" and isinstance(href, str) and href.startswith(("http://", "https://")):
            return ""
        # Monolingual dictionaries (大辞林) label parts with data.name. Map them to the layout the
        # device formats: "• " gloss lines, "[pos]" tag lines, and plain lines for examples. 見出部
        # and 語義Gnum repeat what the panel already shows; ━ stands for the headword and draws badly.
        dn = data.get("name", "")
        dn = dn if isinstance(dn, str) else ""
        if dn in ("見出部", "語義Gnum"):
            return ""
        if dn == "品詞G":
            return "\n[" + re.sub(r"[（）()]", "", text).strip() + "]\n"
        if dn == "語釈":
            return "\n• " + text + "\n"
        if dn == "見出相当部":
            return text.replace("━", "〜")
        if dn and tag == "div":
            # A short label line (㋐) joins the gloss after it: "• ㋐ すっかり。".
            body = re.sub(r"\n+", "\n", text)
            body = body[1:] if body.startswith("\n") else body
            body = body[:-1] if body.endswith("\n") else body
            body = re.sub(r"^([^\n•\[\x1f]{1,3})\n• ", r"• \1 ", body, count=1)
            # A 語義G holds one numbered sense (①, ②): a blank line keeps it apart from the next.
            # \x1f survives the outer divs' newline collapsing; _clean_markup makes it a blank line.
            return ("\n\x1f\n" if dn == "語義G" else "\n") + body + "\n"
        if tag == "rt":
            return ""
        # Tables (類句 lists, conjugation grids): one row per line, cells apart.
        if tag == "summary":
            return ""  # a "show 5 examples" toggle; the details print in full
        # A bordered badge ("Definitions", "Examples") heads what follows it.
        style = content.get("style") if isinstance(content.get("style"), dict) else {}
        if tag == "span" and style.get("borderStyle") and text.strip() and len(text.strip()) <= 20:
            return "\n■ " + text.strip() + "\n"
        if tag == "table":
            return "\n" + text
        if tag == "tr":
            # \x1e marks a header cell: "label：value" beside data, "a · b" in a header row.
            has_data = "　" in text
            row = text.strip().rstrip("\x1e　").strip()
            row = row.replace("\x1e", "：" if has_data else " · ")
            return row.replace("　", " · ") + "\n"
        if tag in ("td", "th"):
            return text.strip() + ("\x1e" if tag == "th" else "　")
        # A section header block (Pixiv's related-tags-header "関連記事"): a heading line.
        if tag == "div" and isinstance(content.get("data"), dict) and any(
                isinstance(v, str) and v.endswith("header") and v != "nav-header" for v in content["data"].values()):
            heading = re.sub(r"^[•\s]+", "", text.strip())
            return "\n■ " + heading + "\n" if heading else ""
        if tag == "ruby":
            return text

#All tag - class spans → [noun][math][colloquial] etc.inline
        if cls == "tag" and dc in ("part-of-speech-info", "field-info", "misc-info",
                                   "dialect-info", "language-info"):
            return "[" + text + "] "
        if cls == "tag" and dc == "forms-label":
            return ""

#Glossary list items — newline before to separate from POS tags
        if dc == "glossary" and tag == "ul":
            return "\n" + text
        if tag == "li" and not dc:
            return "• " + text.strip() + "\n"

#Sense groups
        if dc == "sense-group" and tag in ("li", "div"):
            return text + "\n"
        if dc == "sense" and tag == "li":
            return text

#Notes — indented with arrow
        if dc == "sense-note-label":
            return text + ": "
        if dc == "sense-note-content":
            return text + "\n"
        if dc == "sense-note" and cls == "extra-box":
            return "  → " + text

#Example sentences — each part on its own line, indented
        if dc == "example-sentence-a":
            return text + "\n"
        if dc == "example-sentence-b":
            return text + "\n"
        if dc == "example-sentence" and cls == "extra-box":
            return "  " + text
        if dc == "example-keyword":
            return text

#Cross - references
        if dc == "xref" and cls == "extra-box":
            return ""
        if dc == "reference-label":
            return text + " "

#Other forms — skip to save space
        if dc == "forms":
            return ""

#Attribution
        if dc == "attribution-footnote":
            return ""

#Generic block elements
        if tag in ("div", "p", "blockquote", "section"):
            if dc == "extra-info":
                return text
            return "\n" + text + "\n" if text.strip() else ""
        if tag in ("ol", "ul"):
            return _flatten_list(content)

        return text
    return str(content)


def _headings_as_notes(text: str) -> str:
    """Section headings ("■ 関連記事") joined to the list under them as one labelled line, which the
    device sets as a wrapped block: "関連記事: 高校生・…". A lone heading is a "→ " note. Pixiv's
    category path ("⋯⋯学ぶ ＞ 学校") goes last as "分類: …": a "→ " line opening an entry would be
    read as another headword."""
    out = []
    path = ""
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("■ "):
            j = i + 1
            while j < len(lines) and not lines[j].strip():
                j += 1
            nxt = lines[j].strip() if j < len(lines) else ""
            if nxt and not nxt.startswith(("■ ", "• ", "・")):
                out.append(line[2:] + ": " + nxt)
                i = j + 1
                continue
            out.append("→ " + line[2:])
        elif line.startswith("⋯⋯"):
            path = line[2:].strip()
        else:
            out.append(lines[i])
        i += 1
    if path:
        out.append("分類: " + path)
    return "\n".join(out)


_HTML_TAG = re.compile(r"</?(ruby|rb|rp|span|font|b|i|u|em|strong|sup|sub|small|big|p|div|br|a)(\s[^<>]*)?/?>", re.I)
_RT = re.compile(r"<rt>.*?</rt>|<rp>.*?</rp>", re.I | re.S)


def _clean_markup(text: str) -> str:
    """Removes markup some dictionaries leave inside their text: HTML tags (ruby keeps its base
    text), placeholders for characters the source could not encode, and web junk lines."""
    if "<" in text:
        text = _RT.sub("", text)
        text = re.sub(r"<br\s*/?>", "\n", text, flags=re.I)
        text = _HTML_TAG.sub("", text)
        text = text.replace("<unencoded>", "〓")
    text = re.sub(r"\{\{w_\d+\}\}", "〓", text)
    text = re.sub(r"\n?\x1f\n?", "\n\n", text)
    # CC-CEDICT in Yomitan form: "【衬衫】 【襯衫】 [3304] shirt/blouse". The forms repeat the
    # headword and the rank is noise; each gloss gets its own line. A "gloss" that only lists
    # the forms again ("(襯衫,衬衫)", 漢語大詞典) is dropped.
    def _cedict_line(m):
        glosses = m.group(3).strip()
        if re.fullmatch(r"[(（][^)）a-zA-Z]*[)）]", glosses):
            return ""
        return "\n".join("• " + g.strip() for g in glosses.split("/") if g.strip())
    text = re.sub(r"(?m)^[【〖]([^】〗]+)[】〗] [【〖]([^】〗]+)[】〗](?: \[\d+\])? (.+)$", _cedict_line, text)
    # A heading glued to its text by a list bullet ("・違い• 「猫に小判」と…"): the text starts a line.
    text = re.sub(r"(?<=[^\s•])• ?", "\n", text)
    text = re.sub(r"(?m)^(\[[^\]\n]*\]|→ [^\n]*)\n\n(?=• )", r"\1\n", text)  # a tag line stays on its sense
    text = re.sub(r"｛([0-9０-９]+)｝", r"（\1）", text)  # a source's year: 〔特殊語百科辞典｛１９３１｝〕
    text = _shorten_related(text)
    text = text.replace("┏", "")  # 研究社 marks the scope of a following [variant] with ┏
    text = text.replace("►", "▶").replace("━", "〜")  # ━ stands for the headword  # ► is missing from Japanese fonts; ▶ is a JIS symbol
    # 大辞林 opens each part of an entry with a kanji numeral and its pitch ("一[2]"). The part's
    # own tag line ([接頭]) already marks the break, and a leading "→ " would name the page.
    text = re.sub(r"(?m)^[一二三四五六七八九十]{1,2}(?:\[\d+\])?[ \t]*$", "", text)
    # Dingbat circled digits (❶, ➀) are missing too; ① to ⑩ are in every Japanese font.
    text = re.sub(r"[\u2776-\u277f\u2780-\u2789\u278a-\u2793]",
                  lambda m: chr(0x2460 + (ord(m.group()) - 0x2776) % 10), text)
    # Emoji have no glyph on the device: one leading a heading ("📚使い方") becomes ■.
    text = re.sub(r"(?m)^[\U0001F000-\U0001FAFF]\ufe0f?\s*", "■ ", text)
    text = re.sub(r"[\U0001F000-\U0001FAFF]\ufe0f?", "", text)
    text = re.sub(r"(?m)^.*(adsbygoogle|google\.com/search\?).*$\n?", "", text)
    # A line that is only a web address (scheme dropped with its link) cannot be followed.
    text = re.sub(r"(?m)^[ \t]*(https?://)?[\w-]+(\.[\w-]+)+/\S*[ \t]*$\n?", "", text)
    # Wiktionary maintenance notes ("…並移除{{rfdef}}模板。").
    text = re.sub(r"[^\n。]*\{\{[\w-]+\}\}[^\n。]*。?", "", text)
    if "{" in text:
        # Wadoku cross-references carry an entry number: {百均（ひゃっきん）:81491}.
        text = re.sub(r"\{([^{}:\n]+):\d+\}", r"\1", text)
        # 斎藤 marks a reading in braces after the kanji: 莫連{ばくれん}.
        text = re.sub(r"(?<=[\u3400-\u9fff々])\{([\u3041-\u309f\u30fc]+)\}", r"（\1）", text)
    return text


def format_definition_yomitan(headword: str, reading: str, definitions, furigana: str = "") -> str:
    """Format a Yomitan entry's reading + definitions into display string."""
    parts = []
#Only show reading if it differs from headword(skip for kana - only entries)
    if reading and reading != headword:
        parts.append(f"【{reading}】" + (f"｛{furigana}｝" if furigana else ""))

    def flatten_list_defn(d) -> str:
#Yomitan list - form definitions(variant / redirect entries) like
#["引っ張り上げる", ["redirected from 引っぱり上げる"]].Join the string
#parts; drop "redirected from ..." cross - reference noise.
        if isinstance(d, str):
            if d.startswith("redirected from"):
                return ""
            return d
        if isinstance(d, dict):
            return flatten_structured_content(d)
        if isinstance(d, list):
            pieces = [flatten_list_defn(x) for x in d]
            return " ".join(p for p in pieces if p)
        return ""

    # Plain-string glosses of one entry are one sense (JMdict's older Yomitan build lists
    # "friendship", "friendly relations", ... separately): one line, not a numbered list.
    # A multi-line string is a sense or paragraph of its own (MoE builds); those stay apart.
    if isinstance(definitions, list) and len(definitions) > 1 and \
            all(isinstance(d, str) and "\n" not in d.strip() for d in definitions):
        definitions = ["; ".join(dict.fromkeys(d.strip() for d in definitions[:6] if d.strip()))]

    if isinstance(definitions, list):
        non_empty = []
        for defn in definitions[:6]:
            if isinstance(defn, str):
                text = defn
            elif isinstance(defn, dict):
                text = flatten_structured_content(defn)
            elif isinstance(defn, list):
                text = flatten_list_defn(defn)
            else:
                text = str(defn)
            # The device reads a line holding 【】 as the reading and drops it, which in monolingual
            # dictionaries (あい【愛】…) deletes the definition. Only the reading line above keeps them.
            text = _clean_markup(text).strip().replace("【", "〖").replace("】", "〗")
            # A line of only a symbol (the Mongolian dictionary's ◇ before its examples) is a break,
            # and the reader fonts lack most of these symbols.
            text = re.sub(r"(?m)^[\s◇◆□■○●☆★※]+$", "", text).strip()
            text = _headings_as_notes(text)
            # Separators left behind by dropped web links ("|") are not a definition.
            if re.search(r"\w", text):
                non_empty.append(text)
#Drop pure - duplicate entries(redirect variant repeating the first gloss)
        deduped = []
        for t in non_empty:
            if t not in deduped:
                deduped.append(t)
        non_empty = deduped
        if not non_empty:
            return ""  # a reading alone is no entry

        # A picture-only definition is no sense: it stays unnumbered so the reader sees its marker.
        senses = [t for t in non_empty if not t.startswith(IMAGE_MARKER)]
        number = 0
        for text in non_empty:
            if len(senses) > 1 and not text.startswith(IMAGE_MARKER):
                number += 1
                parts.append(f"\n{number}. {text}")
            else:
                parts.append(f"\n{text}")

    result = "\n".join(parts)
    result = re.sub(r'[ \t]+', ' ', result)
    result = re.sub(r'\n{3,}', '\n\n', result)
    result = re.sub(r'• • ', '• ', result)
    return result.strip()


def find_redirect_target(definitions) -> str:
    """If a Yomitan entry is purely a redirect (variant spelling pointing to a
    canonical headword via a 'redirect-glossary'), return the target headword.
    Otherwise return ''."""
    def search(node):
        if isinstance(node, dict):
            data = node.get("data")
            if isinstance(data, dict) and data.get("content") == "redirect-glossary":
#The target headword is the link text(strip the ⟶ arrow).
                txt = flatten_structured_content(node).replace("⟶", "").strip()
                return txt
            return search(node.get("content"))
        if isinstance(node, list):
            for x in node:
                r = search(x)
                if r:
                    return r
        return ""
    return search(definitions)


def _with_tag_line(definition: str, tags: str) -> str:
    """Puts an entry's tags (KRDICT's part of speech and level: 감탄사 초급) on a line of their own
    after the reading line."""
    label = "〔" + " · ".join(tags.split()) + "〕"
    if definition.startswith("【"):
        head, _, rest = definition.partition("\n")
        return f"{head}\n{label}\n{rest.lstrip(chr(10))}"
    return f"{label}\n{definition.lstrip(chr(10))}"


def convert_yomitan(zip_path: str, reading_records: bool = True, furigana: dict = None,
                    show_tags: bool = False) -> tuple:
    """Convert a Yomitan/Yomichan .zip dictionary to index records.

    reading_records=False skips the extra record per kana reading. Chinese dictionaries carry
    pinyin in the reading field, which no page text ever matches, so the records would only
    bloat the index. Returns (records, title).
    """
    import zipfile

    global _IMAGE_ZIP
    print(f"Loading {zip_path}...")
    title = ""

    with zipfile.ZipFile(zip_path, "r") as z:
        names = z.namelist()
        _IMAGE_ZIP = z

        if "index.json" in names:
            with _zip_open(z, "index.json") as f:
                meta = json.load(f)
            title = str(meta.get("title", ""))
            print(f"  Dictionary: {title or '(unknown)'}")
            print(f"  Format version: {meta.get('format', meta.get('version', '?'))}")

        term_banks = sorted(n for n in names if re.match(r"term_bank_\d+\.json$", n))
        if not term_banks:
            if any(re.match(r"kanji_bank_\d+\.json$", n) for n in names):
                print("ERROR: This is a kanji dictionary (one entry per character). Convert it with "
                      "--name kanji.", file=sys.stderr)
            elif any(re.match(r"(term|kanji)_meta_bank_\d+\.json$", n) for n in names):
                print("ERROR: This is a frequency list with no definitions, not a dictionary. Pass it "
                      "as --frequency to rank another dictionary's entries.", file=sys.stderr)
            else:
                print("ERROR: No term_bank_N.json files found in zip", file=sys.stderr)
            sys.exit(1)

        print(f"  Found {len(term_banks)} term bank files")

#Pass 1 : load all entries; build a headword → best definition map for
#non - redirect entries so variant / redirect entries can be resolved.
        all_entries = []
        canonical_defs = {}  # headword → (definition_string, priority)
        for bank_name in term_banks:
            with _zip_open(z, bank_name) as f:
                entries = json.load(f)
            for entry in entries:
                if not isinstance(entry, list) or len(entry) < 6:
                    continue
                headword = entry[0]
                if not headword or not isinstance(headword, str):
                    continue
                reading = entry[1] if len(entry) > 1 else ""
                # Some Pixiv builds glue the headword in front of the kana reading.
                if (isinstance(reading, str) and reading != headword and reading.startswith(headword)
                        and re.fullmatch(r"[\u3040-\u30ffー]+", reading[len(headword):])):
                    reading = reading[len(headword):]
                rules = entry[3] if len(entry) > 3 and isinstance(entry[3], str) else ""
                score = entry[4] if len(entry) > 4 else 0
                definitions = entry[5] if len(entry) > 5 else []
                redirect = find_redirect_target(definitions)
                tags = entry[2] if isinstance(entry[2], str) else ""
                all_entries.append((headword, reading, score, definitions, redirect, rules, tags))
                if not redirect:
                    anki = furigana.get((headword, reading), "") if furigana else ""
                    definition = format_definition_yomitan(headword, reading, definitions, anki)
                    if definition:
                        priority = max(0, min(255, int(score) + 128)) if isinstance(score, (int, float)) else 100
                        prev = canonical_defs.get(headword)
                        if prev is None or priority > prev[1]:
                            canonical_defs[headword] = (definition, priority)

#Pass 2 : emit records, resolving redirects to the target's real definition.
        records = []
        entry_count = 0
        furigana_count = 0
        for headword, reading, score, definitions, redirect, rules, tags in all_entries:
            if redirect:
                target = canonical_defs.get(redirect)
                if not target:
                    continue  # dangling redirect — skip the useless circular entry
#Show the canonical spelling note + the real definition.
                definition = f"= {redirect}\n{target[0]}"
                priority = target[1]
            else:
                anki = furigana.get((headword, reading), "") if furigana else ""
                definition = format_definition_yomitan(headword, reading, definitions, anki)
                if anki and reading != headword:
                    furigana_count += 1
                if not definition:
                    continue
                if show_tags and tags.strip():
                    definition = _with_tag_line(definition, tags)
                priority = max(0, min(255, int(score) + 128)) if isinstance(score, (int, float)) else 100

            def_bytes = _utf8(definition)
#Yomitan spec : empty rules = "word is not inflected" --that IS positive POS data
#(a non - conjugating word), so stamp POS_OTHER rather than the fail - open 0.
            pos_flags = pos_flags_from_tags(rules.split()) if rules.strip() else POS_OTHER
            seen_headwords = set()
            hw_bytes = _utf8(headword)
            if hw_bytes and len(hw_bytes) < HEADWORD_SIZE:
                seen_headwords.add(hw_bytes)
                records.append((hw_bytes, def_bytes, priority, pos_flags))

            if reading_records and reading and reading != headword and not redirect:
                r_bytes = _utf8(reading)
                if len(r_bytes) < HEADWORD_SIZE and r_bytes not in seen_headwords:
                    r_def = format_definition_yomitan(reading, reading, definitions)
                    if r_def:
#reading != headword means kana reading of a kanji headword : flag it
#(see POS_READING above).
                        records.append((r_bytes, _utf8(r_def), priority,
                                        pos_flags | POS_READING))

            entry_count += 1

    _IMAGE_ZIP = None
    if not records:
        print("ERROR: No entry has a definition the reader can show (web links only?).", file=sys.stderr)
        sys.exit(1)
    print(f"Processed {entry_count} Yomitan entries → {len(records)} index records")
    if furigana is not None:
        print(f"Furigana attached to {furigana_count} headwords")
    return records, title

# Stats worth a reader's glance; KANJIDIC carries some 40 textbook index numbers besides these.
_KANJI_STATS = (("strokes", "Strokes"), ("grade", "Grade"), ("jlpt", "JLPT"), ("freq", "Freq"),
                ("総画", "総画"), ("画数", "画数"), ("部首", "部首"), ("旧字体", "旧字体"), ("漢字検定", "漢字検定"))
# Wiktionary section rules: ＝＝＝発音＝＝＝, ーーー字源ーーー.
_KANJI_HEADING_RE = re.compile(r"^[＝=ー\-]{2,}\s*(.*?)\s*[＝=ー\-]{2,}$")


def _kanji_noise(line: str, character: str) -> bool:
    """Lines that say nothing on the device: the character alone, a bare rule, a web address, or
    Wiktionary's "見：附录:…" pointer to another wiki page."""
    bare = line.strip()
    return (bare == character or re.fullmatch(r"[＝=ー\-]+", bare) is not None or bare.startswith(("http://", "https://"))
            or re.match(r"見[：:]\s*附[录錄]", bare) is not None)


def format_kanji_entry(onyomi: str, kunyomi: str, tags: str, meanings, stats, character: str = "") -> str:
    """One kanji_bank entry as the device shows it: reading lines, the meanings, and a short line
    of stats. Short glosses (KANJIDIC's "study", "learning") share one line; longer meaning lines
    (Wiktionary, TheKanjiMap) keep theirs, and their section rules become "■ heading" lines."""
    lines = []
    # The readings go on the device's reading line (【…】, shown under the character): on-readings,
    # then kun-readings with KANJIDIC's okurigana dot as parentheses (たか.い -> たか(い)).
    kun = [re.sub(r"\.(.+)$", r"(\1)", k) for k in kunyomi.split()]
    # Space-separated, so a long list wraps between readings rather than before a comma.
    readings = [r for r in (" ".join(onyomi.split()), " ".join(kun)) if r]
    if readings:
        lines.append("【" + "　".join(readings) + "】")
    items = [m if isinstance(m, str) else flatten_structured_content(m) for m in (meanings or [])]
    items = [m.strip().replace("【", "〖").replace("】", "〗") for m in items]
    items = [m for m in items if not (m and _kanji_noise(m, character))]
    shown = [m for m in items if m]
    if shown and all(len(m) <= 30 and "\n" not in m and not _KANJI_HEADING_RE.match(m) for m in shown) and \
            all(m.isascii() for m in shown):
        lines.append("• " + "; ".join(shown))
    else:
        # Meaning lines stay as written (the device keeps the lines of an entry without bullets),
        # and the Wiktionary section rules become "→ " notes above their lines.
        body = []
        # Empty meanings split groups whose first line names them (mozc: 旧字体, 俗字).
        grouped = "" in items
        group_start = grouped
        for i, m in enumerate(items):
            heading = _KANJI_HEADING_RE.match(m)
            if grouped and not m:
                group_start = True
                continue
            after = items[i + 1] if i + 1 < len(items) else ""
            if group_start and m and len(m) <= 8 and len(after) > 8:
                body.append("■ " + m)
                group_start = False
                continue
            group_start = False
            if heading:
                # ＝ rules head a language, ー rules a section within it.
                body.append(("■■ " if m.lstrip().startswith(("＝", "=")) else "■ ") + heading.group(1))
            elif m and body and re.search(r"[:：]\s*$", body[-1]) and not body[-1].startswith("■ "):
                body[-1] = body[-1].rstrip() + " " + m  # a label line ("漢字分解:") takes the line after it
            elif m:
                body.append(m)
        for line in _join_labels(_join_word_runs(_drop_empty_headings(body))):
            if line.startswith("■ ") and lines and lines[-1].startswith("→ "):
                lines[-1] += " · " + line[2:]  # a language heading and its first section: 日語 · 讀音
                continue
            if line.startswith("■ "):
                lines.append("")
                lines.append("→ " + line[2:])
            else:
                lines.append(line)
    extra = []
    if isinstance(stats, dict):
        for key, label in _KANJI_STATS:
            value = stats.get(key)
            if value not in (None, ""):
                extra.append(f"{label} {value}")
    extra.extend(t for t in tags.split() if t)
    if extra:
        lines.append("")
        lines.append(" · ".join(extra))
    text = "\n".join(lines)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def _drop_empty_headings(lines: list) -> list:
    """A heading with no text under it before the next heading of its level or higher (Wiktionary's
    empty 詞源1). "■■ " is a language heading, "■ " a section heading. The kept ones lose a level."""
    out = []
    for i, line in enumerate(lines):
        level = 2 if line.startswith("■■ ") else 1 if line.startswith("■ ") else 0
        if level:
            has_text = False
            for x in lines[i + 1:]:
                if not x:
                    continue
                other = 2 if x.startswith("■■ ") else 1 if x.startswith("■ ") else 0
                if other >= level:
                    break
                if not other:
                    has_text = True
                    break
            if not has_text:
                continue
            line = "■ " + line[3:] if level == 2 else line
        out.append(line)
    return out


def _join_word_runs(lines: list) -> list:
    """Three or more short word lines in a row (JPDB's example words) share one line: 学校、学園、大学."""
    out = []
    run = []

    def flush():
        if len(run) >= 3:
            out.append("、".join(run))
        else:
            out.extend(run)
        run.clear()

    for line in lines:
        if line and len(line) <= 6 and not re.search(r"[\s:：、。,（(]", line):
            run.append(line)
        else:
            flush()
            out.append(line)
    flush()
    return out


def _join_labels(lines: list) -> list:
    """A short label line (Wiktionary's 衍生字, 組詞) joins the line after it: 衍生字：敩, 𢽾."""
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        nxt = lines[i + 1] if i + 1 < len(lines) else ""
        if (line and not line.startswith("■ ") and len(line) <= 4 and not re.search(r"[\s\W]", line) and nxt and
                not nxt.startswith("■ ")):
            out.append(line + "：" + nxt)
            i += 2
            continue
        out.append(line)
        i += 1
    return out


def _utf8(text: str) -> bytes:
    """UTF-8 without the lone surrogates some dictionaries' JSON escapes carry (TheKanjiMap)."""
    return text.encode("utf-8", "ignore")


def _is_han(ch: str) -> bool:
    cp = ord(ch)
    return 0x3400 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF or 0x20000 <= cp <= 0x3FFFF


def convert_yomitan_kanji(zip_path: str) -> tuple:
    """A Yomitan kanji dictionary (kanji_bank_N.json) to one record per character, for the kanji
    slot. Returns (records, title)."""
    import zipfile

    print(f"Loading {zip_path}...")
    title = ""
    records = []
    with zipfile.ZipFile(zip_path, "r") as z:
        names = z.namelist()
        if "index.json" in names:
            with _zip_open(z, "index.json") as f:
                title = str(json.load(f).get("title", ""))
        banks = sorted(n for n in names if re.match(r"kanji_bank_\d+\.json$", n))
        merged = {}  # a character in several entries (KANJIDIC's variants) shows them together
        if not banks:
            # Some kanji dictionaries (漢字源, TISMKANJI) are term banks keyed by one character.
            for bank in sorted(n for n in names if re.match(r"term_bank_\d+\.json$", n)):
                with _zip_open(z, bank) as f:
                    for entry in json.load(f):
                        if not isinstance(entry, list) or len(entry) < 6 or not isinstance(entry[0], str):
                            continue
                        char = entry[0]
                        if len(char) != 1 or not _is_han(char):
                            continue
                        text = format_definition_yomitan(char, str(entry[1] or ""), entry[5])
                        if text and re.search(r"\w", text):
                            merged.setdefault(char, [])
                            if text not in merged[char]:
                                merged[char].append(text)
            if not merged and any(re.match(r"kanji_meta_bank_\d+\.json$", n) for n in names):
                print(f"ERROR: {zip_path} is a frequency list, not a kanji dictionary. Pass it with "
                      "--frequency instead.", file=sys.stderr)
                sys.exit(1)
            if not merged:
                print("ERROR: No kanji_bank_N.json files found in zip, and no term entry is a single "
                      "character", file=sys.stderr)
                sys.exit(1)
        for bank in banks:
            with _zip_open(z, bank) as f:
                for entry in json.load(f):
                    if not isinstance(entry, list) or len(entry) < 5 or not isinstance(entry[0], str):
                        continue
                    char = entry[0]
                    stats = entry[5] if len(entry) > 5 else {}
                    text = format_kanji_entry(str(entry[1] or ""), str(entry[2] or ""), str(entry[3] or ""),
                                              entry[4], stats, char)
                    if text and re.search(r"\w", text):
                        merged.setdefault(char, [])
                        if text not in merged[char]:
                            merged[char].append(text)
    for char, texts in merged.items():
        hw = _utf8(char)
        if hw and len(hw) < HEADWORD_SIZE:
            records.append((hw, _utf8("\n\n".join(texts)), 100, POS_OTHER))
    if not records:
        print("ERROR: No kanji entry has text the reader can show.", file=sys.stderr)
        sys.exit(1)
    print(f"Processed {len(records)} kanji entries")
    return records, title


# ── MDict(.mdx) ────────────────────────────────────────────────


def convert_mdict(mdx_path: str) -> list:
    """Convert an MDict .mdx file to index records.

    Requires: pip install readmdict
    Optional: pip install python-lzo  (for LZO-compressed dictionaries)
    """
    try:
        from readmdict import MDX
    except ImportError:
        print(
            "ERROR: readmdict is required for MDict conversion.\n"
            "Install it with: pip install readmdict\n"
            "For LZO support: pip install python-lzo",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Loading {mdx_path}...")
    mdx = MDX(mdx_path)

    records = []
    entry_count = 0
    skipped = 0

    for key_bytes, val_bytes in mdx.items():
        headword = key_bytes.decode("utf-8", errors="replace").strip()
        raw_def = val_bytes.decode("utf-8", errors="replace").strip()

        if not headword or not raw_def:
            skipped += 1
            continue

        if raw_def.startswith("@@@LINK="):
            skipped += 1
            continue

        definition = strip_html(raw_def)
        if not definition:
            skipped += 1
            continue

        hw_bytes = headword.encode("utf-8")
        if len(hw_bytes) >= HEADWORD_SIZE:
            skipped += 1
            continue

        def_bytes = definition.encode("utf-8")
        records.append((hw_bytes, def_bytes, 100, 0))
        entry_count += 1

    print(f"Processed {entry_count} MDict entries ({skipped} skipped) → {len(records)} index records")
    return records

# ── Chinese : pinyin and zhuyin ────────────────────────────────

_TONE_MARKS = {
    "a": "āáǎà", "e": "ēéěè", "i": "īíǐì", "o": "ōóǒò", "u": "ūúǔù", "ü": "ǖǘǚǜ",
    "A": "ĀÁǍÀ", "E": "ĒÉĚÈ", "I": "ĪÍǏÌ", "O": "ŌÓǑÒ", "U": "ŪÚǓÙ", "Ü": "ǕǗǙǛ",
}
_SYLLABLE_RE = re.compile(r"^([A-Za-zü:]+?)([1-5])$")


def pinyin_syllable_to_marks(syllable: str) -> str:
    """'ni3' -> 'nǐ', 'lu:4' -> 'lǜ', 'ma5' -> 'ma'. Anything else is returned unchanged."""
    m = _SYLLABLE_RE.match(syllable)
    if not m:
        return syllable
    base = m.group(1).replace("u:", "ü").replace("U:", "Ü").replace("v", "ü").replace("V", "Ü")
    tone = int(m.group(2))
    if tone == 5:
        return base
    lower = base.lower()
    if "a" in lower:
        idx = lower.index("a")
    elif "e" in lower:
        idx = lower.index("e")
    elif "ou" in lower:
        idx = lower.index("ou")
    else:
        idx = -1
        for i, ch in enumerate(lower):
            if ch in "aeiouü":
                idx = i  # the LAST vowel takes the mark (iu -> iù, ui -> uì)
        if idx < 0:
            return base  # m2, ng2, hm5: no vowel to mark
    marked = _TONE_MARKS[base[idx]][tone - 1]
    return base[:idx] + marked + base[idx + 1:]


_NUMBERED_SYLLABLE_RE = re.compile(r"[A-Za-zü:]+[1-5]")


def pinyin_to_marks(numbered: str) -> str:
    """Convert a numbered-tone string ('ni3 hao3') to diacritics. An erhua r5 joins the syllable
    before it (hua1 r5 -> huār); separators other than spaces (commas, ·) are kept."""
    marked = _NUMBERED_SYLLABLE_RE.sub(lambda m: pinyin_syllable_to_marks(m.group(0)), numbered)
    return re.sub(r"(?<=\S) r(?=$|[\s,·])", "r", marked)


_ZHUYIN_INITIALS = [
    ("zh", "ㄓ"), ("ch", "ㄔ"), ("sh", "ㄕ"), ("b", "ㄅ"), ("p", "ㄆ"), ("m", "ㄇ"), ("f", "ㄈ"),
    ("d", "ㄉ"), ("t", "ㄊ"), ("n", "ㄋ"), ("l", "ㄌ"), ("g", "ㄍ"), ("k", "ㄎ"), ("h", "ㄏ"),
    ("j", "ㄐ"), ("q", "ㄑ"), ("x", "ㄒ"), ("r", "ㄖ"), ("z", "ㄗ"), ("c", "ㄘ"), ("s", "ㄙ"),
]
_ZHUYIN_FINALS = {
    "a": "ㄚ", "o": "ㄛ", "e": "ㄜ", "ê": "ㄝ", "ai": "ㄞ", "ei": "ㄟ", "ao": "ㄠ", "ou": "ㄡ",
    "an": "ㄢ", "en": "ㄣ", "ang": "ㄤ", "eng": "ㄥ", "er": "ㄦ", "i": "ㄧ", "ia": "ㄧㄚ",
    "io": "ㄧㄛ", "ie": "ㄧㄝ", "iai": "ㄧㄞ", "iao": "ㄧㄠ", "iu": "ㄧㄡ", "ian": "ㄧㄢ",
    "in": "ㄧㄣ", "iang": "ㄧㄤ", "ing": "ㄧㄥ", "iong": "ㄩㄥ", "u": "ㄨ", "ua": "ㄨㄚ",
    "uo": "ㄨㄛ", "uai": "ㄨㄞ", "ui": "ㄨㄟ", "uan": "ㄨㄢ", "un": "ㄨㄣ", "uang": "ㄨㄤ",
    "ueng": "ㄨㄥ", "ong": "ㄨㄥ", "ü": "ㄩ", "üe": "ㄩㄝ", "üan": "ㄩㄢ", "ün": "ㄩㄣ",
}
# Syllables written with y/w carry the medial in the spelling, not in a separate initial.
_ZHUYIN_WHOLE = {
    "zhi": "ㄓ", "chi": "ㄔ", "shi": "ㄕ", "ri": "ㄖ", "zi": "ㄗ", "ci": "ㄘ", "si": "ㄙ",
    "yi": "ㄧ", "ya": "ㄧㄚ", "yo": "ㄧㄛ", "ye": "ㄧㄝ", "yai": "ㄧㄞ", "yao": "ㄧㄠ", "you": "ㄧㄡ",
    "yan": "ㄧㄢ", "yin": "ㄧㄣ", "yang": "ㄧㄤ", "ying": "ㄧㄥ", "yong": "ㄩㄥ",
    "wu": "ㄨ", "wa": "ㄨㄚ", "wo": "ㄨㄛ", "wai": "ㄨㄞ", "wei": "ㄨㄟ", "wan": "ㄨㄢ", "wen": "ㄨㄣ",
    "wang": "ㄨㄤ", "weng": "ㄨㄥ",
    "yu": "ㄩ", "yue": "ㄩㄝ", "yuan": "ㄩㄢ", "yun": "ㄩㄣ",
    "r": "ㄦ", "m": "ㄇ", "n": "ㄋ", "ng": "ㄫ", "hm": "ㄏㄇ", "hng": "ㄏㄫ",
}
_ZHUYIN_TONES = {1: "", 2: "ˊ", 3: "ˇ", 4: "ˋ"}


def pinyin_syllable_to_zhuyin(syllable: str) -> str:
    """'ni3' -> 'ㄋㄧˇ', 'lu:4' -> 'ㄌㄩˋ', 'ma5' -> '˙ㄇㄚ'. Unknown syllables come back unchanged."""
    m = _SYLLABLE_RE.match(syllable)
    if not m:
        return syllable
    base = m.group(1).lower().replace("u:", "ü").replace("v", "ü")
    tone = int(m.group(2))
    body = _ZHUYIN_WHOLE.get(base)
    if body is None:
        initial = ""
        rest = base
        for latin, bopomofo in _ZHUYIN_INITIALS:
            if base.startswith(latin):
                initial, rest = bopomofo, base[len(latin):]
                break
        # After j/q/x (and y, handled above) a written u is ü.
        if initial in ("ㄐ", "ㄑ", "ㄒ") and rest.startswith("u"):
            rest = "ü" + rest[1:]
        final = _ZHUYIN_FINALS.get(rest)
        if final is None or (not initial and rest != base):
            return syllable
        body = initial + final
    if tone == 5:
        # Erhua ㄦ is written without the neutral-tone dot (哪兒 ㄋㄚˇ ㄦ).
        return body if base == "r" else "˙" + body
    return body + _ZHUYIN_TONES[tone]


def pinyin_to_zhuyin(numbered: str) -> str:
    return " ".join(pinyin_syllable_to_zhuyin(part) for part in numbered.split(" "))


_BRACKETED_PINYIN_RE = re.compile(r"\[([A-Za-z0-9:\u00fc\u00dc ,\u00b7]+)\]")


# A word given in both scripts inside a gloss: 個|个, 237號房間|237号房间, 對…|对…. Not every bar:
# only one that touches a CJK character.
_CJK_CLASS = "[\u3400-\u9fff\uf900-\ufaff]"
_SCRIPT_PAIR_BAR_RE = re.compile(rf"(?<={_CJK_CLASS})\||\|(?={_CJK_CLASS})")
_CLASSIFIER_RE = re.compile(r"\bCL:(?=\S)")


def prettify_cedict_gloss(gloss: str) -> str:
    """CEDICT glosses carry their own markup for cross-references: both scripts joined by a bar
    and numbered pinyin in brackets (CL:個|个[ge4], see 你好[ni3 hao3]). Written out for a reader:
    "CL: 個/个 (gè)", "see 你好 (nǐ hǎo)"."""
    gloss = _BRACKETED_PINYIN_RE.sub(lambda m: " (" + pinyin_to_marks(m.group(1)) + ")", gloss)
    gloss = _SCRIPT_PAIR_BAR_RE.sub("/", gloss)
    return _CLASSIFIER_RE.sub("CL: ", gloss)


# ── Chinese : frequency ranking ────────────────────────────────

_HAN_RE = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff\U00020000-\U0003134f]")
# A graded list's level: 1-9, or a band such as 7-9.
_LEVEL_RE = re.compile(r"[1-9](?:-[1-9])?")
UNRANKED_PRIORITY = 60  # entries absent from the frequency list


def _split_variants(cell: str) -> list:
    """The Han words in one list cell: HSK and TOCFL write variants as 爸爸|爸 and 你/妳."""
    return [w for w in re.split(r"[/|｜、]", cell) if _HAN_RE.search(w)]


def rank_to_priority(rank: int) -> int:
    """1 -> 255, 10 -> 227, 1000 -> 171, 100000 -> 115: log-scaled so the common words spread."""
    return max(UNRANKED_PRIORITY + 1, min(255, 255 - int(round(28 * math.log10(rank)))))


def load_yomitan_frequency(path: str) -> dict:
    """A Yomitan frequency dictionary (term_meta_bank / kanji_meta_bank "freq" rows) to
    {word: priority}. Its values are ranks: smaller is more common."""
    import zipfile

    rows = {}
    with zipfile.ZipFile(path, "r") as z:
        for name in z.namelist():
            if not re.match(r"(term|kanji)_meta_bank_\d+\.json$", name):
                continue
            with _zip_open(z, name) as f:
                for row in json.load(f):
                    if not isinstance(row, list) or len(row) < 3 or row[1] != "freq":
                        continue
                    value = row[2]
                    if isinstance(value, dict):
                        value = value.get("frequency", value.get("value"))
                        if isinstance(value, dict):
                            value = value.get("value")
                    if isinstance(value, str):
                        # Some lists write the rank as text: "1", "1/10077", "12,345".
                        m = re.match(r"\s*([\d,]+)", value)
                        value = int(m.group(1).replace(",", "")) if m else None
                    if isinstance(value, (int, float)) and isinstance(row[0], str):
                        rows[row[0]] = min(value, rows.get(row[0], value))
    if not rows:
        print(f"WARNING: no frequency rows found in {path}", file=sys.stderr)
        return {}
    ranked = sorted(rows.items(), key=lambda r: r[1])
    print(f"Frequency list {path}: {len(ranked)} words, ranked by its values")
    return {word: rank_to_priority(rank) for rank, (word, _) in enumerate(ranked, start=1)}


def load_yomitan_pitch(path: str) -> dict:
    """A Yomitan pitch dictionary (term_meta_bank "pitch" rows, such as NHK 2016) to
    {(word, reading): [downstep positions]}, with (word, "") also holding the word's first one."""
    import zipfile

    pitch = {}
    with zipfile.ZipFile(path, "r") as z:
        for name in z.namelist():
            if not re.match(r"term_meta_bank_\d+\.json$", name):
                continue
            with _zip_open(z, name) as f:
                for row in json.load(f):
                    if not isinstance(row, list) or len(row) < 3 or row[1] != "pitch" or not isinstance(row[2], dict):
                        continue
                    reading = row[2].get("reading", "")
                    positions = [p.get("position") for p in row[2].get("pitches", []) if isinstance(p, dict)]
                    positions = [p for p in positions if isinstance(p, int)]
                    if positions and isinstance(row[0], str):
                        for key in ((row[0], reading), (row[0], "")):
                            known = pitch.setdefault(key, [])
                            known.extend(p for p in positions if p not in known)
    print(f"Pitch accents {path}: {sum(1 for k in pitch if k[1])} readings")
    return pitch


_READING_LINE_RE = re.compile(r"^【([^】]*)】(｛[^｝]*｝)?")


def apply_pitch(records: list, pitch: dict) -> list:
    """Add `｟n｠` after each entry's reading line, from the first matching pitch entry: the device
    draws a line over the high morae. A kana-only headword (no reading line) gets one too."""
    if not pitch:
        return records
    out = []
    count = 0
    for hw, d, p, pos in records:
        text = d.decode("utf-8")
        word = hw.decode("utf-8")
        source = ""
        if text.startswith(SOURCE_MARKER):
            cut = text.find("\n") + 1
            source, text = text[:cut], text[cut:]
        m = _READING_LINE_RE.match(text)
        reading = m.group(1) if m else word
        positions = pitch.get((word, reading)) or (pitch.get((word, "")) if not m else None)
        if positions:
            mark = "｟" + str(positions[0]) + "｠"
            text = text[:m.end()] + mark + text[m.end():] if m else "【" + word + "】" + mark + "\n" + text
            count += 1
        out.append((hw, _utf8(source + text), p, pos))
    print(f"Pitch accent added to {count} entries")
    return out


def load_frequency(path: str, kind: str = "auto") -> dict:
    """Read a word list into {word: priority}.

    Accepts jieba dict.txt ("word count pos"), BCC/SUBTLEX exports ("word<TAB>count"), and
    graded lists such as HSK or TOCFL CSVs where the order of the rows IS the ranking. The word
    is the first Han field of each row; a count is the first number after it. kind=auto ranks by
    count when most rows carry one, else by row order; count/rank force either.
    """
    if path.endswith(".zip"):
        return load_yomitan_frequency(path)
    rows = []
    graded = False  # a level band (7-9) seen: the numbers in this file are levels, not counts
    opener = __import__("gzip").open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip().lstrip("\ufeff")
            if not line or line.startswith("#"):
                continue
            fields = [x.strip().strip('"') for x in re.split(r"[\t,]|\s+", line) if x.strip()]
            wordAt = next((i for i, x in enumerate(fields) if _HAN_RE.search(x)), None)
            if wordAt is None:
                continue
            count = None
            for x in fields[wordAt + 1:]:
                if re.fullmatch(r"[1-9]-[1-9]", x):
                    graded = True  # HSK's 7-9 band: this column is a level, not a count
                    break
                try:
                    count = float(x)  # a plain small number may still be a count (jieba: 說 3)
                    break
                except ValueError:
                    continue
            # 你/妳, 爸爸|爸: every variant in the cell is the same word.
            for word in _split_variants(fields[wordAt]):
                rows.append((word, count))
    if not rows:
        print(f"WARNING: no words found in frequency list {path}", file=sys.stderr)
        return {}
    with_count = sum(1 for _, c in rows if c is not None)
    # A graded list's level column is numeric too (HSK 1-9, TOCFL 1-7), but it is not a count:
    # auto treats small numbers as levels and keeps the file's own order.
    largest = max((c for _, c in rows if c is not None), default=0.0)
    use_count = kind == "count" or (kind == "auto" and not graded and with_count >= 0.8 * len(rows) and largest > 100)
    if use_count:
        rows.sort(key=lambda r: -(r[1] or 0.0))
    priorities = {}
    for rank, (word, _) in enumerate(rows, start=1):
        p = rank_to_priority(rank)
        if p > priorities.get(word, 0):
            priorities[word] = p
    print(f"Frequency list {path}: {len(priorities):,} words, ranked by {'count' if use_count else 'row order'}")
    return priorities


def shared_simplified_counts(trads_of: dict) -> dict:
    """{traditional form: how many traditional forms share its simplified form} from
    {simplified: {traditional, ...}}, for apply_frequency. 干 stands for 乾, 幹 and 干 itself: 3."""
    shares = {}
    for trads in trads_of.values():
        if len(trads) > 1:
            for trad in trads:
                shares[trad] = max(shares.get(trad, 1), len(trads))
    return shares


def apply_frequency(records: list, priorities: dict, twins: dict = None, shares: dict = None) -> list:
    """Replace each record's priority with its frequency rank, taken from either script's form of
    the word (twins, from CC-CEDICT): the lists are written in one script, and the device segments
    a traditional book with the traditional records."""
    if not priorities:
        return records
    twins = twins or {}
    shares = shares or {}
    out = []
    for hw, d, _, pos in records:
        word = hw.decode("utf-8")
        # The larger of the two forms' ranks: jieba's list is simplified but carries a few stray
        # traditional characters with tiny counts (說 3, 這 7), which must not outrank 说 and 这.
        # A simplified form that stands for several traditional ones (干: 乾, 幹, 干) has its
        # count split between them, as if each were that many times rarer.
        borrowed = priorities.get(twins.get(word, ""), 0)
        if borrowed and shares.get(word, 1) > 1:
            borrowed = max(UNRANKED_PRIORITY + 1, borrowed - int(round(28 * math.log10(shares[word]))))
        p = max(priorities.get(word, 0), borrowed) or UNRANKED_PRIORITY
        out.append((hw, d, p, pos))
    return out


# ── CC - CEDICT(.u8 / .txt) ────────────────────────────────────

# CC-CEDICT, and CC-Canto's extension of it with a {jyutping} field after the pinyin. CC-Canto
# ends a third of its lines with a comment ("/.../ # adapted from cc-cedict").
_CEDICT_LINE_RE = re.compile(r"^(\S+)\s+(\S+)\s+\[([^\]]*)\](?:\s+\{([^}]*)\})?\s+/(.*)/\s*(?:#.*)?$")
# cccedict-canto-readings: "繁 简 [pin1 yin1] {jyut6 ping3}" with no glosses.
_CANTO_READING_RE = re.compile(r"^(\S+)\s+(\S+)\s+\[([^\]]*)\]\s+\{([^}]*)\}")


def format_definition_cedict(trad: str, simp: str, pinyin: str, glosses: list, zhuyin: bool,
                             jyutping: str = "", level: str = "", examples: list = None) -> str:
    reading = pinyin_to_marks(pinyin)
    if zhuyin:
        reading += " " + pinyin_to_zhuyin(pinyin)
    if jyutping:
        reading += " · " + jyutping
    # The device's entry renderer speaks Jitendex's layout: "• gloss" bullets, one sense per
    # blank-line-separated group, "→ note" lines, and any other line an example sentence. An
    # entry without bullets is taken for a names dictionary and gets every line numbered.
    parts = ["【" + reading + "】"]
    if level:
        parts.append(f"[{level}]")  # a tag line: the device shows it on the entry's grammar line
    if trad != simp:
        parts.append(f"→ {trad} / {simp}")
    glosses = [prettify_cedict_gloss(g) for g in glosses if g][:12]
    parts.extend(f"• {g}\n" for g in glosses)
    # Example sentences, each with its translation on the next line.
    for sentence, translation in (examples or []):
        parts.append(sentence)
        if translation:
            parts.append(translation)
    return "\n".join(parts)


# ── Example sentences(Tatoeba) ─────────────────────────────────

EXAMPLES_PER_ENTRY = 2
EXAMPLE_MAX_CHARS = 40

# The commonest characters that exist in only one script, in matching pairs (mirrors the
# firmware's content sniff). Tatoeba mixes both; a simplified pack should show simplified
# sentences, and the other way round.
_SIMPLIFIED_ONLY = set("这说们个么时国来对会发为还没过样开学现后点见问东门车书长几应两认让经关实话听从头尔业爱图电机体试写读马鸟龙叶万与")
_TRADITIONAL_ONLY = set("這說們個麼時國來對會發為還沒過樣開學現後點見問東門車書長幾應兩認讓經關實話聽從頭爾業愛圖電機體試寫讀馬鳥龍葉萬與")


def sentence_script(sentence: str) -> str:
    """'simplified', 'traditional', or 'any' when nothing in the sentence tells them apart."""
    simp = sum(1 for ch in sentence if ch in _SIMPLIFIED_ONLY)
    trad = sum(1 for ch in sentence if ch in _TRADITIONAL_ONLY)
    if simp > trad:
        return "simplified"
    if trad > simp:
        return "traditional"
    return "any"


_UNRENDERABLE_EXAMPLE_RE = re.compile(r"Tatoeba|JMdict|JMnedict|【|^\d+\. ")


def load_sentence_pairs(path: str, script: str = "any") -> list:
    """Tatoeba 'sentence pairs' export (id, sentence, id, translation) or a plain two-column
    sentence<TAB>translation file. Returns [(sentence, translation)] with long sentences dropped:
    a short example shows the word in use; a long one only costs space on the card. script keeps
    only sentences written in that script (or in neither distinguishably)."""
    pairs = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            fields = line.rstrip("\n").split("\t")
            if len(fields) >= 4:
                sentence, translation = fields[1].strip(), fields[3].strip()
            elif len(fields) >= 2:
                sentence, translation = fields[0].strip(), fields[1].strip()
            else:
                continue
            if not sentence or len(sentence) > EXAMPLE_MAX_CHARS or not _HAN_RE.search(sentence):
                continue
            if script != "any" and sentence_script(sentence) not in ("any", script):
                continue
            # The device's entry renderer treats a line that mentions a source, holds a 【, or
            # starts like "3. " as chrome or a sense number, so such a pair would come out mangled.
            if any(_UNRENDERABLE_EXAMPLE_RE.search(t) for t in (sentence, translation)):
                continue
            pairs.append((sentence, translation))
    pairs.sort(key=lambda p: len(p[0]))  # shortest first, so the cap keeps the clearest ones
    print(f"Sentence pairs {path}: {len(pairs):,} usable")
    return pairs


def attach_examples(pairs: list, forms: dict, entry_count: int) -> list:
    """Segment every sentence against the dictionary's headwords (longest match, like the device
    does) and hand it to the entries of the words it contains, up to EXAMPLES_PER_ENTRY each.
    forms maps each headword form to its entry index. Single-character words get none: the
    particles would collect thousands and no reader needs an example of 的."""
    examples = [[] for _ in range(entry_count)]
    if not pairs or not forms:
        return examples
    max_len = max(len(w) for w in forms)
    for sentence, translation in pairs:
        i = 0
        n = len(sentence)
        seen = set()
        while i < n:
            matched = 0
            for length in range(min(max_len, n - i), 1, -1):
                idx = forms.get(sentence[i:i + length])
                if idx is not None:
                    matched = length
                    if idx not in seen and len(examples[idx]) < EXAMPLES_PER_ENTRY:
                        examples[idx].append((sentence, translation))
                        seen.add(idx)
                    break
            i += matched or 1
    print(f"Examples attached to {sum(1 for e in examples if e):,} entries")
    return examples


# Capitalised in CC-CEDICT but everyday vocabulary, not names: languages, nationalities, days,
# festivals, religions and institutions a learner meets in any text.
_COMMON_NOUN_GLOSS_RE = re.compile(
    r"\b(language|people|person|ethnic|nationality|citizen|day|\w+day|week|month|festival|holiday|new year|"
    r"religion|church|bible|god|party|army|navy|games|cup|era|calendar|zodiac|internet|christianity|"
    r"january|february|march|april|may|june|july|august|september|october|november|december|"
    r"american|british|chinese|japanese|korean|english|french|german|russian|spanish|italian|indian|"
    r"asian|european|african|western)\b|ism\b|ist\b",
    re.IGNORECASE)


def is_proper_noun_pinyin(pinyin: str, glosses: list = None) -> bool:
    """CC-CEDICT capitalises the pinyin of proper nouns (Zhong1 guo2, Bei3 jing1). Entries whose
    glosses read like common nouns (汉语, 中国人, 星期天, 春节) stay in the vocabulary."""
    syllables = [p for p in pinyin.split(" ") if p and p[0].isalpha()]
    if not syllables or not syllables[0][0].isupper():
        return False
    # "People's Republic of China" in a place's gloss is not the common noun "people".
    first = re.sub(r"people['\u2019]s", "", (glosses or [""])[0], flags=re.IGNORECASE)
    return not _COMMON_NOUN_GLOSS_RE.search(first)


def load_canto_readings(path: str) -> dict:
    """{(trad, simp, pinyin): jyutping} from a cccedict-canto-readings file."""
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            m = _CANTO_READING_RE.match(line.rstrip("\n"))
            if m:
                out[(m.group(1), m.group(2), m.group(3))] = m.group(4).strip()
    print(f"Jyutping readings {path}: {len(out):,} entries")
    return out


def split_cedict_glosses(body: str) -> list:
    """Split an entry's "/gloss/gloss/" body into glosses. A slash inside parentheses belongs to
    the gloss: CC-Canto writes "(phrase / adverb / noun) no, not." as one."""
    glosses = []
    depth = 0
    current = []
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")" and depth > 0:
            depth -= 1
        if ch == "/" and depth == 0:
            glosses.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    glosses.append("".join(current).strip())
    return glosses


def convert_cedict(path: str, zhuyin: bool = False, split_names: bool = False, levels: dict = None,
                   jyutping: dict = None, sentence_pairs: list = None, twins: dict = None,
                   examples_script: str = "any", trads_of: dict = None, seen: dict = None) -> tuple:
    """Convert a raw CC-CEDICT (or CC-Canto) file to index records, one per traditional and
    simplified form. Returns (vocab_records, name_records); the second list is empty unless
    split_names routes proper nouns (capitalised pinyin) into the names slot. twins, when given,
    is filled with each form's other-script form, so apply_frequency can rank a traditional
    record from a simplified word list and the reverse. trads_of, shared across the inputs of one
    build, collects each simplified form's traditional forms (see shared_simplified_counts). seen,
    shared likewise, drops an entry an earlier input already held (CC-Canto repeats CC-CEDICT
    entries it adapted) unless only this copy carries a Cantonese reading.
    """
    print(f"Loading {path}...")
    levels = levels or {}
    jyutping = jyutping or {}
    parsed = []  # (trad, simp, pinyin, canto, glosses)
    forms = {}
    if trads_of is None:
        trads_of = {}
    skipped = 0
    opener = __import__("gzip").open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line or line.startswith("#"):
                continue
            m = _CEDICT_LINE_RE.match(line.rstrip("\n"))
            if not m:
                skipped += 1
                continue
            trad, simp, pinyin, canto, body = m.groups()
            glosses = split_cedict_glosses(body)
            trads_of.setdefault(simp, set()).add(trad)
            if seen is not None:
                key = (trad, simp, pinyin.lower(), tuple(glosses))
                has_canto = bool(canto or jyutping.get((trad, simp, pinyin)))
                if key in seen and (seen[key] or not has_canto):
                    skipped += 1
                    continue
                seen[key] = has_canto
            idx = len(parsed)
            parsed.append((trad, simp, pinyin, canto, glosses))
            # Examples go to the everyday entry of a form: 周 the week over the surname, and among
            # lowercase readings the one with more senses (東西 "thing" over "east and west").
            glosses = parsed[idx][4]
            proper = is_proper_noun_pinyin(pinyin, glosses)
            for form in (trad, simp):
                held = forms.get(form)
                if held is None:
                    forms[form] = idx
                    continue
                heldProper = is_proper_noun_pinyin(parsed[held][2], parsed[held][4])
                if not proper and (heldProper or len(glosses) > len(parsed[held][4])):
                    forms[form] = idx
            if twins is not None and trad != simp:
                twins.setdefault(trad, simp)
                twins.setdefault(simp, trad)
    if sentence_pairs and examples_script in ("simplified", "traditional"):
        # The characters CC-CEDICT itself uses in only one script decide a sentence's script far
        # more reliably than the short list the firmware's book sniff uses: a 10-character
        # sentence often holds none of those, and a fifth of Tatoeba is the other script.
        tradChars = {ch for t, sm, *_ in parsed if t != sm for ch in t}
        simpChars = {ch for t, sm, *_ in parsed if t != sm for ch in sm}
        foreign = (tradChars - simpChars) if examples_script == "simplified" else (simpChars - tradChars)
        before = len(sentence_pairs)
        sentence_pairs = [pair for pair in sentence_pairs if not any(ch in foreign for ch in pair[0])]
        print(f"Example sentences: {before - len(sentence_pairs):,} in the other script dropped, {len(sentence_pairs):,} kept")
    examples = attach_examples(sentence_pairs or [], forms, len(parsed)) if sentence_pairs else None

    records = []
    names = []
    for idx, (trad, simp, pinyin, canto, glosses) in enumerate(parsed):
        level = levels.get(simp) or levels.get(trad) or ""
        reading_canto = canto or jyutping.get((trad, simp, pinyin), "")
        definition = format_definition_cedict(trad, simp, pinyin, glosses, zhuyin, reading_canto, level,
                                              examples[idx] if examples else None)
        def_bytes = definition.encode("utf-8")
        target = names if split_names and is_proper_noun_pinyin(pinyin, glosses) else records
        for hw in dict.fromkeys((trad, simp)):  # both forms, once each
            hw_bytes = hw.encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE:
                skipped += 1
                continue
            # The everyday entry of a form goes first (周 the week before the surname): records
            # sharing a headword keep this order on the device when their priorities tie.
            target.append((hw_bytes, def_bytes, 100, POS_OTHER, forms.get(hw) != idx))
    for out in (records, names):
        out.sort(key=lambda r: (r[0], r[4]))
        out[:] = [r[:4] for r in out]
    print(f"Processed {len(parsed)} CC-CEDICT entries ({skipped} skipped) → {len(records)} vocab records"
          + (f", {len(names)} name records" if split_names else ""))
    return records, names


# ── Level lists(HSK, TOCFL, TBCL) ──────────────────────────────

def load_levels(path: str, name: str) -> dict:
    """{word: "HSK 3"} from a graded CSV/TSV: every Han field in a row is a form of the word,
    and the level is the first field that is a small number or a band such as "7-9"."""
    out = {}
    opener = __import__("gzip").open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip().lstrip("\ufeff")
            if not line or line.startswith("#"):
                continue
            fields = [x.strip().strip('"') for x in re.split(r"[\t,]", line)]
            words = [w for x in fields for w in _split_variants(x)]
            if not words:
                continue
            # Columns before the word hold ids (a bare row number would read as a level).
            firstWord = next(i for i, x in enumerate(fields) if _HAN_RE.search(x))
            level = next((x for x in fields[firstWord + 1:] if _LEVEL_RE.fullmatch(x)), None)
            if level is None:
                # The ivankra CSVs carry the level in the row id: L3-0123 (HSK), L0-1001 (TOCFL, where
                # L0 is the pre-A1 novice band).
                m = re.match(r"L(\d)-\d+", fields[0]) if fields else None
                if m:
                    level = m.group(1)
            if level is None:
                continue
            # TOCFL's band 0 is the pre-A1 "Novice" list; a zero would read like a mistake.
            if level == "0":
                level = "Novice"
            for w in words:
                out.setdefault(w, f"{name} {level}")
    print(f"Level list {path}: {len(out):,} forms tagged {name}")
    return out


# ── Plain TSV(pattern <TAB> definition) ────────────────────────

def convert_tsv(path: str) -> list:
    """Headword<TAB>definition per line, for grammar patterns or name lists from any source. A
    definition may use \\n for a line break; further tab-separated fields are appended as lines."""
    print(f"Loading {path}...")
    records = []
    skipped = 0
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) < 2 or not fields[0].strip():
                skipped += 1
                continue
            hw_bytes = fields[0].strip().encode("utf-8")
            if len(hw_bytes) >= HEADWORD_SIZE:
                skipped += 1
                continue
            definition = "\n".join(x.strip().replace("\\n", "\n") for x in fields[1:] if x.strip())
            records.append((hw_bytes, definition.encode("utf-8"), 100, POS_OTHER))
    print(f"Processed {len(records)} TSV entries ({skipped} skipped)")
    return records


# ── MoE 重編國語辭典(g0v moedict JSON) ─────────────────────────

# {[8e4f]} text references and raw Plane-15 private-use codepoints: glyphs no font carries.
_MOE_GLYPH_REF_RE = re.compile(r"\{\[[0-9a-fA-F]+\]\}|[\U000F0000-\U000FFFFD]")


def _moe_text(value) -> str:
    if value is None:
        return ""
    if isinstance(value, list):
        return " ".join(_moe_text(v) for v in value)
    text = strip_html(str(value))
    return _MOE_GLYPH_REF_RE.sub("□", text).strip()


def format_definitions_moedict(entry: dict) -> list:
    """One definition text per heteronym (reading): the device shows one 【reading】 per record, so
    好 hǎo and 好 hào become two records rather than one whose second reading would vanish."""
    out = []
    for heteronym in entry.get("heteronyms", [])[:3]:
        parts = []
        # Pinyin first, then zhuyin: the order a CC-CEDICT entry built with --zhuyin uses.
        reading = " ".join(x for x in (_moe_text(heteronym.get("pinyin")), _moe_text(heteronym.get("bopomofo"))) if x)
        if reading:
            parts.append("【" + reading + "】")
        definitions = heteronym.get("definitions", [])[:6]
        for d in definitions:
            # One bullet per sense (the device numbers them), the part of speech inside it.
            line = "• "
            kind = _moe_text(d.get("type"))
            if kind:
                line += f"[{kind}] "
            line += _moe_text(d.get("def")) + "\n"
            parts.append(line)
            # One modern example per sense; the classical quotations are left out, they are what
            # makes the full dictionary 50 MB and a learner rarely reads them on a 6-inch screen.
            for example in (d.get("example") or [])[:1]:
                parts.append(_moe_text(example))
        text = "\n".join(p for p in parts if p.strip())
        if "•" in text:
            out.append(text)
    return out


def convert_moedict(path: str) -> list:
    """Convert the g0v dict-revised.json (MoE 重編國語辭典修訂本) to index records."""
    print(f"Loading {path}...")
    if path.endswith(".xz"):
        import lzma
        with lzma.open(path, "rt", encoding="utf-8") as f:
            data = json.load(f)
    else:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    records = []
    skipped = 0
    for entry in data:
        title = _moe_text(entry.get("title", ""))
        # Only headwords the page scan can match: a run of hanzi. Phrases with punctuation
        # (一不做，二不休) and missing glyphs are skipped.
        if not title or not all(_HAN_RE.match(ch) or ch == "〇" for ch in title):
            skipped += 1
            continue
        definitions = format_definitions_moedict(entry)
        hw_bytes = title.encode("utf-8")
        if not definitions or len(hw_bytes) >= HEADWORD_SIZE:
            skipped += 1
            continue
        # Below CC-CEDICT's default so a merged file shows the bilingual entry first.
        for definition in definitions:
            records.append((hw_bytes, definition.encode("utf-8"), 90, POS_OTHER))
    print(f"Processed {len(records)} MoE entries ({skipped} skipped)")
    return records

def keep_names_together(records: list, name_records: list, bilingual_headwords: set) -> tuple:
    """With --split-names, CC-CEDICT's 中國 "China" goes to the names slot while the monolingual
    entry for 中國 from a second dictionary stays in the vocabulary -- and the device asks the
    vocabulary first, so the reader would get the Chinese definition and never the English one.
    A headword that CC-CEDICT holds only as a name takes its other entries along to the names
    slot. bilingual_headwords: every headword CC-CEDICT left in the vocabulary."""
    name_headwords = {r[0] for r in name_records}
    kept = []
    moved = list(name_records)
    for record in records:
        if record[0] in name_headwords and record[0] not in bilingual_headwords:
            moved.append(record)
        else:
            kept.append(record)
    return kept, moved


# ── Format detection & main ─────────────────────────────────────


def detect_format(path: str, lang: str = "ja") -> str:
    """Detect input format from file extension (and, for Chinese, the file name)."""
    lower = os.path.basename(path.lower())
    if lower.endswith(".mdx"):
        return "mdict"
    if lower.endswith(".zip"):
        return "yomitan"
    if lower.endswith(".u8") or lower.endswith(".u8.gz") or "cedict" in lower or "canto" in lower:
        return "cedict"
    if lower.endswith(".tsv"):
        return "tsv"
    if lang in ("zh", "yue") and (lower.endswith(".json") or lower.endswith(".json.xz")):
        return "moedict"
    if lower.endswith(".json") or lower.endswith(".json.tgz") or lower.endswith(".tgz"):
        return "jmdict"
    if lang in ("zh", "yue"):
        return "cedict"
    return "jmdict"


# Latin only: the panel footer is set in the 8 pt UI font, and the CJK cuts start at 12 pt, so a
# title with hanzi in it is drawn half as large again as every other footer.
DEFAULT_TITLES = {"cedict": "CC-CEDICT", "moedict": "MoE", "jmdict": "", "tsv": ""}


def write_stardict(records: list, output_dir: str, title: str) -> None:
    """Write records as a StarDict dictionary (.ifo/.idx/.dict, plain text) for the languages the
    reader looks up word by word, such as Korean. The index is sorted as the reader compares keys:
    ASCII case-insensitively, byte by byte."""
    import struct

    os.makedirs(output_dir, exist_ok=True)
    stem = os.path.join(output_dir, os.path.basename(os.path.normpath(output_dir)))
    # One entry per headword: homographs (먹다 "eat", 먹다 "go deaf") follow each other, set apart
    # by a blank line, each opening with its reading and tags on one line ("[먹따] 〔동사〕").
    merged = {}
    order = []
    for hw, d, prio, _ in records:
        text = d.decode("utf-8")
        reading = ""
        m = re.match(r"^【([^】]*)】(｛[^｝]*｝)?(｟[^｠]*｠)?\n*", text)
        if m:
            reading, text = "[" + m.group(1) + "]", text[m.end():]
        lines = text.strip("\n").split("\n")
        tags = lines.pop(0).strip() if lines and lines[0].startswith("〔") else ""
        word = hw.decode("utf-8")
        # The headword line repeats the key; keep only its level stars (★★★ 먹다 -> ★★★).
        if lines and lines[0].replace("★", "").strip() == word:
            stars = lines.pop(0).replace(word, "").strip()
            tags = (tags + " " + stars).strip()
        head = " ".join(x for x in (reading, tags) if x)
        block = "\n".join(([head] if head else []) + lines).strip()
        if hw not in merged:
            merged[hw] = ([], prio)
            order.append(hw)
        merged[hw][0].append(block)
    def numbered(blocks):
        # The reader's StarDict view drops blank lines, so homographs are numbered to stay apart.
        if len(blocks) < 2:
            return blocks
        return [chr(0x2460 + i) + " " + b if i < 20 else b for i, b in enumerate(blocks)]

    entries = [(hw, "\n".join(numbered(merged[hw][0])).encode("utf-8"), merged[hw][1]) for hw in order]
    key = lambda e: bytes(b + 32 if 65 <= b <= 90 else b for b in e[0])
    entries.sort(key=lambda e: (key(e), -e[2]))
    with open(stem + ".dict", "wb") as dict_file, open(stem + ".idx", "wb") as idx_file:
        offset = 0
        for hw, body, _ in entries:
            dict_file.write(body)
            idx_file.write(hw + b"\0" + struct.pack(">II", offset, len(body)))
            offset += len(body)
    idx_size = os.path.getsize(stem + ".idx")
    with open(stem + ".ifo", "w", encoding="utf-8") as ifo:
        ifo.write("StarDict's dict ifo file\nversion=2.4.2\n")
        ifo.write(f"bookname={title or os.path.basename(stem)}\nwordcount={len(entries)}\n")
        ifo.write(f"idxfilesize={idx_size}\nsametypesequence=m\n")
    print(f"  {stem}.ifo/.idx/.dict: {len(entries)} entries")


def main():
    parser = argparse.ArgumentParser(
        description="Convert dictionary files to CrossPoint binary index.\n\n"
        "Supported formats: JMdict JSON, Yomitan .zip, MDict .mdx, CC-CEDICT text, MoE (g0v) JSON",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--input",
        action="append",
        help="Path to an input dictionary file. Format auto-detected from extension: "
        ".json/.tgz → JMdict, .zip → Yomitan, .mdx → MDict, .u8/.txt → CC-CEDICT, "
        ".json(.xz) with --lang zh → MoE. Repeat to merge several dictionaries into one file; "
        "a word in both shows both entries. If omitted, downloads jmdict-simplified.",
    )
    parser.add_argument("--output-dir", default="output", help="Output directory (default: output)")
    parser.add_argument(
        "--lang",
        default="ja",
        choices=["ja", "zh", "yue", "ko"],
        help="Language of the dictionary: ja (default), zh, or yue for a Cantonese set (CC-Canto "
        "merged with CC-CEDICT, installed under /dictionaries/yue/). Chinese and Cantonese skip the "
        "reading records (pinyin never appears in page text) and name the dictionary for the footer. "
        "ko writes a StarDict dictionary into --output-dir (put it in /dictionaries/ko/<name>/): "
        "Korean is looked up word by word, with particles and endings undone on the device.",
    )
    parser.add_argument(
        "--name",
        default="vocab",
        choices=["vocab", "names", "grammar", "kanji", "hanzi", "hanja"],
        help="Which dictionary slot the output files fill (default: vocab -> vocab.idx/"
        "vocab.dat). The device reads vocab (main dictionary), names (name dictionary), "
        "grammar (grammar dictionary), and kanji (one entry per character, shown after a "
        "word's entries; hanzi and hanja are other names for this slot); pass --name names, "
        "grammar or kanji to fill those slots directly.",
    )
    parser.add_argument(
        "--format",
        choices=["jmdict", "yomitan", "mdict", "cedict", "moedict", "tsv"],
        help="Force input format for every --input (overrides auto-detection). tsv is "
        "headword<TAB>definition per line, for grammar patterns or name lists from any source.",
    )
    parser.add_argument(
        "--title",
        help="Dictionary name shown in the lookup panel footer (written to <name>.title). "
        "Defaults to the Yomitan title, CC-CEDICT, or MoE for Chinese inputs.",
    )
    parser.add_argument(
        "--zhuyin",
        action="store_true",
        help="CC-CEDICT: show zhuyin (bopomofo) beside the pinyin, for Taiwanese Mandarin.",
    )
    parser.add_argument(
        "--frequency",
        help="Word list that ranks the entries (jieba dict.txt, a BCC/SUBTLEX export, or a graded "
        "HSK/TOCFL CSV). Common words get a higher priority, which orders the entries shown "
        "for a word found in several dictionaries.",
    )
    parser.add_argument(
        "--split-names",
        action="store_true",
        help="CC-CEDICT: write proper nouns (capitalised pinyin: places, people, dynasties, but "
        "also languages and nationalities) to names.idx/names.dat beside the vocabulary files, "
        "so they show as Name entries like JMnedict's.",
    )
    parser.add_argument(
        "--levels",
        help="Graded word list (HSK 3.0 or TOCFL CSV): CC-CEDICT entries on it get a level tag "
        "such as [HSK 3] on the entry's grammar line.",
    )
    parser.add_argument(
        "--level-name",
        default="HSK",
        help="Label for --levels tags (default HSK; use TOCFL or TBCL for the Taiwanese lists).",
    )
    parser.add_argument(
        "--jyutping",
        help="cccedict-canto-readings file: adds the Cantonese reading after the pinyin of every "
        "CC-CEDICT entry it covers (CC-Canto's own .u8 as --input carries jyutping already).",
    )
    parser.add_argument(
        "--examples",
        help="Tatoeba sentence pairs export (Chinese to English) or a sentence<TAB>translation "
        "file: each CC-CEDICT entry of two or more characters gets up to two short example "
        "sentences, shown under its glosses.",
    )
    parser.add_argument(
        "--examples-script",
        default="any",
        choices=["any", "simplified", "traditional"],
        help="Keep only example sentences written in this script (Tatoeba mixes both).",
    )
    parser.add_argument(
        "--frequency-kind",
        default="auto",
        choices=["auto", "count", "rank"],
        help="How to read --frequency: by the count column, by row order, or auto (default).",
    )
    parser.add_argument(
        "--pitch",
        action="append",
        help="Japanese: a Yomitan pitch-accent dictionary (NHK 2016, 大辞泉): the lookup panel draws "
        "a line over the high syllables of each reading. Repeat for a fallback.",
    )
    parser.add_argument(
        "--images",
        action="store_true",
        help="Yomitan: keep the pictures some dictionaries carry (デジタル大辞泉, 広辞苑, 日本国語大辞典) as "
        "grayscale BMPs in <output-dir>/images/, shown in the lookup panel. Copy that folder beside the "
        "dictionary files. Needs Pillow (pip install pillow). Off by default: a large dictionary can "
        "hold thousands of pictures (デジタル大辞泉: about 15,000, 380 MB).",
    )
    parser.add_argument(
        "--furigana",
        help="Japanese: JmdictFurigana .json (or .json.zip) to add per-kanji furigana to each kanji "
        "headword's reading line, or 'auto' to download the latest release into --output-dir.",
    )
    args = parser.parse_args()
    global IMAGES_ENABLED
    IMAGES_ENABLED = args.images

    chinese = args.lang in ("zh", "yue", "ko")  # no reading records: the reading is not page text
    if not args.input:
        if chinese:
            print("Error: --lang zh needs --input (a CC-CEDICT file, MoE JSON, Yomitan zip or .mdx).",
                  file=sys.stderr)
            sys.exit(1)
        if args.name != "vocab":
            print(
                f"Error: --name {args.name} without --input would write the auto-downloaded "
                "JMdict (a vocabulary dictionary) into the "
                f"{args.name} slot. Pass --input with the actual "
                f"{args.name} dictionary instead.",
                file=sys.stderr,
            )
            sys.exit(1)
        args.input = [download_jmdict(os.path.join(args.output_dir, "jmdict-eng"))]

    furigana = None
    if args.furigana and not chinese:
        path = download_furigana(args.output_dir) if args.furigana == "auto" else args.furigana
        # ~200k entries: a few tens of MB of Python strings/tuples, host-side only.
        furigana = load_furigana(path)

    levels = load_levels(args.levels, args.level_name) if args.levels else {}
    jyutping = load_canto_readings(args.jyutping) if args.jyutping else {}
    sentence_pairs = load_sentence_pairs(args.examples, args.examples_script) if args.examples else None
    records = []
    name_records = []
    twins = {}  # trad <-> simp forms from CC-CEDICT, for apply_frequency
    trads_of = {}  # simplified form -> its traditional forms, over every input
    seen = {}  # CC-CEDICT entries already converted from an earlier input -> has a Cantonese reading
    bilingual_headwords = set()  # what CC-CEDICT left in the vocabulary; see keep_names_together
    titles = []
    for path in args.input:
        fmt = args.format or detect_format(path, args.lang)
        print(f"{path}: format {fmt}")
        title = DEFAULT_TITLES.get(fmt, "")
        if fmt == "mdict":
            part = convert_mdict(path)
            title = os.path.splitext(os.path.basename(path))[0]
        elif fmt == "yomitan" and args.name in ("kanji", "hanzi", "hanja"):
            part, title = convert_yomitan_kanji(path)
        elif fmt == "yomitan":
            part, title = convert_yomitan(path, reading_records=not chinese, furigana=furigana,
                                           show_tags=args.lang == "ko")
        elif fmt == "cedict":
            part, names = convert_cedict(path, zhuyin=args.zhuyin, split_names=args.split_names, levels=levels,
                                         jyutping=jyutping, sentence_pairs=sentence_pairs, twins=twins,
                                         examples_script=args.examples_script, trads_of=trads_of, seen=seen)
            name_records.extend(names)
            bilingual_headwords.update(r[0] for r in part)
            if "canto" in os.path.basename(path).lower():
                title = "CC-Canto"
        elif fmt == "moedict":
            part = convert_moedict(path)
        elif fmt == "tsv":
            part = convert_tsv(path)
            title = os.path.splitext(os.path.basename(path))[0]
        else:
            part = convert_jmdict(path, furigana=furigana)
        if fmt == "yomitan" and args.lang == "ja" and args.name == "vocab":
            part = [(r[0], _shorten_related(_drop_headword_echo(r[1].decode("utf-8"), r[0].decode("utf-8")))
                     .encode("utf-8")) + tuple(r[2:]) for r in part]
        if title and len(args.input) > 1 and args.lang != "ko":
            label = (SOURCE_MARKER + _short_title(title) + "\n").encode("utf-8")
            part = [(r[0], label + r[1]) + tuple(r[2:]) for r in part]
        records.extend(part)
        if title:
            titles.append(title)

    if name_records:
        records, name_records = keep_names_together(records, name_records, bilingual_headwords)

    if args.frequency:
        priorities = load_frequency(args.frequency, args.frequency_kind)
        shares = shared_simplified_counts(trads_of)
        records = apply_frequency(records, priorities, twins, shares)
        name_records = apply_frequency(name_records, priorities, twins, shares)

    if args.pitch and not chinese:
        pitch = {}
        for path in args.pitch:
            for key, value in load_yomitan_pitch(path).items():
                pitch.setdefault(key, value)
        records = apply_pitch(records, pitch)

    title = args.title or ""
    if not title and titles:
        joined = " + ".join(dict.fromkeys(titles))
        title = joined if len(joined.encode("utf-8")) <= 39 else titles[0]
    if args.lang == "ko":
        write_stardict(records, args.output_dir, title)
        return
    write_binary(records, args.output_dir, args.name, title)
    write_images(args.output_dir)
    if name_records:
        if args.name != "vocab":
            print("Note: --split-names only applies when writing the vocab slot; names kept in the output.")
        else:
            write_binary(name_records, args.output_dir, "names", "CC-CEDICT names")  # only convert_cedict splits names
    if chinese:
        print(f"Install under /dictionaries/{args.lang}/ on the SD card (or /.dictionaries/{args.lang}/).")


if __name__ == "__main__":
    main()
