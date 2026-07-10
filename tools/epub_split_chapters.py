#!/usr/bin/env python3
"""Split single-file EPUB novels into per-chapter files with real ToC entries.

Many Japanese EPUBs ship a whole novel as one or two huge XHTML files whose
chapters exist only as visual number headings (a paragraph containing nothing
but 1 / ２ / 三 between empty paragraphs). E-readers then see a single giant
"chapter": no ToC navigation, page counters spanning the whole book, and heavy
memory pressure while paginating the file.

This tool detects those number-only heading blocks, splits each spine file at
the heading boundaries into separate XHTML files, rewrites the OPF manifest and
spine, and appends matching entries to the EPUB3 nav document and the EPUB2
NCX. The first part keeps the original filename so existing ToC hrefs and
anchors stay valid. Content bytes are never modified -- only sliced -- so text,
furigana, and styling are untouched.

Usage:
    python3 epub_split_chapters.py input.epub [output.epub]

The same detection heuristic is implemented client-side in the firmware's web
file upload page (FilesPage.html); keep the two in sync.
"""

import re
import sys
import zipfile
from pathlib import Path

# A heading is a paragraph whose ENTIRE text is a short numeral (ASCII digits,
# fullwidth digits, or kanji numerals), optionally wrapped in one enclosing div.
NUM = r"(?:[0-9]{1,3}|[０-９]{1,3}|[〇一二三四五六七八九十百]{1,4})"
HEADING_RE = re.compile(
    r"(?:<div[^>]*>\s*)?<p[^>]*>\s*(?:<span[^>]*>\s*)?(" + NUM + r")\s*(?:</span>\s*)?</p>\s*(?:</div>)?",
    re.S,
)
MIN_HEADINGS_PER_FILE = 2


def find_headings(html: str):
    """Return [(split_offset, title)] for number-heading blocks in document order."""
    out = []
    for m in HEADING_RE.finditer(html):
        # The regex is permissive; require the paragraph to be a standalone block:
        # preceded by a tag end (not mid-sentence text).
        before = html[: m.start()].rstrip()
        if before and before[-1] != ">":
            continue
        out.append((m.start(), m.group(1)))
    return out


def open_block_tags(html: str, upto: int):
    """Names of div/section/body elements still open at byte offset `upto`."""
    stack = []
    for m in re.finditer(r"<(/?)(body|div|section)([^>]*)>", html[:upto], re.S):
        closing, name, attrs = m.group(1), m.group(2).lower(), m.group(3)
        if attrs.rstrip().endswith("/"):
            continue
        if closing:
            for i in range(len(stack) - 1, -1, -1):
                if stack[i][0] == name:
                    del stack[i]
                    break
        else:
            stack.append((name, m.group(0)))
    return stack


def split_file(html: str, offsets):
    """Slice html at offsets into parts, each a well-formed document.

    Part 1 spans from the document start to the second heading (front matter glues to
    chapter 1) and keeps the original header. Later parts reuse the original document
    prefix (everything before the first still-open block) plus the open block tags.
    The FINAL part keeps the original closing tags; all others get synthesized ones.
    """
    stack = open_block_tags(html, offsets[0])  # blocks open at every split (headings are siblings)
    prefix_doc = html[: html.find(stack[0][1])] if stack else ""
    open_tags = "".join(tag for _, tag in stack)
    close_tags = "\n" + "".join(f"</{name}>" for name, _ in reversed(stack)) + "\n</html>"

    parts = []
    for i in range(len(offsets)):
        start = 0 if i == 0 else offsets[i]
        end = offsets[i + 1] if i + 1 < len(offsets) else None
        body = html[start:end] if end is not None else html[start:]
        head = "" if i == 0 else prefix_doc + open_tags + "\n"
        closing = close_tags if end is not None else ""  # final part keeps the original tail
        parts.append(head + body + closing)
    return parts


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_name(src.stem + "-chapters.epub")

    zin = zipfile.ZipFile(src)
    files = {n: zin.read(n) for n in zin.namelist()}

    container = files["META-INF/container.xml"].decode("utf-8")
    opf_path = re.search(r'full-path="([^"]+)"', container).group(1)
    opf_dir = str(Path(opf_path).parent)
    opf = files[opf_path].decode("utf-8")

    manifest = dict(re.findall(r'<item[^>]*id="([^"]+)"[^>]*href="([^"]+)"', opf))
    # href may precede id
    for m in re.finditer(r"<item\b[^>]*>", opf):
        tag = m.group(0)
        mid = re.search(r'id="([^"]+)"', tag)
        mhref = re.search(r'href="([^"]+)"', tag)
        if mid and mhref:
            manifest[mid.group(1)] = mhref.group(1)
    spine_ids = re.findall(r'<itemref[^>]*idref="([^"]+)"', opf)

    nav_path = None
    ncx_path = None
    for m in re.finditer(r"<item\b[^>]*>", opf):
        tag = m.group(0)
        href = re.search(r'href="([^"]+)"', tag)
        if not href:
            continue
        if 'properties="nav"' in tag or "nav" in tag and 'properties="nav"' in tag:
            nav_path = str(Path(opf_dir) / href.group(1))
        if 'media-type="application/x-dtbncx+xml"' in tag:
            ncx_path = str(Path(opf_dir) / href.group(1))

    new_files = {}
    toc_additions = []  # (href_rel_to_opf, title)
    spine_replacements = {}  # original idref -> [idrefs]
    manifest_additions = []  # (id, href)

    total_new_chapters = 0
    for idref in spine_ids:
        href = manifest.get(idref)
        if not href or not href.endswith((".xhtml", ".html", ".htm")):
            continue
        full = str(Path(opf_dir) / href)
        full = str(Path(full))  # normalize
        if full not in files:
            continue
        html = files[full].decode("utf-8")
        headings = find_headings(html)
        if len(headings) < MIN_HEADINGS_PER_FILE:
            continue

        offsets = [o for o, _ in headings]
        parts = split_file(html, offsets)
        stem = Path(href).stem
        suffix = Path(href).suffix
        part_ids = []
        for i, part in enumerate(parts):
            if i == 0:
                part_href = href  # keep original name: existing ToC/anchors stay valid
                part_id = idref
            else:
                part_href = str(Path(href).parent / f"{stem}_mr{i + 1}{suffix}").replace("\\", "/")
                if part_href.startswith("./"):
                    part_href = part_href[2:]
                part_id = f"{idref}-mr{i + 1}"
                manifest_additions.append((part_id, part_href))
            new_files[str(Path(opf_dir) / part_href)] = part.encode("utf-8")
            part_ids.append(part_id)
            toc_additions.append((part_href, headings[i][1]))
        spine_replacements[idref] = part_ids
        total_new_chapters += len(parts)
        print(f"{href}: {len(parts)} chapters ({', '.join(t for _, t in headings)})")

    if not spine_replacements:
        print("No splittable number-heading chapters found; nothing to do.")
        sys.exit(0)

    # --- Rewrite OPF ---
    for part_id, part_href in manifest_additions:
        item = f'<item id="{part_id}" href="{part_href}" media-type="application/xhtml+xml"/>'
        opf = opf.replace("</manifest>", item + "\n</manifest>")
    for idref, part_ids in spine_replacements.items():
        m = re.search(r'<itemref[^>]*idref="' + re.escape(idref) + r'"[^>]*/>', opf)
        if m:
            refs = m.group(0) + "".join(f'\n<itemref idref="{pid}" linear="yes"/>' for pid in part_ids[1:])
            opf = opf[: m.start()] + refs + opf[m.end():]
    files[opf_path] = opf.encode("utf-8")

    # --- Rewrite EPUB3 nav ---
    if nav_path and nav_path in files:
        nav = files[nav_path].decode("utf-8")
        nav_dir = Path(nav_path).parent
        items = ""
        for part_href, title in toc_additions:
            target = Path(opf_dir) / part_href
            try:
                rel = target.relative_to(nav_dir)
            except ValueError:
                rel = Path("..") / target
            items += f'<li><a href="{rel.as_posix()}">{title}</a></li>\n'
        # The numbered entries REPLACE any existing entries that point into the split files
        # (e.g. a whole-novel title entry aimed at the first chapter's anchor) -- keeping both
        # would list the book title as if it were a chapter. Insert at the first replaced
        # entry's position to preserve reading order; fall back to before </ol>.
        split_names = {Path(h).name for h, _ in toc_additions}
        removed_spans = []
        for li in re.finditer(r"<li>.*?</li>\s*", nav, re.S):
            href = re.search(r'href="([^"#]+)', li.group(0))
            if href and Path(href.group(1)).name in split_names:
                removed_spans.append((li.start(), li.end()))
        if removed_spans:
            insert_at = removed_spans[0][0]
            for s, e in reversed(removed_spans):
                nav = nav[:s] + nav[e:]
        else:
            m = re.search(r"<nav[^>]*epub:type=\"toc\".*?(</ol>)", nav, re.S)
            insert_at = m.start(1) if m else -1
        if insert_at >= 0:
            nav = nav[:insert_at] + items + nav[insert_at:]
            files[nav_path] = nav.encode("utf-8")

    # --- Rewrite EPUB2 NCX (same replacement semantics as the nav document) ---
    if ncx_path and ncx_path in files:
        ncx = files[ncx_path].decode("utf-8")
        ncx_dir = Path(ncx_path).parent
        split_names = {Path(h).name for h, _ in toc_additions}
        points = ""
        base = 1000  # playOrder past existing entries; most readers ignore exact ordering
        for i, (part_href, title) in enumerate(toc_additions):
            target = Path(opf_dir) / part_href
            try:
                rel = target.relative_to(ncx_dir)
            except ValueError:
                rel = Path("..") / target
            points += (
                f'<navPoint id="mrch-{i + 1}" playOrder="{base + i}">'
                f"<navLabel><text>{title}</text></navLabel>"
                f'<content src="{rel.as_posix()}"/></navPoint>\n'
            )
        removed_spans = []
        for np in re.finditer(r"<navPoint\b(?:(?!</navPoint>).)*</navPoint>\s*", ncx, re.S):
            href = re.search(r'src="([^"#]+)', np.group(0))
            if href and Path(href.group(1)).name in split_names:
                removed_spans.append((np.start(), np.end()))
        if removed_spans:
            insert_at = removed_spans[0][0]
            for s, e in reversed(removed_spans):
                ncx = ncx[:s] + ncx[e:]
            ncx = ncx[:insert_at] + points + ncx[insert_at:]
        else:
            ncx = ncx.replace("</navMap>", points + "</navMap>")
        files[ncx_path] = ncx.encode("utf-8")

    files.update(new_files)

    with zipfile.ZipFile(dst, "w") as zout:
        # mimetype must be first and stored uncompressed
        zout.writestr("mimetype", files["mimetype"], compress_type=zipfile.ZIP_STORED)
        for name, data in files.items():
            if name == "mimetype":
                continue
            zout.writestr(name, data, compress_type=zipfile.ZIP_DEFLATED)

    print(f"Wrote {dst} ({total_new_chapters} chapter files)")


if __name__ == "__main__":
    main()
