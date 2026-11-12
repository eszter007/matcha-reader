# Anki card template for sentence mining

A note type for the CSV files that [sentence mining](../../USER_GUIDE.md#sentence-mining) saves. Its cards look like the device's own flashcards: the sentence on the front, and on the back the word large, with its reading over each kanji, then the definition and the sentence again. It has a night mode.

## Import

1. In Anki, choose **File → Import** and pick [`matcha-sentence.apkg`](matcha-sentence.apkg). This adds the **Matcha sentence** note type and a **Matcha** deck with one example card, which you can delete.
2. Import your `sentences-<lang>.csv` with **File → Import**. Choose the **Matcha sentence** note type. Anki matches each column to the field of the same name, so there is nothing to map.

The same note type works for every language. Chinese readings (pinyin, zhuyin, jyutping) appear on the Reading line. Only Japanese words have a Furigana value; words without one show the plain word.

## Files

- [`front.html`](front.html), [`back.html`](back.html) and [`style.css`](style.css): the card template, if you would rather paste it into a note type of your own (**Tools → Manage Note Types → Cards**). Its fields must be named Word, Reading, Sentence, Definition, Book, Author, Date, Dictionary and Furigana.
- `matcha-sentence.apkg` is built from these three files by `tools/anki_template/build_apkg.py` (needs `pip install anki`). Run it again after changing them.
