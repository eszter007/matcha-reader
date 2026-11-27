# Dictionary setup

Which files go where on the SD card, and how to build them. No Python is needed for the common
cases: [Matcha Reader Tools](https://eszter007.github.io/matcha-reader-tools/) converts
dictionaries in the browser. Using lookup on the device is in the
[User Guide §6](../USER_GUIDE.md#6-language-learning-features); the StarDict format and `.syn`
files are in [dictionary.md](dictionary.md).

## Quick start

**Japanese.** Download `japanese-dictionaries.zip` from the [dictionaries release](https://github.com/eszter007/matcha-reader-assets/releases/tag/dictionaries-2.0) and copy its `dictionaries` folder to the root of the card, so it holds `dictionaries/jp/`. Nothing to convert.

**Mandarin.** Download the simplified or the traditional pack from the [dictionaries release](https://github.com/eszter007/matcha-reader-assets/releases/tag/dictionaries-2.0) and unzip it so that the card holds `dictionaries/zh/`. Install one, not both. To build your own, convert CC-CEDICT with the [browser tool](https://eszter007.github.io/matcha-reader-tools/) or the command under [Chinese](#chinese-mandarin-simplified-or-traditional) below.

**Cantonese.** Download `dictionaries-yue.zip` from the [dictionaries release](https://github.com/eszter007/matcha-reader-assets/releases/tag/dictionaries-2.0) and unzip it so that the card holds `dictionaries/yue/`. To build your own, convert CC-Canto and CC-CEDICT with the [browser tool](https://eszter007.github.io/matcha-reader-tools/) or the command under [Cantonese](#chinese-cantonese) below.

**French, English, German and others.** Find a StarDict dictionary (three files: `.ifo`, `.idx`, `.dict` or `.dict.dz`) and copy them into `dictionaries/<lang>/<name>/`, for example `dictionaries/fr/larousse/`. Nothing to convert.

To check it worked, open a book in that language, open the reader menu and choose **Word Lookup**. **Reader Settings → Dictionary Settings** in the book also shows which dictionary it reads.

## Folder layout

Word lookup needs at least a vocabulary dictionary for each language you read. A book's language tag picks the folder, so you can keep several languages on one card and never choose by hand.

```
dictionaries/
  jp/                          # Japanese
    vocab.idx    vocab.dat    vocab.spx    vocab.title    # vocabulary (required)
    names.idx    names.dat    names.spx    names.title    # names (recommended)
    grammar.idx  grammar.dat  grammar.spx  grammar.title  # grammar reference (optional)
    kanji.idx    kanji.dat    kanji.spx    kanji.title    # kanji, one entry per character (optional)
  zh/                          # Mandarin, simplified and traditional books alike
    vocab.idx    vocab.dat    vocab.spx    vocab.title    # vocabulary (required)
    names.idx    names.dat    names.spx    names.title    # proper nouns (optional)
    grammar.idx  grammar.dat  grammar.spx  grammar.title  # grammar patterns (optional)
    kanji.idx    kanji.dat    kanji.spx    kanji.title    # hanzi, one entry per character (optional)
  yue/                         # Cantonese
    vocab.idx    vocab.dat    vocab.spx    vocab.title
    kanji.idx    kanji.dat    kanji.spx    kanji.title    # hanzi, one entry per character (optional)
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

Add `--furigana auto` to attach the reading over each kanji from [JmdictFurigana](https://github.com/Doublevil/JmdictFurigana) (downloaded once into `--output-dir`, about 5 MB), or `--furigana JmdictFurigana.json` for a copy you already have. Saved words then get a Furigana column, and flashcards place each reading beside its kanji. A book's own furigana is used first when the word has it.

**Kanji dictionaries.** A Yomitan kanji dictionary (KANJIDIC, Wiktionary 漢字, JPDB Kanji and others) converts with `--name kanji` into the kanji files, with the script (the browser tool does not make kanji files). A lookup, in a book or in manga, then shows the word's own pages first and after them one page for each of the first four kanji of the word that have an entry. Several kanji dictionaries can be merged into one file by repeating `--input`; each shows as its own page. The same works for hanzi under `zh/` or `yue/`.

**Pitch accent.** Add `--pitch` with a Yomitan pitch-accent dictionary (NHK 2016, 大辞泉) when converting the vocabulary: the lookup panel then draws a line over the high syllables of each reading, with a short stroke where the pitch drops. Repeat `--pitch` to fall back to a second one for words the first lacks.

**Pictures.** Some Yomitan dictionaries hold pictures: デジタル大辞泉, 広辞苑 and 日本国語大辞典, for example. Add `--images` to keep them. The lookup panel then shows each picture in the entry, in four shades of gray, at most 400×320 pixels. A picture that does not fit on the rest of a page starts on the next page. The converter writes the pictures as BMP files to an `images/` folder next to the dictionary files; copy that folder to the card too. This is off by default, because the pictures take a lot of space: デジタル大辞泉 has about 15,000 pictures, 380 MB on top of 154 MB of text. It needs Pillow (`pip install pillow`). Small label icons, such as the 〔名〕 part-of-speech marks, become text with or without `--images`.

**Arabic in 深辞海.** 深辞海 (Japanese–Arabic) converts, but the lookup panel cannot show Arabic yet: the Arabic words appear as `�`, even with an Arabic font on the card. The Japanese parts of each entry display normally.

**Frequency lists.** A Yomitan frequency list (a zip with only `term_meta_bank` or `kanji_meta_bank` files, such as Aozora Bunko or Innocent Corpus) has no definitions. Pass it as `--frequency` to order the entries of a word found in several dictionaries.

The `.title` file holds the name shown in the lookup panel. The converter takes it from the Yomitan dictionary's own title (or `--title`); without one the panel shows a default name such as JMdict.

To add furigana to a Japanese book that has none, annotate the EPUB once before copying it to the card. Readings depend on context, so this always goes through Gemini (the book's text is sent to it under your own key):

```bash
python3 tools/furigana_ruby/add_furigana_ruby.py --ai --gemini-key-file gemini.key book.epub book-furigana.epub
```

## Chinese: Mandarin, simplified or traditional

Ready-made packs come from the [dictionaries release](https://github.com/eszter007/matcha-reader-assets/releases/tag/dictionaries-2.0): unzip the **simplified** or the **traditional** one onto the card so that it holds `dictionaries/zh/`. Install one, not both: they share the folder. Either pack serves books in both scripts, because every entry is indexed under both forms; they differ in what the entry shows.

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

  Readings are written as spoken: 一 and 不 change tone before the next syllable (一个 yí gè, 不是 bú shì), particles such as 了, 着 and 得 take the neutral tone, and 儿 joins the syllable before it (哪儿 nǎr). Add `--citation-tones` to keep each character's dictionary tone instead.

  Add `--ai --gemini-key-file gemini.key` to have each character's reading picked in context. The book's text is sent to Gemini under your own key, sentence by sentence; a reading is used only when CC-CEDICT lists it for that character, and the dictionary's is kept otherwise.

## Chinese: Cantonese

Cantonese is its own language with its own words, so a `yue` book reads `dictionaries/yue/` instead. Build it from [CC-Canto](https://cantonese.org/download.html), which holds the Cantonese-only vocabulary with jyutping, merged with CC-CEDICT for everything the two languages share, and the readings file so the shared words carry jyutping too:

```bash
python3 tools/dict_convert/convert_jmdict.py --lang yue \
  --input cccanto-webdist.txt --input cedict_1_0_ts_utf-8_mdbg.txt \
  --jyutping cccedict-canto-readings.txt --output-dir /path/to/sd/dictionaries/yue/
```

The converter writes the `.spx` sparse index that makes lookups fast itself; only a copy of the script run outside the repository needs `python3 scripts/gen_dict_spx.py` on the folder afterwards. CC-CEDICT is CC BY-SA; the MoE dictionary is CC BY-ND and is shortened for the screen without changing its wording; Tatoeba sentences are CC BY.

## Korean

A Korean book (tagged `ko` or `kor`) reads StarDict dictionaries from `dictionaries/ko/<name>/`, like the other languages below. To use a Yomitan Korean dictionary, such as KRDICT, convert it to StarDict with `--lang ko`:

```bash
python3 tools/dict_convert/convert_jmdict.py --lang ko --format yomitan \
  --input krdict-yomitan.zip --output-dir /path/to/sd/dictionaries/ko/krdict/
```

The reading shows in brackets (`[저ː]`), and the part of speech and level as a tag line (`〔감탄사 · 초급〕`). Words on the page resolve to their dictionary form: particles come off nouns (학교에서 → 학교) and verb endings are undone to the 다 form (먹었습니다 → 먹다, 공부했어요 → 공부하다, 더워요 → 덥다). Korean text needs a Korean font on the card, such as Noto Sans KR, and you must choose it yourself: open a Korean book, then **Reader Settings → Text Settings → Font**. Japanese and Chinese books pick a font from the card on their own; Korean books do not, and show `�` until a Korean font is chosen. When a word has several entries with the same spelling (먹다 "to eat" and 먹다 "to go deaf"), each one starts with ①, ② and so on.

## Other languages

Every other language uses plain StarDict, with no conversion: put each dictionary in `dictionaries/<lang>/<name>/`, using the language shorthand (`de` for German, `en` for English, `fr` for French, and so on). A book tagged with that language then selects it automatically.

You can put several dictionaries in one language, each in its own folder (`en/collins/`, `en/wiktionary/`). A lookup checks all of them, up to four, and shows every entry it finds one after another: page past the end of one dictionary's entry and the next dictionary's follows, with the footer naming the dictionary and its place (`Collins (1/2)`). The dictionary picked in Settings comes first if it is one of them, then the rest by folder name. Saving a sentence records the dictionary whose entry is on screen.

The dictionary you pick in Settings is also the fallback, used when the book has no language or no folder matches it. In a book, **Reader Settings → Dictionary Settings** shows which dictionary it reads first.
