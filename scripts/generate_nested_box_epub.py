#!/usr/bin/env python3
"""Generate test/epubs/test_nested_boxes.epub: bordered blocks, nested and with per-side styles."""

import sys
import zipfile
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "test" / "epubs" / "test_nested_boxes.epub"

CSS = """
p { margin: 0.4em 0; }
.box { border: 1px solid black; padding: 0.5em; margin: 0.8em 0; }
.inner { border: 2px dashed black; padding: 0.5em; margin: 0.5em 0.5em; }
.bare { border: 1px solid black; }
.dotted { border: 1px dotted black; padding: 0.4em; margin: 0.4em; }
.rule { border-bottom: 1px solid black; }
.chip { display: inline-block; border: 1px solid black; padding: 0.2em; }
.topbottom { border-top: 1px solid black; border-bottom: 3px double black; padding: 0.3em 0; }
.foursides { border-style: solid dashed dotted double; padding: 0.4em; margin: 0.6em 0; }
.override { border: 1px solid black; border-bottom: 3px dashed black; padding: 0.4em; }
.mixedinner { border-left: 4px solid black; border-top: 1px dotted black; border-right: 1px dotted black;
  border-bottom: 1px dotted black; padding: 0.4em; margin: 0.4em; }
.thick td { border: 3px solid black; padding: 0.2em; }
.newpage { page-break-before: always; }
"""

FILLER = "The quick brown fox jumps over the lazy dog while the reader turns the page."


def para(text):
    return f"<p>{text}</p>"


def first_child_box(label):
    """A box whose first child is another box, so the outer frame has no line of its own yet."""
    return ('<div class="box"><div class="inner">' + para(f"{label}: inner box, first child of the outer.") +
            "</div>" + para("Outer box text after the inner box.") + "</div>")


MAIN = [
    ("Single box", para("Before the box.") + '<div class="box">' + para("One bordered block.") +
     para("Its second paragraph.") + "</div>" + para("After the box.")),
    ("Heading rule", '<h2 class="rule">A heading with a bottom rule</h2>' + para("Text under the rule.")),
    ("Two deep", '<div class="box">' + para("Outer box, first paragraph.") + '<div class="inner">' +
     para("Inner box inside the outer one.") + "</div>" + para("Outer box again, after the inner one.") +
     "</div>" + para("After both boxes.")),
    ("Two deep, no spacing", '<div class="bare">' + para("Outer box without padding.") +
     '<div class="bare">' + para("Inner box without margin or padding.") + "</div>" + "</div>"),
    ("Three deep", '<div class="box">' + para("Level one.") + '<div class="inner">' + para("Level two.") +
     '<div class="dotted">' + para("Level three.") + "</div>" + para("Back in level two.") + "</div>" +
     para("Back in level one.") + "</div>"),
    ("Inline-block inside a box", '<div class="box">' + para("A box holding a chip:") + '<div class="chip">' +
     "chip</div>" + "</div>"),
    ("Box as first child", para("Text before.") + first_child_box("Mid-page")),
    ("Empty rule row first", '<div class="box"><p class="rule"></p>' + para("Text under an empty ruled row.") +
     "</div>"),
    ("Text into a box", "<div>Intro text runs straight into<div class=\"box\">" + para("the box after it.") +
     "</div></div>"),
    ("Per-side styles", '<div class="topbottom">' + para("Thin top rule, thick double bottom.") + "</div>" +
     '<div class="foursides">' + para("Solid top, dashed right, dotted bottom, double left.") + "</div>" +
     '<div class="override">' + para("A solid box whose bottom is overridden to thick dashes.") + "</div>" +
     '<div class="box">' + para("Outer box.") + '<div class="mixedinner">' +
     para("Nested box with a thick left bar and dotted other sides.") + "</div>" + "</div>"),
    ("Forced break on the outer box", para("Text before a box that starts a new page.") +
     '<div class="box newpage"><div class="inner">' + para("Inner box, first child of an outer box that "
     "starts a new page.") + "</div></div>"),
    ("Thick table", '<table class="thick" border="2"><tr><td>A1</td><td>B1</td></tr>'
     "<tr><td>A2</td><td>B2</td></tr></table>"),
    ("Across a page break", '<div class="box">' + para("Outer box before a long inner box.") +
     '<div class="inner">' + "".join(para(f"{i}. {FILLER}") for i in range(1, 25)) + "</div>" +
     para("Outer box after the long inner box.") + "</div>" + para("After the long boxes.")),
]

# Starts with a box whose first child is a box: there is no page yet when it opens.
CHAPTER_START = first_child_box("Chapter start") + para("After the boxes.")

# The same box after growing amounts of filler, so some copies open near the foot of a page.
PAGE_FOOT = "".join("".join(para(FILLER) for _ in range(n)) + first_child_box(f"After {n} paragraphs") +
                    para(FILLER) + '<div class="box"><div class="chip">chip</div>' + para("A chip first.") + "</div>"
                    for n in range(1, 9))


def xhtml(title, body):
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en"><head><title>{title}</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head><body>{body}</body></html>"""


def main():
    chapters = [
        ("c1", "Nested boxes", "".join(f"<h1>{title}</h1>{html}" for title, html in MAIN)),
        ("c2", "Chapter start", CHAPTER_START),
        ("c3", "Page foot", PAGE_FOOT),
    ]
    manifest = "".join(f'<item id="{cid}" href="{cid}.xhtml" media-type="application/xhtml+xml"/>'
                       for cid, _, _ in chapters)
    spine = "".join(f'<itemref idref="{cid}"/>' for cid, _, _ in chapters)
    toc = "".join(f'<li><a href="{cid}.xhtml">{title}</a></li>' for cid, title, _ in chapters)
    opf = f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">test-nested-boxes</dc:identifier>
<dc:title>Nested boxes</dc:title><dc:language>en</dc:language>
<meta property="dcterms:modified">2026-10-18T00:00:00Z</meta></metadata>
<manifest>{manifest}<item id="css" href="style.css" media-type="text/css"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
<spine>{spine}</spine></package>"""
    nav = f"""<?xml version="1.0" encoding="UTF-8"?><html xmlns="http://www.w3.org/1999/xhtml"
xmlns:epub="http://www.idpf.org/2007/ops"><head><title>nav</title></head><body><nav epub:type="toc"><ol>
{toc}</ol></nav></body></html>"""
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   '<?xml version="1.0"?><container version="1.0" '
                   'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
                   '<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
                   "</rootfiles></container>", compress_type=zipfile.ZIP_DEFLATED)
        files = [("content.opf", opf), ("nav.xhtml", nav), ("style.css", CSS)]
        files += [(f"{cid}.xhtml", xhtml(title, body)) for cid, title, body in chapters]
        for name, content in files:
            z.writestr("OEBPS/" + name, content, compress_type=zipfile.ZIP_DEFLATED)
    print(out)


if __name__ == "__main__":
    main()
