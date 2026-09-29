#pragma once

#include <string>
#include <string_view>

// Formatting for the sentence-mining export: one CSV row per saved word, laid out so Anki's
// importer configures itself from the header lines (separator, HTML, GUID and tags columns).
// Pure string work -- no HAL, no SD -- so every rule here is host-testable.
namespace sentencemining {

// Hard caps on what one row may carry. A dictionary entry can run to several KB and a sentence
// with no terminator to a whole page; one row must stay a small, bounded allocation.
constexpr size_t MAX_SENTENCE_CODEPOINTS = 300;
constexpr size_t MAX_DEFINITION_BYTES = 2048;

struct Card {
  std::string word;        // dictionary form (headword)
  std::string reading;     // kana reading; empty for non-Japanese entries
  std::string sentence;    // HTML: escaped text with the word as it appears wrapped in <b>
  std::string definition;  // HTML: escaped text, senses separated by <br>
  std::string book;
  std::string author;
  std::string date;  // YYYY-MM-DD, or empty when the clock was never set
  std::string dictionary;
};

// The comment lines Anki reads before the data, written once when the file is created.
std::string headerLines();

// One complete CSV line for `card`, terminated by '\n'. The GUID and tags are derived here.
std::string formatRow(const Card& card);

// RFC 4180 field: quoted when it holds a comma, quote or line break, with quotes doubled.
std::string csvField(std::string_view value);

// Escapes &, < and > so text reads literally in an HTML field.
std::string htmlEscape(std::string_view text);

// Stable card id from word + sentence: saving the same pair twice yields the same GUID, so
// re-importing the growing file updates cards instead of duplicating them.
std::string makeGuid(std::string_view word, std::string_view sentence);

// Anki splits tags on whitespace: a book title becomes one tag with spaces as underscores.
std::string tagFromTitle(std::string_view title);

// The sentence around a word, as HTML with the word in <b>. `before` is the text preceding the
// word and `after` the text following it (on this page and, when the caller has it, the next).
// Cuts back to just after the previous sentence end and forward through the next one, then caps
// the result at MAX_SENTENCE_CODEPOINTS around the word.
std::string sentenceHtml(std::string_view before, std::string_view word, std::string_view after);

// True when `after` already contains a sentence end, i.e. the caller need not read further.
bool hasSentenceEnd(std::string_view after);

// Cuts `text` to at most `maxBytes` on a UTF-8 boundary, appending "…" when anything was cut.
std::string capUtf8(std::string_view text, size_t maxBytes);

constexpr const char* EXPORT_DIR = "/sentence-mining";

// "/sentence-mining/sentences-<lang>.csv". The code is lowercased and reduced to [a-z0-9-];
// an empty or unusable code files under "other".
std::string exportPath(std::string_view language);

// Language of a StarDict dictionary from its folder below /dictionaries ("en/Collins" -> "en").
// A folder without a language segment falls back to the book's language, reduced to its
// primary subtag ("en-US" -> "en").
std::string languageForDictionary(std::string_view folderName, std::string_view bookLanguage);

// A definition already in HTML (StarDict HTML dictionaries), capped without cutting a tag in half.
std::string capHtml(std::string_view html);

// A dictionary entry's plain text as an HTML field: escaped, with line breaks kept as <br>.
std::string definitionHtml(std::string_view plain);

}  // namespace sentencemining
