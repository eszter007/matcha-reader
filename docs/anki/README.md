# Anki card template for sentence mining

A note type for the CSV files that [sentence mining](../../USER_GUIDE.md#sentence-mining) saves. Its cards look like the device's own flashcards: the sentence on the front, and on the back the word large, with its reading over each kanji, then the definition and the sentence again. It has a night mode.

## Import

1. In Anki, choose **File → Import** and pick [`matcha-sentence.apkg`](matcha-sentence.apkg). This adds the **Matcha sentence** note type and a **Matcha** deck with one example card, which you can delete.
2. Import your `sentences-<lang>.csv` with **File → Import**. Choose the **Matcha sentence** note type. Anki matches each column to the field of the same name, so there is nothing to map.

The same note type works for every language. Chinese readings (pinyin, zhuyin, jyutping) appear on the Reading line. Only Japanese words have a Furigana value; words without one show the plain word.

## Files

- [`front.html`](front.html), [`back.html`](back.html) and [`style.css`](style.css): the card template, if you would rather paste it into a note type of your own (**Tools → Manage Note Types → Cards**). Its fields must be named Word, Reading, Sentence, Definition, Book, Author, Date, Dictionary and Furigana.
- `matcha-sentence.apkg` is built from these three files by `tools/anki_template/build_apkg.py` (needs `pip install anki`). Run it again after changing them.

## Anki decks on the reader

Anki decks can be reviewed under **Flashcards** on the reader. Each card is drawn with its deck's own template and CSS. There are two ways to get a deck onto the reader. Sync is optional.

### Import without sync

`tools/anki_import/apkg_to_flashcards.py` turns an exported `.apkg` into deck folders. Install its requirements once with `pip install anki lxml pillow`, then run:

```bash
python3 tools/anki_import/apkg_to_flashcards.py Kaishi.apkg --sd /Volumes/SDCARD
python3 tools/anki_import/apkg_to_flashcards.py deck.apkg --out ./decks --deck "HSK 1"
```

`--sd` writes to `.system/anki/` on the card. `--out` writes to a folder, which you then copy to `.system/anki/` yourself. Review progress stays on the reader. A card too large for the device is kept in the deck. On the device it says to review it in Anki.

### Two-way sync

Under **Settings → Connectivity → Anki Sync**, enter the address and key of one of these:

- Anki desktop 26.08 or later, with AnkiConnect
- the companion server, `matcha-anki-server`, which you run yourself with Docker

Then enter the decks to sync under **Anki Decks**, separated by commas. Use the deck names Anki shows; a subdeck is written as `Parent::Child`. The same settings are also on the web settings page, except **Test Connection**. **Flashcards → Sync with Anki** downloads the cards and sends reviews in both directions.
