# Matcha Reader, a Japanese learning fork of CrossPoint

A fork of [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) e-reader firmware for the Xteink X4 and X3, built for reading Japanese. Vertical text, instant dictionary lookup with verb deinflection, a manga panel reader, and page translation, all on e-ink.

It includes all features of upstream CrossPoint and runs on any supported X4 or X3. You can try it first in the [emulator](https://github.com/eszter007/Crosspoint-Emulator-Matcha).

<p align="center">
  <img src="docs/images/screenshots/vertical-text.png" width="200" alt="Vertical Japanese text">
  <img src="docs/images/screenshots/word-lookup.png" width="200" alt="Dictionary word lookup">
  <img src="docs/images/screenshots/manga-full-page.png" width="200" alt="Manga reader with panel detection">
  <img src="docs/images/screenshots/language-stats.png" width="200" alt="Reading stats split by language">
</p>

Full instructions live in the [User Guide](USER_GUIDE.md). This page is the short version.

---

## Features

### Vertical Japanese text

Japanese books are detected from their metadata and set vertically: right-to-left columns, kinsoku line breaking, sesame emphasis marks, and furigana beside the kanji. A per-book toggle overrides the detection when you disagree with it.

<p align="center">
  <img src="docs/images/screenshots/vertical-text.png" width="260" alt="Vertical Japanese text">
  <img src="docs/images/screenshots/horizontal-text.png" width="260" alt="The same passage with vertical text switched off">
</p>
<p align="center"><em>The same passage, Vertical Text on and off</em></p>

### Dictionary and word lookup

Look up any word on the page, vertically or horizontally. Conjugations resolve to the dictionary form on their own (読んで becomes 読む, 食べませんでした becomes 食べる), and the page is scanned first so you only land on words that actually have an entry.

In vertical text, lookup opens on the page itself: the current word is highlighted where it stands, the side buttons step word by word down the column, Left and Right jump a column, and the definition opens only when you press Look Up. Back returns to the highlighted page, so several words on a page are a few presses apart. The cursor opens mid-page and the scan starts there too, so the half you are looking at is ready first; words it has not reached yet can still be selected, and the highlight moves as soon as the scan arrives. No button labels are drawn over the page — vertical text would be covered by them. Horizontal text opens straight into the definition view as before.

The definition itself opens as a panel floating over the page you were reading: the word sits above a divider at the top, the entry fills the middle, and the dictionary it came from is named along the bottom. In vertical text and in English books the entry is paged a screenful at a time with a page counter in the top right; horizontal Japanese and manga scroll the entry freely and show your position among the page's words instead.

Vocabulary, names and grammar each come from their own dictionary. If the book itself annotated a reading, the entry opens with "In this book: はやし" and remembers it for the rest of the book. See [Setup](#setup) for the files, and [§6.2](USER_GUIDE.md#62-word-lookup) for how to drive it.

Other languages get the same treatment from their StarDict dictionaries. A word at the start of a sentence keeps its accents and still resolves (`École` finds `école`), and French adds its own rules: `l'eau` looks up `eau`, `journaux` finds `journal`, `heureuse` finds `heureux`, and the regular conjugations resolve to the infinitive (`parlaient` → `parler`, `mangeons` → `manger`, `finissent` → `finir`). English and everything else fall back to plurals and verb endings. Irregular verbs that share no stem with their infinitive need a `.syn` file in the dictionary folder — see [docs/dictionary.md](docs/dictionary.md).

Reader Settings includes **Word Lookup Font Size** (Tiny, Small, Medium or Large) for adjusting dictionary entry text.

<p align="center"><img src="docs/images/screenshots/word-lookup.png" width="260" alt="Word lookup with reading, part of speech, definitions and an example sentence"></p>

### Page translation

Translates the current page to English with Gemini. Works in any book, not only Japanese ones. Needs Wi-Fi and your own API key.

<p align="center"><img src="docs/images/screenshots/translate-page.png" width="260" alt="A translated page"></p>

### Manga panel reader

Panels are detected at conversion time, along with their text and translations, so lookup and translation work offline and appear instantly. Move panel by panel in reading order, each one scaled to fill the screen.

**Rotate Panels** (Settings, on by default) turns a panel whose shape does not match the screen, so a wide panel fills the display and you turn the device to read it. Switch it off to keep every panel upright inside the current orientation. **Panels Only** skips the full page overviews. Both are covered in [§6.4](USER_GUIDE.md#64-reading-manga).

Convert with the [browser tool](https://eszter007.github.io/matcha-reader-tools/), or see [Converting manga](#converting-manga).

<p align="center">
  <img src="docs/images/screenshots/manga-full-page.png" width="260" alt="Full page view with panel highlights">
  <img src="docs/images/screenshots/manga-panel-zoom.png" width="260" alt="Panel zoom view">
</p>

### Library

Every book on the card as a cover grid, at any depth. Covers and titles come from the book's own metadata on first
visit, with progress as a badge. Manga sits beside EPUBs. A **Shelves** tab lists folders that contain books.

<p align="center"><img src="docs/images/screenshots/library.png" width="260" alt="Library grid with manga and EPUB covers side by side"></p>

### Reading stats

Streak, minutes this week, books finished, total time, and a calendar of the days you read. Recorded as you go, every few minutes and again when you close a book, so a flat battery costs you minutes rather than the whole session.

Press **Details** for the same numbers per language, one tab each. Long press a book in the Library for its own sessions, total time, average session and calendar.

<p align="center">
  <img src="docs/images/screenshots/insights.png" width="240" alt="Insights with streak, stat cards and calendar">
  <img src="docs/images/screenshots/language-stats.png" width="240" alt="Per-language stats with a tab for each language">
  <img src="docs/images/screenshots/book-stats.png" width="240" alt="Per-book stats for one book">
</p>

Manga counts the same as EPUBs. Language comes from the book, so set `--language` when you convert manga. Details and the known limits are in [§7](USER_GUIDE.md#7-reading-stats).

### Transparent sleep screen

A wallpaper laid over the page you were reading, so the book shows through instead of being covered. Set **Sleep Screen** to **Transparent** and drop 480x800 BMPs into `.sleep/transparent/` on the card. Images with plenty of white space work best, since anything solid hides the text under it. See [§3.7](USER_GUIDE.md#37-sleep-screen).

<p align="center"><img src="docs/images/screenshots/sleep-screen-transparent.png" width="260" alt="Sleep wallpaper over the page text, which stays readable behind it"></p>

### Also in this fork

- Per-book reader settings: font, size, spacing, margins and orientation are remembered per book
- A built-in CJK fallback font, so the odd kanji in a non-Japanese book still renders
- **Optimize EPUB** on upload: splits single-file Japanese novels into real chapters with a working table of contents, and fits images to the screen as dithered 1-bit BMPs
- More of the book's own CSS respected: headings sized as headings, line spacing, page breaks, boxed asides, and rules written as `.callout p`
- **Use Book Margins** (Text Settings > Layout, on by default) keeps the indents a book sets for itself, so epigraphs and long quotations stay inset. Turn it off and those blocks sit flush with the body text
- Instant image page turns, since the next image decodes in the background
- Next-book suggestions at the end of EPUB, TXT/Markdown, XTC and manga books
- A file browser that shows everything on the card, with unsupported files greyed out rather than hidden
- Fully localised, in all the languages CrossPoint ships

---

## Setup

> No Python needed. [**Matcha Reader Tools**](https://eszter007.github.io/matcha-reader-tools/) converts dictionaries, fonts and manga in your browser and hands back a zip laid out for the card. Files stay on your machine, except manga OCR, where panels go to Gemini under your own key. ([source](https://github.com/eszter007/matcha-reader-tools))

**1. Flash the firmware** with the standard CrossPoint process, see the [upstream docs](https://github.com/crosspoint-reader/crosspoint-reader).

**2. Install dictionaries.** Word lookup needs at least a vocabulary dictionary.

Dictionaries are picked by the book's language. Put each one in `dictionaries/<lang>/`, using the language shorthand: `de` for German, `en` for English, `fr` for French, and so on. A book tagged with that language then selects it automatically.

```
dictionaries/
  en/your_dictionary_name/     # English, StarDict files
  fr/your_dictionary_name/     # French, StarDict files
  jp/                          # Japanese, Yomitan files converted for the device
    vocab.idx    vocab.dat    vocab.spx      # vocabulary (required)
    names.idx    names.dat    names.spx      # names (recommended)
    grammar.idx  grammar.dat  grammar.spx    # grammar reference (optional)
```

Japanese is the exception: it always uses the converted files in `dictionaries/jp/`, from [Jitendex](https://github.com/stephenmk/Jitendex), [JMnedict](https://github.com/JMdictProject) or any other Yomitan dictionary. Every other language uses plain StarDict.

The dictionary you pick in Settings becomes the fallback, used when the book has no language or no folder matches it. Reader Settings shows which dictionary a book actually ended up with.

Convert with the [browser tool](https://eszter007.github.io/matcha-reader-tools/), or the script:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input jitendex-yomitan.zip \
  --output-dir /path/to/sd/dictionaries/jp/    # add --name names / --name grammar for the others
```

**3. Install a Japanese font** (optional). The built-in Noto handles Japanese, but a dedicated font looks better. Convert any TTF or OTF with the [browser tool](https://eszter007.github.io/matcha-reader-tools/) and put the result in `.fonts/<Family>/regular.cpfont`. An SD card Japanese font also fills in rare kanji elsewhere, such as dictionary entries and book titles.

**4. Set up translation** (optional). Get a key from [Google AI Studio](https://aistudio.google.com/apikey) and save it as `/system/gemini.key` on the card.

Using all of it: [§6 of the User Guide](USER_GUIDE.md#6-japanese-reading-features).

---

## Converting manga

The [browser tool](https://eszter007.github.io/matcha-reader-tools/) needs no local setup. As a script:

```bash
pip install ultralytics huggingface_hub Pillow
export GEMINI_API_KEY=$(cat /path/to/gemini.key)

python3 tools/manga_convert/convert_manga.py \
  --input /path/to/manga.cbz \
  --output-dir /path/to/sd/manga/MangaTitle/ \
  --language ja \
  --x4
```

Set `--language` on every manga. It is what splits your reading time by language, and most manga carries no language of its own. The tag is read at conversion time, so a book converted without it counts as unknown until you convert it again.

`--input` takes an image folder, `.cbz`, `.zip`, `.epub` or PDF. The flags worth knowing:

| Flag | Effect |
| --- | --- |
| `--x4` / `--x3` | Scale to the device screen. Smaller files, faster page turns, nothing lost. |
| `--mono` | 1-bit dithered BMP. Good for line art, less so for heavy screentone. |
| `--no-ocr` | Panel boxes only, no Gemini calls, no text or translations. |
| `--max-pages N` | Convert the first N pages as a cheap test. |
| `--title` / `--author` | Override metadata. |
| `--language` | Book language tag. See above. |

Panels are found with a YOLO model trained on Manga109 ([leoxs22/manga-panel-detector-yolo26n](https://huggingface.co/leoxs22/manga-panel-detector-yolo26n)), falling back to a white-gutter heuristic without `ultralytics`. Gemini then reads and translates each panel.

The output is a folder of images, panel crops and three small index files. Drop it anywhere on the card, the Library finds any folder containing `panels.idx`.

---

## Building from source

```bash
git clone --recursive https://github.com/eszter007/matcha-reader.git
cd matcha-reader
git submodule update --init --recursive   # if you cloned without --recursive
pio run              # build
pio run -t upload    # flash
```

Same PlatformIO setup as upstream. For desktop testing see the [emulator](https://github.com/eszter007/Crosspoint-Emulator-Matcha). Development notes are in [CLAUDE.md](CLAUDE.md), the on-card cache formats in [docs/file-formats.md](docs/file-formats.md).

## Compatibility with upstream

This fork tracks upstream CrossPoint and merges new releases. Nearly everything is additive: new libraries (`lib/Dict/`, `lib/MangaPanel/`), new activities (word lookup, translation, manga reader) and the vertical text engine. Existing files see only auto-detection and menu wiring, so merges stay cheap.

## Credits

Built on [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader), open-source e-reader firmware, community-built and fully hackable.

Dictionary data from [JMdict](https://www.edrdg.org/jmdict/j_jmdict.html) and [Jitendex](https://github.com/stephenmk/Jitendex), under their respective licences. Icons by [Tabler Icons](https://tabler.io/icons) (MIT). Sleep and boot screen logo by [ふにゃ猫 / funyaneko](https://iconbu.com/).
