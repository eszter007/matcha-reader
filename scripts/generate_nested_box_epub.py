#!/usr/bin/env python3
"""Generate test/epubs/test_nested_boxes.epub: bordered blocks inside bordered blocks."""

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
"""

FILLER = "The quick brown fox jumps over the lazy dog while the reader turns the page."


def para(text):
    return f"<p>{text}</p>"


SECTIONS = [
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
    ("Across a page break", '<div class="box">' + para("Outer box before a long inner box.") +
     '<div class="inner">' + "".join(para(f"{i}. {FILLER}") for i in range(1, 25)) + "</div>" +
     para("Outer box after the long inner box.") + "</div>" + para("After the long boxes.")),
]


def main():
    body = "".join(f"<h1>{title}</h1>{html}" for title, html in SECTIONS)
    xhtml = f"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en"><head><title>Nested boxes</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head><body>{body}</body></html>"""
    opf = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">test-nested-boxes</dc:identifier>
<dc:title>Nested boxes</dc:title><dc:language>en</dc:language>
<meta property="dcterms:modified">2026-10-18T00:00:00Z</meta></metadata>
<manifest><item id="c1" href="c1.xhtml" media-type="application/xhtml+xml"/>
<item id="css" href="style.css" media-type="text/css"/>
<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/></manifest>
<spine><itemref idref="c1"/></spine></package>"""
    nav = """<?xml version="1.0" encoding="UTF-8"?><html xmlns="http://www.w3.org/1999/xhtml"
xmlns:epub="http://www.idpf.org/2007/ops"><head><title>nav</title></head><body><nav epub:type="toc"><ol>
<li><a href="c1.xhtml">Nested boxes</a></li></ol></nav></body></html>"""
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml",
                   '<?xml version="1.0"?><container version="1.0" '
                   'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles>'
                   '<rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
                   "</rootfiles></container>", compress_type=zipfile.ZIP_DEFLATED)
        for name, content in (("content.opf", opf), ("c1.xhtml", xhtml), ("nav.xhtml", nav), ("style.css", CSS)):
            z.writestr("OEBPS/" + name, content, compress_type=zipfile.ZIP_DEFLATED)
    print(out)


if __name__ == "__main__":
    main()
