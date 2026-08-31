# Dictionary

Look up words while reading an EPUB using an offline StarDict dictionary stored on the SD card.

## Supported Format

The reader supports **StarDict** dictionaries. When searching for dictionaries online, look for "StarDict format" or files with `.dict`, `.idx`, and `.ifo` extensions.

A dictionary folder must contain:

- `.idx` — word index (required, **must be uncompressed** — a `.idx.gz` will not work; decompress it on your computer with `gzip -d` first)
- `.dict` or `.dict.dz` — definition data (`.dict.dz` is supported as-is; entries are decompressed on the fly during lookup)
- `.syn` — synonym index (optional; maps alternate spellings and irregular forms to their headword)
- `.ifo` — metadata (optional)

Not supported: dictionaries with 64-bit index offsets (`idxoffsetbits=64` in the `.ifo` — rare, and rejected with an error).

## Setting Up a Dictionary

1. Copy your dictionary folder(s) to `/dictionaries/` on the SD card — one dictionary per folder, e.g. `/dictionaries/webster/webster.idx` + `webster.dict.dz`. A hidden `/.dictionaries/` folder (dot-prefixed) works the same way, for keeping it out of the file browser.
2. Open **Settings → Reader → Fallback dictionary** on the device.
3. Select the dictionary to use when a book's language has no matching dictionary, or **None** to disable fallback lookups. For a tagged EPUB with a matching language folder, the language-specific dictionary is selected automatically; the Reader Settings **Dictionary** row shows that applied dictionary and is read-only.

The Fallback dictionary row only appears when at least one usable dictionary folder exists. Folders containing more than one dictionary (multiple `.idx` stems) are skipped as ambiguous.

## Looking Up a Word

Two ways to start a lookup while reading:

- Open the reader menu (**Confirm**) and choose **Look Up**.
- Or set **Settings → Controls → Long-press Menu** to "Dictionary", then hold **Confirm** (~0.4s) on the reading page.

One word on the page becomes highlighted:

1. Use **Left/Right** to move between words in reading order, and the side **Up/Down** buttons to jump between lines.
2. Press **Confirm** to look up the highlighted word.
3. Press **Back** to return to the reader.

On the very first lookup with a dictionary (and again whenever the `.idx` or `.syn` source file changes), the reader shows *"Indexing dictionary…"* while it builds small sidecar files next to them — a `.qidx` for the word index, and a `.sidx` when a `.syn` synonym file is present. Each sidecar is rebuilt independently, only when its own source changes. This takes a few seconds for large dictionaries and makes all subsequent lookups fast. The sidecars can be deleted safely at any time — they will simply be rebuilt.

### How Lookup Works

1. **Direct match** — the word is found as-is (case-insensitive) in the dictionary index. Surrounding punctuation is ignored.
2. **Synonyms** — on a miss, if the dictionary ships a `.syn` file, alternate spellings and irregular forms recorded there are resolved to their headword (e.g. `oxen` → `ox`, `colour` → `color`). This step is skipped if the `.sidx` sidecar could not be built (e.g. transient low memory during indexing); the dictionary otherwise stays usable, and the build is retried the next time it is opened.
3. **Inflection** — still no match: word forms are retried automatically, using the rules for the book's language.
   - Accented capitals are folded first, in every language, so a word at the start of a sentence (`École`, `Être`, `Über`) reaches its lowercase headword.
   - **English**, and any language without its own rules: possessives and plurals (`dogs` → `dog`, `stories` → `story`) and verb endings (`walked` → `walk`, `running` → `run`, `making` → `make`).
   - **French**: elision (`l'eau` → `eau`, `qu'il` → `il`), plurals and feminines (`journaux` → `journal`, `heureuse` → `heureux`, `nouvelle` → `nouveau`), and the three regular conjugations across their tenses (`parlaient` → `parler`, `mangeons` → `manger`, `finissent` → `finir`, `vendu` → `vendre`).
4. **Not found** — a short popup appears and you return to word selection.

The rules are selected by the book's language tag, falling back to the language folder the dictionary sits in (`/dictionaries/fr/...`) for a book with no tag. They cover regular morphology only: irregular verbs that share no stem with their lemma (French `est`, `ont`, `fut`, `vais`) are not rule-reachable and need a `.syn`, which resolves them in one step and works in any language.

## The Definition Screen

When a word is found, the definition screen shows the matched headword at the top and the definition text below, with a page counter for long definitions.

HTML dictionaries that declare `sametypesequence=h` use the EPUB text-layout engine for semantic formatting such as headings, bold, italics, lists, and line breaks. Images and CSS styling are ignored. Definitions that are too large or cannot be laid out within the available memory fall back to plain text.

- **Left/Right** or side **Up/Down** — previous / next page
- **Back** — return to word selection



## Where to find dictionaries

> credit to https://github.com/koreader/koreader/wiki/Dictionary-support for the list.

- The [reader.dict](https://www.reader-dict.com) (ex "BoboTiG/ebook-reader-dict") project provides StarDict version of daily dumps of [Wiktionary](https://www.wiktionary.org/) monolingual dictionaries for a variety of languages. It also provides [non-free multilingual](https://www.reader-dict.com) dictionaries.
- The [WikDict](https://www.wikdict.com) project provides free bilingual dictionaries based on [Wiktionary](https://www.wiktionary.org/) for a lot of language pairs. StarDict versions can be [downloaded from here](https://download.wikdict.com/dictionaries/stardict/).
- The [`Vuizur/Wiktionary-Dictionaries`](https://github.com/Vuizur/Wiktionary-Dictionaries) repository contains dictionaries based on [Wiktionary](https://www.wiktionary.org/) from many languages to English, including English-English.
- The [DictInfo](https://www.dictinfo.com/) website provides outdated monolingual dictionaries based on [Wiktionary](https://www.wiktionary.org/).
- The [Firedict site](https://tuxor1337.frama.io/firedict/dictionaries.html) contains a list of freely available dictionaries.
- [wiktionary_stardict](https://xxyzz.github.io/wiktionary_stardict/): update monthly.
- [Fictionaries](https://fictionary.gumroad.com/) provides dictionaries for various speculative fiction books and series.
- [World Factbooks Archive](https://github.com/MilkMp/CIA-World-Factbooks-Archive-1990-2025) provides 36 years of CIA's World Factbook dictionaries in StarDict format.
- [StarDict-Hebrew](https://github.com/Uri-Tauber/StarDict-Hebrew) Hebrew-English StarDict versions of Babylon dictionaries.
