#!/usr/bin/env python3
"""Self-check for panel reading order, edge snapping and the OCR prompt.
No deps -- run: python3 test_panel_order.py

Boxes are [x1, y1, x2, y2]. Each ordering case lists panels in an arbitrary
input order and asserts the sequence the reader should walk them in.
"""

from convert_manga import _snap_to_unclaimed_edges, build_panel_ocr_prompt, sort_panels_reading_order


def order(panels, named, rtl):
    """Return the sorted panels as their names from `named` ({name: box})."""
    by_box = {tuple(b): n for n, b in named.items()}
    return [by_box[tuple(b)] for b in sort_panels_reading_order(panels, rtl=rtl)]


def test_strip_page_ltr():
    """A Moomin-style page: four horizontal strips, three panels each."""
    named = {}
    for row in range(4):
        for col in range(3):
            named[f"r{row}c{col}"] = [col * 280, row * 300, col * 280 + 270, row * 300 + 260]
    panels = list(named.values())[::-1]  # shuffled input
    assert order(panels, named, rtl=False) == [f"r{r}c{c}" for r in range(4) for c in range(3)]
    # Same page read as manga: tiers still top-to-bottom, columns reversed.
    assert order(panels, named, rtl=True) == [f"r{r}c{c}" for r in range(4) for c in (2, 1, 0)]


def test_tall_panel_beside_stacked_pair():
    """The layout Y-center clustering gets wrong: one full-height panel next
    to two stacked half-height ones."""
    named = {
        "tall": [0, 0, 300, 400],
        "top": [320, 0, 600, 190],
        "bottom": [320, 210, 600, 400],
        "below": [0, 420, 600, 700],
    }
    panels = [named["below"], named["bottom"], named["tall"], named["top"]]
    assert order(panels, named, rtl=False) == ["tall", "top", "bottom", "below"]
    assert order(panels, named, rtl=True) == ["top", "bottom", "tall", "below"]


def test_full_width_strip_between_rows():
    """A splash panel spanning the page separates the tiers above and below."""
    named = {
        "a": [0, 0, 290, 200],
        "b": [310, 0, 600, 200],
        "splash": [0, 220, 600, 420],
        "c": [0, 440, 290, 640],
        "d": [310, 440, 600, 640],
    }
    panels = [named["d"], named["splash"], named["a"], named["c"], named["b"]]
    assert order(panels, named, rtl=False) == ["a", "b", "splash", "c", "d"]
    assert order(panels, named, rtl=True) == ["b", "a", "splash", "d", "c"]


def test_single_and_empty():
    assert sort_panels_reading_order([], rtl=False) == []
    assert sort_panels_reading_order([[0, 0, 10, 10]], rtl=True) == [[0, 0, 10, 10]]


def test_snap_stops_at_content_not_paper():
    """A page with a printed margin: panels must not be stretched into it."""
    # 1000x1000 page, 80px margin all round (8% -- inside the 15% snap threshold).
    boxes = [[80, 80, 500, 500], [520, 80, 920, 500], [80, 520, 920, 920]]
    assert _snap_to_unclaimed_edges([list(b) for b in boxes], 1000, 1000) == boxes


def test_snap_extends_short_panel_to_its_neighbours():
    """The case snapping exists for: one panel falls short of the row's edge."""
    # Same page, but the top-right panel stops 60px short of the content edge.
    short = [520, 80, 860, 500]
    out = _snap_to_unclaimed_edges([[80, 80, 500, 500], list(short), [80, 520, 920, 920]], 1000, 1000)
    assert out[1] == [520, 80, 920, 500], out[1]


def test_no_panels_dropped():
    """Whatever the layout, every input panel comes back exactly once."""
    panels = [[0, 0, 100, 100], [50, 50, 200, 200], [10, 90, 300, 120], [0, 0, 300, 300]]
    for rtl in (True, False):
        out = sort_panels_reading_order([list(p) for p in panels], rtl=rtl)
        assert sorted(out) == sorted(panels), (rtl, out)


def test_ocr_prompt_names_the_source_language():
    ja = build_panel_ocr_prompt("ja", rtl=True)
    assert "Japanese manga page" in ja
    assert "the Japanese text" in ja
    assert "right-to-left" in ja

    de = build_panel_ocr_prompt("de", rtl=False)
    assert "comic page in German" in de
    assert "the German text" in de
    assert "left-to-right" in de
    assert "Japanese" not in de and "manga" not in de


def test_ocr_prompt_skips_translating_into_the_source_language():
    en = build_panel_ocr_prompt("en", rtl=False)
    assert "no translation is needed" in en
    assert '"translation": ""' in en


def test_ocr_prompt_survives_an_unknown_or_absent_language():
    for tag in ("", "xx", "tlh-Piqd"):
        p = build_panel_ocr_prompt(tag, rtl=False)
        assert "{" in p and "bbox_2d" in p
        assert "None" not in p and "{}" not in p


def test_ocr_prompt_normalizes_the_tag():
    """Region subtags and case must not defeat the lookup."""
    for tag in ("JA", "ja-JP", "ja_JP"):
        assert "Japanese manga page" in build_panel_ocr_prompt(tag, rtl=True), tag
    # Chinese and Korean comics are not manga; only the language name should appear.
    for tag, name in (("zh-Hant", "Chinese"), ("ko", "Korean")):
        p = build_panel_ocr_prompt(tag, rtl=True)
        assert f"comic page in {name}" in p and "manga" not in p, tag


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"ok  {name}")
    print("All panel-order checks passed.")
