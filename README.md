# CrossPoint Reader — Japanese Language Learning Fork

A fork of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) e-reader firmware for the Xteink X4, focused on **reading Japanese books** with built-in learning tools. Read native Japanese novels and texts with instant dictionary lookup, verb deinflection, grammar references, and AI-powered page translation — all on an e-ink device.

This fork is fully compatible with upstream CrossPoint and can be flashed onto any supported Xteink X4 device.
It can be tested in an emulator before flashing: https://github.com/eszter007/Crosspoint-Emulator

---

## What's Different from Upstream CrossPoint

This fork adds a complete Japanese reading toolkit on top of the base e-reader:

### Vertical Japanese Text (Tategaki)

Japanese books are automatically detected from EPUB metadata (`<dc:language>ja</dc:language>`) and rendered in vertical text layout — no manual setting needed. The vertical text engine handles:

- Right-to-left column flow with proper line breaking (kinsoku rules)
- Font-adaptive punctuation, bracket, and dash positioning (works with UDDigiKyokasho, Noto Serif, Noto Sans)
- Bold, italic, and emphasis marks (sesame dots ﹅)
- A per-book "Vertical Text: ON/OFF" toggle in the reader menu to override auto-detection (persists across reopens)

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

### Page Translation

Select **Translate Page** from the reader menu to translate the current page from Japanese to English via Google's Gemini 2.5 Flash API. Available for all books (not just Japanese). Requires a Gemini API key on the SD card.

### Furigana (Ruby Text)

Reading aids rendered above (horizontal) or beside (vertical) kanji, with positioning adjustments so dense furigana doesn't overlap base characters. Can be toggled on/off per-book from the reader menu.

### Image Handling

- Dedicated full-page images with aspect-aware rotation
- No blank pages between consecutive images
- Status bar respected for rotated images

### Library

The home menu's **Library** has two tabs:

- **Books** — Recent books as a 3-column cover grid. Each book displays its cover image with reading progress percentage below. Books without a cover show a placeholder icon with the title. Long-press a book to remove it from the list.
- **Shelves** — Folders on the SD card that contain books, shown as a list with a cover thumbnail, folder name, book count, and a chevron. Tap a shelf to see all books in that folder as a cover grid with progress — not just recent books, but every book file in the folder.

Tab switching uses the same pattern as Settings: Confirm cycles tabs when the tab row is focused, hold Up/Down to switch tabs from anywhere.

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
2. Create a file `gemini.key` at the SD card root containing just the API key:
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

## Compatibility with Upstream

This fork tracks upstream CrossPoint and can merge new releases. The Japanese features are additive — they don't modify the base reading experience for non-Japanese books. English and other-language EPUBs work identically to upstream.

Key integration points:
- `lib/Dict/` — Dictionary lookup, deinflection (new library, no upstream conflicts)
- `lib/Epub/Epub/VerticalSection.*`, `VerticalParsedText.*` — Vertical text engine (new files)
- `src/activities/reader/EpubReaderWordLookupActivity.*` — Word lookup UI (new activity)
- `src/activities/reader/EpubReaderTranslationActivity.*` — Translation UI (new activity)
- `src/activities/reader/EpubReaderActivity.cpp` — Auto-detection and menu wiring (minimal changes to existing code)
- `lib/I18n/translations/english.yaml` — New UI strings (additive)

---

## Credits

Built on top of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) — open-source e-reader firmware, community-built, fully hackable, free forever.

Dictionary data from [JMdict](https://www.edrdg.org/jmdict/j_jmdict.html) and [Jitendex](https://github.com/stephenmk/Jitendex), used under their respective licenses.
