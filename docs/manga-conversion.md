# Converting manga, manhua and comics

Matcha reads comics that have been converted once on a computer: panels are found, their text is
read and translated, and the result is a folder the Library picks up. How to read them on the
device is in the [User Guide §6.6](../USER_GUIDE.md#66-manga-manhua-and-comics).

## In the browser (recommended)

Open [**Matcha Reader Tools**](https://eszter007.github.io/matcha-reader-tools/), drop in the book (CBZ, ZIP or PDF), and copy the `.matcha` file it gives
you anywhere on the SD card. For firmware older than 2.0.0-rc-4, pick **Matcha Reader, legacy folder** instead. Nothing to install. The OCR step sends panels to Gemini under your own key.

## As a script

For batch jobs, or the options below:

```bash
pip install ultralytics huggingface_hub Pillow
export GEMINI_API_KEY=$(cat /path/to/gemini.key)

python3 tools/manga_convert/convert_manga.py \
  --input /path/to/manga.cbz \
  --output-dir /path/to/sd/manga/MangaTitle/ \
  --language ja \
  --x4
```

Set `--language` on every book. It splits your reading time by language, and it tells the OCR pass which language to expect. Most manga carries no language of its own. The tag is read at conversion time, so a book converted without it counts as unknown until you convert it again.

`--input` takes an image folder, `.cbz`, `.zip`, `.epub` or PDF. The flags worth knowing:

| Flag | Effect |
| --- | --- |
| `--x4` / `--x3` / `--device NAME` | Scale to the device screen. Smaller files, faster page turns, nothing lost. `--device` takes `x4`, `x4c`, `x4pro`, `sticky`, `papermono`, `picco`, `metalio` (all 480×800), `x3` (528×792), `eego` (EEGO Reader A4, 552×768) or `metalio4plus` (Metalio E-Ink 4 Plus, 684×1216). |
| `--mono` | 1-bit dithered BMP. Good for line art, less so for heavy screentone. |
| `--no-ocr` | Panel boxes only, no Gemini calls, no text or translations. |
| `--ltr` | Read panels left-to-right, for western comics and strips. Default is manga order. |
| `--trim-margins` | Crop the blank paper border and page number off scanned pages. |
| `--webtoon` | Vertical-scroll manhwa or webcomic. Re-cuts the strip into screen-shaped pages. |
| `--yonkoma` | 4-koma strips: read each column top to bottom, then the column to its left. |
| `--max-pages N` | Convert the first N pages as a cheap test. |
| `--title` / `--author` | Override metadata. |
| `--language` | Book language tag. See above. |
| `--series` / `--series-index` | Series name and volume number. Read from a CBZ's ComicInfo (`<Series>`, `<Number>`) or an EPUB's series metadata when present. Sent with your progress to sync servers set to **Other**. |

Panels are found with a YOLO model trained on Manga109 ([leoxs22/manga-panel-detector-yolo26n](https://huggingface.co/leoxs22/manga-panel-detector-yolo26n)), falling back to a white-gutter heuristic without `ultralytics`. Gemini then reads and translates each panel.

Western comics work too. Pass `--ltr` so panels within a row are walked left-to-right. The panel detector was trained on manga but handles strip layouts well; page turn direction is a device setting (Reversed page turn), not a conversion one.

OCR follows `--language`, so it works on any of them. The prompt names the language it should expect, which is what stops the model hallucinating Japanese out of a German speech bubble, and a book already in English gets transcription without a pointless English-to-English translation. Set the tag even if you don't care about reading stats.

Yonkoma (4-koma) needs `--yonkoma`. A 4-koma page is columns of four panels, read down one column and then down the next, where ordinary manga reads across the page. Without the flag the two strips are interleaved: panel 1, the top panel of the *other* strip, panel 2, and so on. The flag swaps the axes of the ordering rule (a tier becomes a column) so a title page whose left half is one full-height illustration beside a strip of four still comes out right, the illustration being a column of its own. Columns run right to left, or left to right with `--ltr`.

Manhwa and other vertical-scroll webcomics need `--webtoon`. A webtoon is one continuous strip, and distributors ship it pre-sliced into fixed-height tiles whose cuts land wherever the slicer's counter reached, often through a face. This reassembles the strip and re-cuts it at the artwork's own gutters into pages shaped to your screen, so no page opens or closes mid-panel. Panels are then the art blocks between gutters, read top to bottom, and the manga panel detector is skipped: it looks for bordered rectangles in a grid and there are none. A 48-tile chapter came out as 42 pages filling 90% of the screen on average.

Add `--trim-margins` for anything scanned from print. It crops the paper border away before panels are detected, which both fills the screen and measurably improves detection: a Moomin page that came back as 9 panels untrimmed, with one whole strip undivided, split into all 11 once the margin was gone.

The output is one file, `<output-dir>.matcha`, holding the page images, panel crops and three small index files. Copy it anywhere on the card. With `--legacy` you get the folder instead, for firmware older than 2.0.0-rc-4; the Library finds any folder containing `panels.idx`.

To turn a folder (from `--legacy` or an older conversion) into a `.matcha`:

```bash
python3 scripts/pack_matcha.py "<book folder>"
```

This also works for books converted earlier. The `.matcha` is a ZIP with every entry stored uncompressed and a
`matcha.idx` lookup table as the first entry, so the reader finds a page in a few small reads. Rename it to `.zip`
and any unzip tool gives back the folder (plus `matcha.idx`). A ZIP made with another tool and renamed to `.matcha`
does not open, because it has no `matcha.idx`. The format is in [file-formats.md](file-formats.md#matcha--single-file-manga-volume).
