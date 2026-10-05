# Dictionary setup

Which files go where on the SD card, and how to build them. No Python is needed for the common
cases: [Matcha Reader Tools](https://eszter007.github.io/matcha-reader-tools/) converts
dictionaries in the browser. Using lookup on the device is in the
[User Guide §6](../USER_GUIDE.md#6-language-learning-features); the StarDict format and `.syn`
files are in [dictionary.md](dictionary.md).

## Quick start

**Japanese.** Download `japanese-dictionaries.zip` from the [latest release](https://github.com/eszter007/matcha-reader/releases/latest), unzip it, and copy it to the card so that the card holds `dictionaries/jp/vocab.idx` and the files beside it. Nothing to convert.

**Mandarin.** Download the simplified or the traditional pack from the [`dictionaries-zh` release](https://github.com/eszter007/matcha-reader/releases/tag/dictionaries-zh) and unzip it so that the card holds `dictionaries/zh/`. Install one, not both. To build your own, convert CC-CEDICT with the [browser tool](https://eszter007.github.io/matcha-reader-tools/) or the command under [Chinese](#chinese-mandarin-simplified-or-traditional) below.

**Cantonese.** Convert CC-Canto and CC-CEDICT with the [browser tool](https://eszter007.github.io/matcha-reader-tools/), or the command under [Cantonese](#chinese-cantonese) below, into `dictionaries/yue/`.

**French, English, German and others.** Find a StarDict dictionary (three files: `.ifo`, `.idx`, `.dict` or `.dict.dz`) and copy them into `dictionaries/<lang>/<name>/`, for example `dictionaries/fr/larousse/`. Nothing to convert.

To check it worked, open a book in that language, open the reader menu and choose **Word Lookup**. Reader Settings also shows which dictionary the book reads.

## Folder layout

Word lookup needs at least a vocabulary dictionary for each language you read. A book's language tag picks the folder, so you can keep several languages on one card and never choose by hand.

```
dictionaries/
  jp/                          # Japanese
    vocab.idx    vocab.dat    vocab.spx      # vocabulary (required)
    names.idx    names.dat    names.spx      # names (recommended)
    grammar.idx  grammar.dat  grammar.spx    # grammar reference (optional)
  zh/                          # Mandarin, simplified and traditional books alike
    vocab.idx    vocab.dat    vocab.spx    vocab.title    # vocabulary (required)
    names.idx    names.dat    names.spx    names.title    # proper nouns (optional)
    grammar.idx  grammar.dat  grammar.spx  grammar.title  # grammar patterns (optional)
  yue/                         # Cantonese
    vocab.idx    vocab.dat    vocab.spx    vocab.title
  en/your_dictionary_name/     # English, StarDict files
  fr/your_dictionary_name/     # French, StarDict files
```

The folder can also be called `.dictionaries/`, which hides it from the file browser. It works exactly the same.

## Japanese

Japanese always uses the converted files in `dictionaries/jp/`. Convert them from [Jitendex](https://github.com/stephenmk/Jitendex), [JMnedict](https://github.com/JMdictProject) or any other Yomitan dictionary with the [browser tool](https://eszter007.github.io/matcha-reader-tools/), or the script:

```bash
python3 tools/dict_convert/convert_jmdict.py \
  --input jitendex-yomitan.zip \
  --output-dir /path/to/sd/dictionaries/jp/    # add --name names / --name grammar for the others
```

To add furigana to a Japanese book that has none, annotate the EPUB once before copying it to the card. Readings depend on context, so this always goes through Gemini (the book's text is sent to it under your own key):

```bash
python3 tools/furigana_ruby/add_furigana_ruby.py --ai --gemini-key-file gemini.key book.epub book-furigana.epub
```

## Chinese: Mandarin, simplified or traditional

Ready-made packs come from the [`dictionaries-zh` release](https://github.com/eszter007/matcha-reader/releases/tag/dictionaries-zh): unzip the **simplified** or the **traditional** one onto the card so that it holds `dictionaries/zh/`. Install one, not both: they share the folder. Either pack serves books in both scripts, because every entry is indexed under both forms; they differ in what the entry shows.

| | Simplified pack | Traditional (Taiwanese) pack |
| --- | --- | --- |
| Reading | Pinyin | Pinyin and zhuyin |
| Level tag | [HSK 3.0](https://github.com/ivankra/hsk30) | [TOCFL](https://github.com/ivankra/tocfl) |
| Entries | CC-CEDICT | CC-CEDICT, with the MoE 重編國語辭典 entry under it |
| Ranked by | [jieba](https://github.com/fxsjy/jieba) `dict.txt` (MIT) | jieba `dict.txt.big`, which carries traditional forms too |
| Example sentences | Simplified | Traditional |

To build your own, the same script takes `--lang zh`. Sources: the raw [CC-CEDICT](https://www.mdbg.net/chinese/dictionary?page=cc-cedict) file, the Taiwan Ministry of Education's 重編國語辭典 as the [g0v `dict-revised.json`](https://github.com/g0v/moedict-data), or a Yomitan build such as [CC-CEDICT for Yomitan](https://github.com/MarvNC/cc-cedict-yomitan). The first command below is the simplified set, the second the Taiwanese one:

```bash
python3 tools/dict_convert/convert_jmdict.py --lang zh \
  --input cedict_1_0_ts_utf-8_mdbg.txt --frequency dict.txt --split-names \
  --levels hsk30.csv --level-name HSK \
  --output-dir /path/to/sd/dictionaries/zh/

python3 tools/dict_convert/convert_jmdict.py --lang zh --zhuyin --split-names \
  --input cedict_1_0_ts_utf-8_mdbg.txt --input dict-revised.json.xz \
  --frequency dict.txt.big --levels tocfl-202307.csv --level-name TOCFL \
  --output-dir /path/to/sd/dictionaries/zh/
```

`--frequency` puts the common sense of a word first and makes the page segment by frequency; any list with one word per row works, and a word missing from it takes the rank of its other-script form, so a simplified list still ranks a traditional book. `--split-names` sends proper nouns to the names dictionary.

Optional extras, for either set:

- **Example sentences** from [Tatoeba](https://tatoeba.org/en/downloads): download the Chinese–English sentence pairs and add `--examples "Sentence pairs in Mandarin Chinese-English.tsv"`, and every entry of two or more characters shows up to two short sentences with their translations.
- **A grammar reference** goes in the grammar slot from any two-column file, pattern and explanation, with `--format tsv --name grammar`; the [Chinese Grammar Wiki](https://resources.allsetlearning.com/chinese/grammar/) is CC BY-NC-SA, so that one is for your own card only.
- **Pinyin above the text itself**: annotate the EPUB once before copying it to the card.

  ```bash
  python3 tools/pinyin_ruby/add_pinyin_ruby.py --cedict cedict_1_0_ts_utf-8_mdbg.txt \
    --frequency dict.txt --skip-top 1500 book.epub book-pinyin.epub   # --zhuyin for bopomofo
  ```

  Add `--ai --gemini-key-file gemini.key` to have each character's reading picked in context. The book's text is sent to Gemini under your own key, sentence by sentence; a reading is used only when CC-CEDICT lists it for that character, and the dictionary's is kept otherwise.

## Chinese: Cantonese

Cantonese is its own language with its own words, so a `yue` book reads `dictionaries/yue/` instead. Build it from [CC-Canto](https://cantonese.org/download.html), which holds the Cantonese-only vocabulary with jyutping, merged with CC-CEDICT for everything the two languages share, and the readings file so the shared words carry jyutping too:

```bash
python3 tools/dict_convert/convert_jmdict.py --lang yue \
  --input cccanto-webdist.txt --input cedict_1_0_ts_utf-8_mdbg.txt \
  --jyutping cccedict-canto-readings.txt --output-dir /path/to/sd/dictionaries/yue/
```

The converter writes the `.spx` sparse index that makes lookups fast itself; only a copy of the script run outside the repository needs `python3 scripts/gen_dict_spx.py` on the folder afterwards. CC-CEDICT is CC BY-SA; the MoE dictionary is CC BY-ND and is shortened for the screen without changing its wording; Tatoeba sentences are CC BY.

## Other languages

Every other language uses plain StarDict, with no conversion: put each dictionary in `dictionaries/<lang>/<name>/`, using the language shorthand (`de` for German, `en` for English, `fr` for French, and so on). A book tagged with that language then selects it automatically.

You can put several dictionaries in one language, each in its own folder (`en/collins/`, `en/wiktionary/`). A lookup checks all of them, up to four, and shows every entry it finds one after another: page past the end of one dictionary's entry and the next dictionary's follows, with the footer naming the dictionary and its place (`Collins (1/2)`). The dictionary picked in Settings comes first if it is one of them, then the rest by folder name. Saving a sentence records the dictionary whose entry is on screen.

The dictionary you pick in Settings is also the fallback, used when the book has no language or no folder matches it. Reader Settings shows which dictionary a book reads first.
