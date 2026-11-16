# Anki card template for sentence mining

A note type for the CSV files that [sentence mining](../../USER_GUIDE.md#sentence-mining) saves. With Anki sync, the device makes this note type in Anki itself and sends the mined words there, so the import below is only needed without sync. Its cards look like the device's own flashcards: the sentence on the front, and on the back the word large, with its reading over each kanji, then the definition and the sentence again. It has a night mode.

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

The device syncs with one of these, set under **Settings → Connectivity → Flashcards Sync**:

- the companion server, [matcha-anki-server](https://github.com/eszter007/matcha-anki-server), which you run yourself with Docker: turn on **Use Sync Server**, enter its **Server Address** and **Server Key**, and set **Anki Server** to **Sync Server**
- Anki desktop 26.08 or later, with AnkiConnect: set **Anki Server** to **Anki Desktop** and enter **Anki Desktop Address** and **Anki Desktop Key**

Then enter the decks to sync under **Anki Decks**, separated by commas. Use the deck names Anki shows; a subdeck is written as `Parent::Child`. After the first sync the device also knows each deck's id, so renaming a deck in Anki does not break the sync; **Anki Decks** then shows the new name. If Anki has no deck with a listed name, the sync still syncs the other decks, then shows an error with the name of the first missing deck. The same settings are also on the web settings page, except **Test Connection**. **Flashcards → Sync with Anki** downloads the cards and sends reviews in both directions. Suspended cards are not downloaded, cards buried in Anki are skipped for that day, and each deck takes new cards per day, the review limit, desired retention, maximum interval, learning steps, card order, sibling burying, Easy Days and leech rule from its preset in Anki once a sync has read them (until then, and when the sync cannot read the preset, the device's Flashcards settings are used).
