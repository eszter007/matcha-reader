# Matcha Reader — Japanese Language Learning CrossPoint Fork

A fork of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) e-reader firmware for the Xteink X4 & X3, focused on **reading Japanese books** with built-in learning tools. Read native Japanese novels and texts with instant dictionary lookup, verb deinflection, grammar references, and AI-powered page translation — all on an e-ink device.

This fork is fully compatible with upstream CrossPoint and can be flashed onto any supported Xteink X4 or X3 device.
It can be tested in an emulator before flashing: https://github.com/eszter007/Crosspoint-Emulator-Matcha

<p align="center">
  <img src="docs/images/screenshots/vertical-text-furigana.png" width="200" alt="Vertical Japanese text with furigana">
  <img src="docs/images/screenshots/word-lookup.png" width="200" alt="Dictionary word lookup">
  <img src="docs/images/screenshots/manga-full-page.png" width="200" alt="Manga reader with panel detection">
  <img src="docs/images/screenshots/reader-menu.png" width="200" alt="Reader menu">
</p>

---

## Features

### Vertical Japanese Text (Tategaki)

Japanese books are detected from EPUB metadata and rendered vertically — right-to-left columns, kinsoku line breaking, font-adaptive punctuation and brackets, bold/italic, and sesame emphasis marks (﹅). A per-book toggle overrides the auto-detection when you want it.

<p align="center">
  <img src="docs/images/screenshots/vertical-text-furigana.png" width="260" alt="Vertical text with furigana">
  <img src="docs/images/screenshots/horizontal-text.png" width="260" alt="Same book with vertical text toggled off">
</p>
<p align="center"><em>Same book, "Vertical Text" on (left) and off (right)</em></p>

### Dictionary & Word Lookup

Look up any word on the page, in vertical or horizontal mode. Conjugations resolve to their dictionary form automatically (読んで → 読む, 食べませんでした → 食べる, 眠ろう → 眠る), and the page is pre-scanned so you only land on words that actually have an entry.

- **Vocabulary (e.g. JMdict/Jitendex)** with readings, part-of-speech, definitions, and example sentences; you can use any other Yomitan dictionary for vocabulary if you wish.
- **Grammar dictionary** — patterns surface alongside vocabulary
- **Name dictionary (e.g. JMnedict)** — names grouped with their honorifics (根岸さん as one unit). You can use any other Yomitan dictionary for names if you wish.
- **The book's own furigana** — if the book annotated a reading, the entry opens with "In this book: はやし", remembered for the rest of the book even where it isn't annotated again

<p align="center"><img src="docs/images/screenshots/word-lookup.png" width="260" alt="Word lookup showing definition, reading, and example sentence"></p>

### Page Translation

Translate the current page to English via Gemini 3.6 Flash. Works in any book, not just Japanese ones. Needs Wi-Fi and your own API key.

<p align="center"><img src="docs/images/screenshots/translate-page.png" width="260" alt="Translated page"></p>

### Manga Panel Reader

Real panel detection, dictionary lookup, and translations extracted at conversion time so they appear instantly offline. Navigate panel by panel in reading order, each scaled to fill the screen; press Confirm on a panel to look up its text.

For converted manga that contain both full pages and panel crops, open the reader menu and enable **Panels Only** to move continuously through snippets in either direction. Pages with no detected panel crop still appear as full-page fallbacks. With the option off, every full-page overview appears before that page's panels. The choice is saved per book. Conversions that omit full-page images enter panel mode automatically.

Page images render directly, JPG, PNG, and BMP are supported. To get started, convert your manga using the [web tool](https://eszter007.github.io/matcha-reader-tools/). For optimal results, check the X3 or X4 option as the target resolution.

<p align="center">
  <img src="docs/images/screenshots/manga-full-page.png" width="260" alt="Manga full-page view with panel highlights">
  <img src="docs/images/screenshots/manga-panel-zoom.png" width="260" alt="Manga panel-zoom view">
</p>
<p align="center"><em>Full-page view with panel highlights (left), panel zoom (right)</em></p>

### Library

Every book on the card as a cover grid, at any folder depth — covers and titles generated from metadata on first visit, progress as a badge. Manga sits alongside EPUBs with its cover, title, and author. A **Shelves** tab lists folders that contain books.

<p align="center"><img src="docs/images/screenshots/library.png" width="260" alt="Library showing manga and EPUB covers side by side"></p>

### Insights

Reading streak, weekly minutes, books finished, total time, and a monthly calendar of the days you read. Recorded automatically when you close a book; manga counts the same as EPUBs.

<p align="center"><img src="docs/images/screenshots/insights.png" width="260" alt="Insights screen with reading streak, stat cards, and monthly calendar"></p>

### Also in this fork

- **Furigana** above (horizontal) or beside (vertical) kanji, per-book toggle, positioned so dense readings don't collide
- **Per-book reader settings** — font, size, spacing, margins, orientation and more are remembered per book; the global settings page holds the defaults for books you haven't opened yet
- **CJK fallback font** — a non-Japanese book with the odd kanji renders it from a built-in font covering kana, the 2,136 Jōyō and 863 Jinmeiyō kanji
- **Chapter splitting** — Japanese novels shipped as one giant XHTML file get real chapters (and a working ToC) when uploaded with **Optimize EPUB**
- **Device-ready EPUB images** — **Optimize EPUB** fits images to the X3/X4 reader viewport and writes 1-bit BMPs using serpentine Floyd–Steinberg dithering
- **Instant image page turns** — the next page's image decodes in the background, so illustrated novels don't stall on it
- **Transparent sleep screen** — a wallpaper overlaid on the page you were reading, so the book shows through
- **Book side margins** — ignore the book's own CSS side margins by default, so the text column follows your margin setting
- **Headings look like headings** — the book's CSS `font-size` is honoured, scaled from *your* reading size and snapped to the built-in sizes around it, so `h1`/`h2` no longer render at body size (built-in fonts only; an SD card font is loaded at one size)
- **No more invisible chapter titles** — a heading the book styles as light text on a coloured panel is drawn as white text on a black bar instead of vanishing into the page; every other colour combination is simply read as normal black text
- **Sections start where the book says they do** — the book's CSS `page-break-before`/`-after`/`-inside` (and the `break-*` spellings) are honoured, so a chapter opens on a fresh page and a heading is no longer left alone at the bottom of one
- **The book's line spacing is honoured** — CSS `line-height` (`1.4`, `1.3em`, `120%`, `18px`, `14pt`) now sets a block's leading, measured from *its own* font size, so tightly-set headings and airy pull-quotes read as typeset; your Line Spacing setting still governs, and the book can only move the leading between 0.8x and 2x of it
- **Styling that lives in a container reaches the text inside it** — CSS rules written as `.callout p` or `blockquote > p` now match, instead of being dropped; books that hang their whole design off a wrapper class finally get their fonts, spacing and page breaks (two levels; `.a .b p` is still ignored)
- **File browser** shows every file on the card, unsupported ones greyed out rather than hidden; opening a folder with `panels.idx` starts the manga reader
- **Your font choice sticks** — nothing auto-overrides the font you picked in Settings, in any book
- **Fully localized** — every string this fork adds goes through the same i18n system as the rest of CrossPoint, translated in all 27 languages. Dictionary definitions and translations stay English, since that's the content, not the interface

---

## Setup

> **No Python needed.** [**Matcha Reader Tools**](https://eszter007.github.io/matcha-reader-tools/) converts
> dictionaries, fonts, and manga in your browser and hands back a zip laid out for the SD card. Files stay on
> your device — the exception is manga OCR, where panel images go to Gemini under your own API key.
> ([source](https://github.com/eszter007/matcha-reader-tools))

**1. Flash the firmware** using the standard CrossPoint process — see the [upstream documentation](https://github.com/crosspoint-reader/crosspoint-reader).

**2. Install dictionaries.** Word lookup needs at least the vocabulary dictionary; the other two are optional.

| Dictionary | Source | Output |
| --- | --- | --- |
| Vocabulary (required) | [Jitendex](https://github.com/stephenmk/Jitendex), Yomitan format | `dictionaries/jp/vocab.*` |
| Names (recommended) | [JMnedict](https://github.com/JMdictProject), Yomitan format | `dictionaries/jp/names.*` |
| Grammar (optional) | e.g. "Dictionary of Japanese Grammar", Yomitan format | `dictionaries/jp/grammar.*` |

Use the [browser tool](https://eszter007.github.io/matcha-reader-tools/), or the script:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input jitendex-yomitan.zip \
  --output-dir /path/to/sd/dict/          # add --name names / --name grammar for the other two
```

Any Yomitan dictionary, jmdict-simplified JSON, or MDict `.mdx` works as input. Cards set up before the move keep working — the firmware also checks the legacy `/dict/` layout.

For regular StarDict dictionaries, use `/dictionaries/<language>/<dictionary>/` (for example `/dictionaries/en/oxford/`), or keep a legacy flat dictionary directly under `/dictionaries/`. Tagged EPUBs automatically use the first matching language folder. The global **Fallback dictionary** is used when no language-specific dictionary can be selected, including flat layouts and books without a language tag; Reader Settings shows the dictionary actually applied and is read-only. Japanese (`ja`) always uses the Yomitan files in `/dictionaries/jp/`.

**3. Install Japanese fonts** (optional). The built-in Noto Serif/Sans handle Japanese fine; a dedicated font looks better. Convert any TTF/OTF with the [browser tool](https://eszter007.github.io/matcha-reader-tools/) and place the result in `/.fonts/<Family>/regular.cpfont`. UDDigiKyokasho is picked as the default when present.

An SD-card Japanese font also serves as a global glyph fallback, so rare kanji in dictionary entries and book titles render instead of coming out blank.

**4. Set up translation** (optional). Get a key from [Google AI Studio](https://aistudio.google.com/apikey) and save it as `/system/gemini.key` on the card. The device needs Wi-Fi for this; the emulator uses libcurl instead.

**How to use all of it:** see [§6 of the User Guide](USER_GUIDE.md#6-japanese-reading-features).

---

## Converting manga

The [browser tool](https://eszter007.github.io/matcha-reader-tools/) does this without a local setup. As a script:

```bash
pip install ultralytics huggingface_hub Pillow
export GEMINI_API_KEY=$(cat /path/to/gemini.key)

python3 tools/manga_convert/convert_manga.py \
  --input /path/to/manga.cbz \
  --output-dir /path/to/sd/manga/MangaTitle/ \
  --x4
```

Panels are found with a YOLO model trained on Manga109 ([leoxs22/manga-panel-detector-yolo26n](https://huggingface.co/leoxs22/manga-panel-detector-yolo26n)); without `ultralytics` it falls back to a white-gutter heuristic. Gemini then reads each panel's text and translates it, both stored in the output so the device needs no network.

`--input` takes an image folder, `.cbz`/`.zip`, `.epub` (true spine order), or PDF. The flags worth knowing:

| Flag | Effect |
| --- | --- |
| `--x4` / `--x3` | Scale pages and panels to the device screen. Smaller files, faster page turns, nothing lost — the screen can't show more. |
| `--mono` | 1-bit dithered BMP. Paints in one fast black-and-white pass; ideal for line art, less so for heavy screentone. |
| `--no-ocr` | Panel boxes only — no Gemini calls, no text or translations. |
| `--max-pages N` | Convert the first N pages as a cheap test run. |
| `--title` / `--author` | Override metadata. Auto-detected from EPUB/CBZ/PDF otherwise. |

`--help` lists the rest. The API key is never written into the output; pass it at runtime.

The result is a folder of page images, panel crops, and three small binaries (`panels.idx`, `panels.dat`, `meta.bin`). Drop it anywhere on the card — the Library finds any folder containing `panels.idx`, at any depth.

---

## Building from Source

```bash
git clone --recursive https://github.com/eszter007/matcha-reader.git
cd matcha-reader
# already cloned without --recursive? the SDK lives in a submodule:
git submodule update --init --recursive
pio run              # build
pio run -t upload    # flash
```

Same PlatformIO setup as upstream. For desktop testing see the [emulator](https://github.com/eszter007/Crosspoint-Emulator-Matcha). Development notes live in [CLAUDE.md](CLAUDE.md); the on-card cache formats are documented in [docs/file-formats.md](docs/file-formats.md).

## Compatibility with Upstream

This fork tracks upstream CrossPoint and can merge new releases. Nearly everything is additive — new libraries (`lib/Dict/`, `lib/MangaPanel/`), new activities (word lookup, translation, manga reader), and the vertical text engine (`lib/Epub/Epub/VerticalSection.*`). Existing files see only auto-detection and menu wiring, so merges stay cheap.

## Credits

Built on [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) — open-source e-reader firmware, community-built, fully hackable, free forever.

Dictionary data from [JMdict](https://www.edrdg.org/jmdict/j_jmdict.html) and [Jitendex](https://github.com/stephenmk/Jitendex), used under their respective licenses. Icons by [Tabler Icons](https://tabler.io/icons) (MIT). Sleep and boot screen logo by [ふにゃ猫 – funyaneko](https://iconbu.com/).
