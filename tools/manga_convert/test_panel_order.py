#!/usr/bin/env python3
"""Self-check for panel reading order, webtoon page cuts and the OCR prompt.
No deps -- run: python3 test_panel_order.py

Boxes are [x1, y1, x2, y2]. Each ordering case lists panels in an arbitrary
input order and asserts the sequence the reader should walk them in.
"""

from convert_manga import (
    _webtoon_cut_points,
    build_panel_ocr_prompt,
    expand_panels_over_text,
    sort_panels_reading_order,
    split_frames_over_subpanels,
    text_pad_px,
)


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


def _strip(blocks):
    """A webtoon row profile from (art_height, gutter_height) pairs."""
    rows = []
    for art, gutter in blocks:
        rows += [False] * art + [True] * gutter
    return rows


def test_webtoon_cuts_land_in_the_gutters():
    """Panels a bit shorter than a screen: every cut should fall in a gutter."""
    rows = _strip([(700, 40)] * 8)
    cuts = _webtoon_cut_points(rows, 800)
    for c in cuts[1:-1]:
        assert rows[c], f"cut at {c} is inside artwork"


def test_webtoon_never_exceeds_the_screen():
    """A stretch of art taller than the screen has to be cut mid-panel, but the
    page must still fit the display."""
    rows = _strip([(5000, 30), (900, 30)])
    cuts = _webtoon_cut_points(rows, 800)
    heights = [cuts[i + 1] - cuts[i] for i in range(len(cuts) - 1)]
    assert max(heights) <= 800, heights


def test_webtoon_covers_the_whole_strip_once():
    """No row may be dropped between pages, and none may be emitted twice."""
    rows = _strip([(613, 25), (1500, 40), (222, 60), (990, 15)])
    cuts = _webtoon_cut_points(rows, 800)
    assert cuts[0] == 0 and cuts[-1] == len(rows)
    assert cuts == sorted(cuts), cuts
    assert all(cuts[i + 1] > cuts[i] for i in range(len(cuts) - 1)), cuts


def test_webtoon_short_strip_is_one_page():
    assert _webtoon_cut_points(_strip([(300, 0)]), 800) == [0, 300]


def test_webtoon_prefers_the_latest_usable_gutter():
    """Two gutters in reach: taking the earlier one would waste half a screen."""
    # Gutters at rows 400-419 and 750-769; their midpoints are 410 and 760.
    rows = _strip([(400, 20), (330, 20), (900, 0)])
    cuts = _webtoon_cut_points(rows, 800)
    assert cuts[1] == 760, cuts


def test_bubble_overhanging_the_frame_grows_the_crop():
    """A caption drawn above the frame border must not be sliced off, and the
    crop edge must clear its outline rather than land on it."""
    pad = text_pad_px(800, 800)
    panel = [100, 100, 500, 500]
    caption = [120, 60, 300, 140]  # top half sits outside the frame
    assert expand_panels_over_text([panel], [caption], 800, 800) == [[100, 60 - pad, 500, 500]]


def test_a_bubble_inside_the_panel_never_moves_the_crop():
    """Padding is breathing room for a breached border, not a blanket inset --
    a bubble drawn just inside the frame must leave the crop exactly as it is."""
    panel = [100, 100, 500, 500]
    bubble = [104, 104, 300, 180]  # within `pad` of the border, but inside it
    assert expand_panels_over_text([panel], [bubble], 800, 800) == [panel]


def test_only_the_breached_side_grows():
    panel = [100, 100, 500, 500]
    caption = [120, 60, 300, 140]
    grown = expand_panels_over_text([panel], [caption], 800, 800)[0]
    assert grown[0] == panel[0] and grown[2] == panel[2] and grown[3] == panel[3], grown


def test_bubble_in_a_gutter_goes_to_the_panel_holding_most_of_it():
    """A bubble straddling two panels belongs to one of them, not both."""
    left = [0, 100, 300, 500]
    right = [320, 100, 620, 500]
    bubble = [250, 200, 420, 260]  # 50px in the left panel, 100px in the right
    pad = text_pad_px(800, 800)
    grown = expand_panels_over_text([left, right], [bubble], 800, 800)
    assert grown == [[0, 100, 300, 500], [250 - pad, 100, 620, 500]], grown


def test_text_outside_every_panel_is_ignored():
    """A page number in the margin must not stretch the nearest panel to it."""
    panel = [100, 100, 500, 500]
    page_number = [470, 700, 520, 740]
    assert expand_panels_over_text([panel], [page_number], 800, 800) == [panel]


def test_growth_never_leaves_the_page():
    panel = [10, 100, 500, 500]
    caption = [-40, 60, 300, 200]  # bleeds off the left edge of the page
    pad = text_pad_px(800, 800)
    assert expand_panels_over_text([panel], [caption], 800, 800) == [[0, 60 - pad, 500, 500]]


def test_growth_does_not_cascade_between_panels():
    """One panel growing across the gutter must not make it the owner of the
    next panel's text -- ownership is decided on the original boxes."""
    left = [0, 100, 300, 500]
    right = [320, 100, 620, 500]
    overhang = [250, 200, 420, 260]  # pulls `right` back past x=250
    inner = [60, 300, 200, 360]  # squarely inside `left`
    pad = text_pad_px(800, 800)
    grown = expand_panels_over_text([left, right], [overhang, inner], 800, 800)
    assert grown == [[0, 100, 300, 500], [250 - pad, 100, 620, 500]], grown


def test_pad_scales_with_the_page():
    """A thumbnail scan and a full scan get proportional air, not the same px."""
    assert text_pad_px(290, 420) < text_pad_px(1024, 1449) < text_pad_px(1364, 2000)


def test_a_merged_row_is_split_into_its_subpanels():
    """A strip the model returned as one frame, but also saw three panels in."""
    frame = [0, 0, 900, 300]
    kids = [[0, 0, 290, 300], [300, 0, 590, 300], [600, 0, 900, 300]]
    assert split_frames_over_subpanels([frame], kids + [frame]) == kids


def test_a_frame_seen_twice_is_not_split():
    """The same panel detected at two scales must stay one panel -- the second
    box is not meaningfully smaller, so it is no evidence of a subdivision."""
    frame = [0, 0, 900, 300]
    again = [4, 3, 896, 297]
    assert split_frames_over_subpanels([frame], [frame, again]) == [frame]


def test_one_stray_box_never_splits_a_frame():
    """A weak detection inside a real panel is not a sub-panel."""
    frame = [0, 0, 900, 300]
    stray = [100, 60, 300, 240]
    assert split_frames_over_subpanels([frame], [frame, stray]) == [frame]


def test_subpanels_must_account_for_the_frame():
    """Two small boxes in a corner are not a subdivision of the whole frame."""
    frame = [0, 0, 900, 300]
    kids = [[0, 0, 150, 100], [160, 0, 310, 100]]
    assert split_frames_over_subpanels([frame], kids + [frame]) == [frame]


def test_overlapping_subpanels_are_rejected():
    """Children must tile the frame, not restate the same region."""
    frame = [0, 0, 900, 300]
    kids = [[0, 0, 600, 300], [100, 0, 700, 300]]
    assert split_frames_over_subpanels([frame], kids + [frame]) == [frame]


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"ok  {name}")
    print("All panel-order checks passed.")
