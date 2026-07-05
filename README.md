# CrossPoint Reader — Japanese Language Learning Fork

A fork of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) e-reader firmware for the Xteink X4, focused on **reading Japanese books** with built-in learning tools. Read native Japanese novels and texts with instant dictionary lookup, verb deinflection, grammar references, and AI-powered page translation — all on an e-ink device.

This fork is fully compatible with upstream CrossPoint and can be flashed onto any supported Xteink X4 device.
It can be tested in an emulator before flashing: https://github.com/eszter007/Crosspoint-Emulator

<p align="center">
  <img src="docs/images/screenshots/vertical-text-furigana.png" width="200" alt="Vertical Japanese text with furigana">
  <img src="docs/images/screenshots/word-lookup.png" width="200" alt="Dictionary word lookup">
  <img src="docs/images/screenshots/manga-full-page.png" width="200" alt="Manga reader with panel detection">
  <img src="docs/images/screenshots/reader-menu.png" width="200" alt="Reader menu">
</p>

---

## What's Different from Upstream CrossPoint

This fork adds a complete Japanese reading toolkit on top of the base e-reader:

### Vertical Japanese Text (Tategaki)

Japanese books are automatically detected from EPUB metadata (`<dc:language>ja</dc:language>`) and rendered in vertical text layout — no manual setting needed. The vertical text engine handles:

- Right-to-left column flow with proper line breaking (kinsoku rules)
- Font-adaptive punctuation, bracket, and dash positioning (works with UDDigiKyokasho, Noto Serif, Noto Sans)
- Bold, italic, and emphasis marks (sesame dots ﹅)
- A per-book "Vertical Text: ON/OFF" toggle in the reader menu to override auto-detection (persists across reopens)

<p align="center">
  <img src="docs/images/screenshots/vertical-text-furigana.png" width="260" alt="Vertical text with furigana">
  <img src="docs/images/screenshots/horizontal-text.png" width="260" alt="Same book with vertical text toggled off">
</p>
<p align="center"><em>Same book, "Vertical Text" toggled on (left) vs. off (right)</em></p>

### Dictionary & Word Lookup

Open the reader menu and select **Word Lookup** to look up any word on the current page. Works in both vertical and horizontal reading modes. Only shown for Japanese books.

- **JMdict vocabulary** — Full JMdict/Jitendex dictionary with readings, part-of-speech tags, definitions, and example sentences
- **Verb deinflection** — Conjugated forms resolve to their dictionary base automatically:
  - te-form: 読んで → 読む
  - masu: 食べます → 食べる
  - negative: ありません → ある
  - past: 食べませんでした → 食べる
  - volitional: 眠ろう → 眠る
  - passive: 読まれた → 読む
  - causative: 食べさせる → 食べる
  - compound auxiliaries: 読んでいる, 食べてしまう, etc.
- **Grammar dictionary** — Integrated grammar reference (e.g. "Dictionary of Japanese Grammar" in Yomitan format) surfaces grammar patterns alongside vocabulary
- **Name dictionary (JMnedict)** — Japanese names recognized and grouped with honorifics (根岸さん, 和樹くん shown as one unit)
- **Smart word boundaries** — Pre-scans the page to find dictionary-matchable positions. Filters out single-character particles and conjugation fragments from results. Handles compound words and bound suffixes (設計士).
- **Digit + counter grouping** — Numbers with counters (2年, 15人) shown together with the counter's reading
- **Multiple readings** — Kanji with multiple dictionary entries show all readings sorted by frequency
- **Scrollable definitions** — Long entries scroll with Up/Down. Word navigation with Left/Right

<p align="center"><img src="docs/images/screenshots/word-lookup.png" width="260" alt="Word lookup showing definition, reading, and example sentence"></p>

### Page Translation

Select **Translate Page** from the reader menu to translate the current page from Japanese to English via Google's Gemini 2.5 Flash API. Available for all books (not just Japanese). Requires a Gemini API key on the SD card.

<p align="center"><img src="docs/images/screenshots/translate-page.png" width="260" alt="Translated page"></p>

### Furigana (Ruby Text)

Reading aids rendered above (horizontal) or beside (vertical) kanji, with positioning adjustments so dense furigana doesn't overlap base characters. Can be toggled on/off per-book from the reader menu.

### Localization

All UI chrome this fork adds — menu items and toggles (Word Lookup, Translate Page, Vertical Text, Furigana, Bookmarks/Toggle Bookmark, manga's Panels indicator), popups, and the bookmarks list screens (both EPUB and manga) — goes through the same `tr()`/i18n system as the rest of CrossPoint, so it follows the device's language setting. The same is true for the base CrossPoint UI (Library tabs, Insights, clock sync, firmware update, OPDS servers, keyboard hints, and everything else) — every `STR_*` key in `lib/I18n/translations/english.yaml` currently has a matching translation in all 26 other supported languages, with no English-fallback gaps. Translations were initially machine-translated for the languages that hadn't caught up yet — corrections are welcome via PR, same as any other string in `lib/I18n/translations/` (see [translators.md](docs/translators.md)).

Two features are intentionally **English-only regardless of device language**, since they're producing English *content*, not displaying translatable UI text:
- **Dictionary Word Lookup** — definitions, part-of-speech, and grammar notes come from the underlying JMdict-based dictionary data, which is English-language by design.
- **Page Translation** — the Gemini-powered translation always targets English (Japanese → English), both for EPUB "Translate Page" and manga's pre-extracted panel translations.

### CJK Fallback Font

Non-Japanese-language books that contain occasional CJK characters (e.g. a stray Chinese loanword or Japanese proper noun) render using a curated built-in fallback font covering CJK punctuation, hiragana, katakana, fullwidth forms, and the 2,136 Jōyō kanji (常用漢字, Japan's standard list of common-use kanji) — rather than the full ~21,000-character CJK Unified Ideographs block, which doesn't fit in the available flash budget alongside everything else. The fallback font is regular-weight only (no bold); `EpdFontFamily` falls back to regular for bold requests, which is expected for an occasional-use fallback. Japanese books rendered through the vertical-text engine (see above) use the full font selection path, not this fallback.

### Manga Panel Reader

Read manga with real panel detection, dictionary lookup, and pre-extracted translations. A conversion tool (`tools/manga_convert/`) detects actual panel rectangles with a YOLO model trained on Manga109, asks Gemini what text appears in each panel (plus an English translation), and packs everything into a compact binary format the device reads natively. Page images (JPG/PNG) are used directly — no BMP conversion needed.

- **Full-page view** — Displays the manga page scaled to screen with panel highlight rectangles. When the reading orientation is landscape, a page whose aspect doesn't match the screen rotates to fill it edge-to-edge instead of shrinking into a small centered box (same behavior panel-zoom already had).
- **Panel-by-panel zoom** — Navigate panels in reading order with page turn buttons. Each panel is scaled to fill the screen, rotating to landscape when that fills more of the screen than portrait would.
- **Text overlay** — View OCR'd Japanese text from the current panel, word-wrapped with the reader font
- **Word lookup** — Press Confirm on a zoomed panel (or in full-page view, for the whole page) to open dictionary lookup. Same dictionary, deinflection, and grammar features as EPUB word lookup.
- **Translate Page** — Shows the translation extracted at conversion time instantly, no network call needed. Falls back to a live Gemini call if no translation was pre-extracted for that panel.
- **Progress saving** — Current page and panel position saved automatically. Reaching the last page marks the manga finished in Insights, same as EPUBs.
- **Bookmarks** — Toggle a bookmark on the current page from the reader menu, and browse/jump to/delete bookmarks from the Bookmarks list, same as EPUBs.
- **Title and author** — Read from a `meta.bin` file the converter writes alongside the panel data (auto-detected from the source EPUB/CBZ/PDF, or set explicitly with `--title`/`--author`). Shown in the Library, on shelves, and on the Home screen's Continue Reading card, just like EPUB metadata.
- **Cover and progress in the Library** — The first page is used as the cover thumbnail everywhere (Library grid, shelf list, Home). Progress is shown as a percentage below the cover, the same as EPUBs.
- **Found anywhere on the card** — The Library scans every folder on the SD card, at any nesting depth, for a `panels.idx` file — a manga folder doesn't need to sit directly under a folder named "manga" or live only one level deep.

<p align="center">
  <img src="docs/images/screenshots/manga-full-page.png" width="260" alt="Manga full-page view with panel highlights">
  <img src="docs/images/screenshots/manga-panel-zoom.png" width="260" alt="Manga panel-zoom view">
</p>
<p align="center"><em>Full-page view with panel highlights (left), panel-zoom on a single panel (right)</em></p>

<p align="center"><img src="docs/images/screenshots/library.png" width="260" alt="Library showing manga and Epub covers side by side"></p>

### Image Handling

- Dedicated full-page images with aspect-aware rotation
- No blank pages between consecutive images
- Status bar respected for rotated images
- SVG `<image>` elements (Calibre-generated cover pages) now render correctly

### Home Screen

- Reading progress percentage shown below the author on the "Continue Reading" card
- Progress uses spine-aware calculation (matches the Library view)

### Library

The home menu's **Library** has two tabs:

- **Books** — All books on the SD card as a 3-column cover grid, sorted by recency (recently opened first, then alphabetical). Covers and titles are auto-generated from EPUB metadata on first visit; manga covers use the first page image and title/author from `meta.bin`. A peek row hints at more content below the button bar. The scan walks every folder on the card, at any depth, so books and manga don't need to sit at the SD card root or one level down.
- **Shelves** — Folders on the SD card that contain books, shown as a list with a cover thumbnail, folder name, book count, and a chevron. Tap a shelf to see all books in that folder as a cover grid with progress.

Tab switching uses the same pattern as Settings: Confirm cycles tabs when the tab row is focused, hold Up/Down to switch tabs from anywhere.

### File Browser

Browsing shows every file on the SD card, not just supported formats — files CrossPoint can't open (unsupported extensions) are still listed, grayed out, so nothing on the card is hidden from view. Opening a folder that contains a `panels.idx` (a converted manga) launches the manga reader directly, regardless of where that folder lives.

### Insights

The home menu shows an **Insights** entry (between File Transfer and Settings) that tracks your reading activity:

- **Streak widget** — flame icon with current streak count, weekly minutes, and a Mon–Sun day grid with checkmark circles for days you read
- **Stat cards** (2x2 grid):
  - Books finished
  - Days read
  - Total reading time
  - Longest streak
- **Monthly calendar** — navigate months with Left/Right buttons. Days you read are shown as filled black circles. Today is shown with an outline circle. A "X days read" subtitle summarizes each month.

Reading time is recorded automatically when you close a book (minimum 1 minute to count). Manga counts toward "books finished" the same as EPUBs — reaching the last page marks it, independent of session length. Books finished are counted once per book (no double-counting on re-open). Stats persist in `/system/reading_stats.bin` on the SD card root — unaffected by cache clears or firmware updates.

### Font Selection

The reader uses whatever font is selected in Settings (built-in Noto Serif/Sans or SD card fonts like UDDigiKyokasho). No font is auto-overridden — the user's choice is always respected.

---

## Setup

### Flash the Firmware

Flash this fork's firmware to your Xteink X4 using the standard CrossPoint flashing process. See the [upstream documentation](https://github.com/crosspoint-reader/crosspoint-reader) for flashing instructions.

### Install Dictionaries

The word lookup feature requires dictionary files on the SD card. Three dictionaries are supported:

#### 1. Vocabulary Dictionary (required)

Download [Jitendex](https://github.com/stephenmk/Jitendex) in Yomitan format, then convert:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input jitendex-yomitan.zip \
  --output-dir /path/to/sd/dict/
```

This produces `dict/jmdict.idx` and `dict/jmdict.dat` on the SD card.

#### 2. Name Dictionary (optional, recommended)

Download [JMnedict](https://github.com/JMdictProject) in Yomitan format:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input jmnedict-yomitan.zip \
  --output-dir /path/to/sd/dict/
```

Rename the output files to `jmnedict.idx` and `jmnedict.dat` in the `dict/` folder.

#### 3. Grammar Dictionary (optional)

Convert a grammar reference (e.g. "Dictionary of Japanese Grammar") in Yomitan format:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input grammar-dict-yomitan.zip \
  --output-dir /path/to/sd/dict/
```

Rename the output files to `grammar.idx` and `grammar.dat` in the `dict/` folder.

#### SD Card Layout

After setup, your SD card `dict/` folder should contain:

```
/dict/
  jmdict.idx        # Vocabulary index (required)
  jmdict.dat        # Vocabulary definitions (required)
  jmnedict.idx      # Name dictionary index (optional)
  jmnedict.dat      # Name dictionary definitions (optional)
  grammar.idx       # Grammar reference index (optional)
  grammar.dat       # Grammar reference definitions (optional)
```

### Install Japanese Fonts (optional)

For the best vertical text experience, install Japanese `.cpfont` font files on the SD card. Place them in `/.fonts/` organized by family:

```
/.fonts/
  UDDigiKyokasho/
    regular.cpfont
    bold.cpfont
```

UDDigiKyokasho is auto-selected as the default when available. Other supported fonts include Noto Serif JP and Noto Sans JP — the vertical text engine adapts positioning to each font's metrics.

Without SD card fonts, the built-in Noto Serif/Sans fonts work fine for both horizontal and vertical Japanese text.

### Set Up Translation (optional)

To use the "Translate Page" feature:

1. Get a free API key from [Google AI Studio](https://aistudio.google.com/apikey)
2. Create a file `gemini.key` in the `/system/` folder on the SD card containing just the API key:
   ```
   AIzaSyYOUR_KEY_HERE
   ```

The device needs WiFi access for translation. The emulator uses libcurl instead (no WiFi needed on desktop).

---

## Usage

### Reading a Japanese Book

1. Copy a Japanese EPUB to the SD card
2. Open it from My Library — vertical text mode activates automatically
3. Press **Confirm/Enter** to open the reader menu

### Word Lookup

1. Reader menu → **Word Lookup**
2. **Left/Right** — navigate between matched words on the page
3. **Up/Down** — scroll within a long definition
4. **Back** — return to reading

The position counter (e.g. 10/35) appears in the header. Words are pre-scanned so you only land on positions with actual dictionary matches.

### Translation

1. Reader menu → **Translate Page**
2. Wait for "Translating..." to complete
3. **Up/Down** — scroll the translation
4. **Back** — return to reading

### Reading Manga

1. Prepare your manga using `tools/manga_convert/convert_manga.py` (see below). Pass `--title`/`--author` if you want to set them explicitly — otherwise the converter auto-detects them from the source file's own metadata.
2. Copy the output folder anywhere on the SD card — it contains page images plus `panels.idx`, `panels.dat`, and `meta.bin`. No specific parent folder name or nesting depth is required; the Library finds it automatically.
3. Open the folder from the file browser, or from the Library (manga appears in the book grid, on shelves, and in Continue Reading automatically, with its cover, title, author, and progress percentage)
4. **Page turn buttons** — In full-page view, enters panel zoom on the first panel. In panel zoom, cycles through panels then advances to the next page.
5. **Back** — Returns from panel zoom to full-page view, or from full-page to the file browser
6. **Confirm** — In full-page view, opens the reader menu (Word Lookup, Translate Page, Go to %, Bookmarks, etc.). In panel zoom, opens word lookup directly for the current panel's text.
7. Long-press **Back** — Jump to the file browser from anywhere
8. Reaching the last page marks the manga finished in the Insights tab, the same as an EPUB.

### Toggling Vertical Text

For Japanese books, the reader menu shows **Vertical Text: ON/OFF** and **Furigana: ON/OFF**. Toggle either in-place without leaving the menu. Both settings are per-book and persist across reopens.

---

## Building from Source

This fork uses the same PlatformIO build system as upstream CrossPoint:

```bash
# Clone
git clone https://github.com/eszter007/crosspoint-reader-JP.git
cd crosspoint-reader-JP

# Build
pio run

# Flash
pio run -t upload
```

### Building for the Desktop Emulator

This fork works with the [Crosspoint Emulator](https://github.com/eszter007/Crosspoint-Emulator) for desktop testing. See the emulator's README for setup instructions.

---

## Dictionary Converter

The `tools/dict_convert/convert_jmdict.py` script converts dictionary sources to the binary format the device reads. Supported input formats:

| Format | Extension | Source |
|--------|-----------|--------|
| Yomitan/Yomichan | `.zip` | Jitendex, JMnedict, grammar dicts |
| JMdict JSON | `.json` / `.tgz` | jmdict-simplified |
| MDict | `.mdx` | Any MDict dictionary (requires `pip install readmdict`) |

The converter handles:
- Structured content flattening (Yomitan's nested HTML → plain text with formatting)
- Redirect resolution (variant spellings → canonical entry with full definition)
- Frequency-based priority sorting
- Reading/headword cross-indexing

Output: binary `.idx` (sorted 40-byte records) + `.dat` (UTF-8 definitions) files optimized for the device's constrained memory (binary search, no full-file loading).

---

## Manga Conversion Tools

The `tools/manga_convert/convert_manga.py` script prepares manga for the device. It detects real panel rectangles with a YOLO model trained on Manga109 ([leoxs22/manga-panel-detector-yolo26n](https://huggingface.co/leoxs22/manga-panel-detector-yolo26n)), then asks Gemini what text appears in each panel — and for an English translation of it — storing both directly in the panel data so "Translate Page" works instantly offline.

### Requirements

```bash
pip install ultralytics huggingface_hub Pillow
```

The YOLO weights download automatically on first run and are cached locally. If `ultralytics` isn't installed, the tool falls back to a pure-Pillow white-gutter grid heuristic (lower quality, but zero extra dependencies).

### Usage

```bash
export GEMINI_API_KEY=$(cat /path/to/gemini.key)
python3 tools/manga_convert/convert_manga.py \
  --input /path/to/manga_pages/ \
  --output-dir /path/to/sd/manga/MangaTitle/
```

`--input` accepts an image folder, `.cbz`/`.zip`, or `.epub` (EPUB uses true spine order). Other useful flags:

- `--page-order-file FILE` — explicit page order (one source filename per line) for sources whose filenames don't sort into true reading order
- `--no-ocr` — skip the Gemini calls entirely; panel boxes only, no text/translation/lookup data
- `--max-pages N` — only process the first N pages, useful for a quick test before running the full (potentially expensive) batch
- `--gemini-key-file FILE` — read the API key from a file instead of `GEMINI_API_KEY`
- `--title TITLE` / `--author AUTHOR` — override the book's title/author. When omitted, the converter auto-detects them from the source: EPUB `dc:title`/`dc:creator`, CBZ `ComicInfo.xml`, or PDF document metadata. If nothing is found and these flags aren't passed, the device falls back to the folder name with no author shown.

The Gemini API key is never embedded in the script — pass it via `--gemini-key-file` or the environment at runtime.

### SD Card Layout

```
/manga/MangaTitle/
  page_0000.jpg     # Page images (JPG/PNG, used directly — no BMP conversion)
  page_0001.jpg
  ...
  p0_0.jpg          # Cropped panel images for panel-zoom view (p<page>_<panel>.jpg)
  p0_1.jpg
  ...
  panels.idx        # Panel index (binary, auto-generated)
  panels.dat        # Panel boxes, OCR text, and translations (binary, auto-generated)
  meta.bin          # Title + author (binary, auto-generated when detected)
```

The device detects any folder containing `panels.idx` as a manga book — anywhere on the SD card, at any folder depth. There's no requirement to place it under a folder literally named `manga`; the example path above is just a suggested convention.

---

## Compatibility with Upstream

This fork tracks upstream CrossPoint and can merge new releases. The Japanese features are additive — they don't modify the base reading experience for non-Japanese books. English and other-language EPUBs work identically to upstream.

Key integration points:
- `lib/Dict/` — Dictionary lookup, deinflection (new library, no upstream conflicts)
- `lib/MangaPanel/` — Manga panel binary format parser (new library)
- `lib/Epub/Epub/VerticalSection.*`, `VerticalParsedText.*` — Vertical text engine (new files)
- `src/activities/reader/EpubReaderWordLookupActivity.*` — Word lookup UI (new activity)
- `src/activities/reader/EpubReaderTranslationActivity.*` — Translation UI (new activity)
- `src/activities/reader/MangaReaderActivity.*` — Manga panel reader (new activity)
- `src/activities/reader/MangaWordLookupActivity.*` — Manga word lookup (new activity)
- `src/activities/reader/MangaBookmarksActivity.*` — Manga bookmarks list (new activity)
- `src/activities/reader/EpubReaderActivity.cpp` — Auto-detection and menu wiring (minimal changes to existing code)
- `src/activities/reader/ReaderActivity.cpp` — Manga folder routing (minimal changes)
- `src/activities/home/FileBrowserActivity.cpp` — Manga folder detection (minimal changes)
- `lib/I18n/translations/english.yaml` — New UI strings (additive)

---

## Credits

Built on top of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) — open-source e-reader firmware, community-built, fully hackable, free forever.

Dictionary data from [JMdict](https://www.edrdg.org/jmdict/j_jmdict.html) and [Jitendex](https://github.com/stephenmk/Jitendex), used under their respective licenses.

Icons by [Tabler Icons](https://tabler.io/icons) (MIT license).

Logo in Sleep & Boot Screen by [ふにゃ猫 – funyaneko](https://iconbu.com/).