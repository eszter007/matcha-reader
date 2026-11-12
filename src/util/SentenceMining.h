#pragma once

#include <SentenceMiningCsv.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Saving looked-up words for sentence mining: one CSV row per word, appended to a file Anki
// imports with no setup (see SentenceMiningCsv.h for the row layout).
namespace sentencemining {

// Language code of the Japanese dictionary, whose files carry no language folder of their own.
constexpr const char* JAPANESE = "ja";

// What a card needs from the reader that a lookup screen cannot see for itself: the book, and
// the text that follows the page, so a sentence the page turn cut off can still be finished.
struct BookContext {
  std::string bookTitle;
  std::string bookAuthor;
  std::string nextPageText;
  // The book's own language, for views whose dictionary does not say (manga: a comic in any
  // language). Empty when unknown.
  std::string bookLanguage;
  // The book's identity in BookStats, which counts lookups and saved sentences per book.
  std::string bookPath;
  // Where the lookup was made, for vocabulary history. Unknown: empty, 0, or UINT32_MAX.
  std::string chapter;
  std::string series;
  std::optional<float> seriesIndex;
  uint16_t page = 0;  // 1-based page in the chapter
  uint16_t spine = 0;
  uint32_t startOffset = UINT32_MAX;
};

// A card waiting for its definition, handed from a word-select screen to the definition screen,
// plus the language file it belongs in.
struct Draft {
  Card card;
  std::string language;
  std::string bookPath;  // BookStats identity, for the saved-sentence counter
  bool valid() const { return !card.word.empty(); }
};

enum class SaveResult : uint8_t { Saved, AlreadySaved, Failed };

// Appends `card` to the file for `language` (the dictionary's language code, e.g. "ja" or "en")
// unless that file already has a row for the same word and sentence (the GUID); the same word
// in another sentence is a new card. Anki's header lines go first when the file is new. One file
// per language, so each imports into its own deck. A file is only ever extended.
SaveResult saveOnce(const Card& card, std::string_view language);

// Brings a sentence file written before the Furigana column to the current layout, in place.
// True when the file is current (or absent); false when a legacy file could not be rewritten,
// in which case it is left as it was.
bool upgradeExport(const std::string& path);

// Today's date as YYYY-MM-DD, or empty when the device has no idea what day it is.
std::string today();

}  // namespace sentencemining
