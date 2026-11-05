#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Per-book furigana glossary: harvested (base text -> ruby reading) pairs from <ruby>
// markup, persisted to <bookCachePath>/ruby.bin. Japanese books typically annotate a
// name's reading only on its FIRST appearance; word lookup consults this glossary so
// "In this book: <reading>" can be shown for later, bare-kanji occurrences too.
//
// Unlike section caches, the pairs depend only on the book's content -- never on font,
// margins, or viewport -- so the file survives every relayout and is only invalidated
// with the book cache directory itself.
namespace RubyGlossary {

using Pair = std::pair<std::string, std::string>;  // {baseText, rubyText}, both UTF-8

// Bounded parse-time collection: appends {base, ruby} if it is worth storing (ruby
// non-empty, base contains kanji, both reasonably short) and not already collected.
// Silently drops entries once `pairs` reaches its per-section cap.
void collect(std::vector<Pair>& pairs, const std::string& base, const std::string& ruby);

// Merge collected pairs into <bookCachePath>/ruby.bin, skipping pairs already present.
// Best-effort: caps the file size, tolerates a missing/corrupt file by rewriting it,
// and never fails the caller (a lost glossary entry is cosmetic).
void merge(const std::string& bookCachePath, const std::vector<Pair>& pairs);

// Find the reading(s) recorded for an exact base text. Multiple distinct readings
// (e.g. two characters sharing surname kanji) are joined with '・'. Returns false
// when the glossary has no entry (or no glossary file exists).
bool lookup(const std::string& bookCachePath, const std::string& base, std::string& outReadings);

// Longest glossary base of at least two characters that `text` starts with. Fills outBase and
// its reading(s) as lookup() does. Lets a name the dictionary lacks (緋倉美津子) be selected whole.
bool longestPrefix(const std::string& bookCachePath, const std::string& text, std::string& outBase,
                   std::string& outReadings);

// In-memory key of a base of at least two characters: its hash (hashText) and length in
// characters. The word-lookup scan tests page text against these without reading the file.
struct SpanKey {
  uint32_t hash;
  uint8_t chars;
};
uint32_t hashText(const char* data, size_t len);
size_t charCount(std::string_view utf8);

// Keys of every base of at least two characters, sorted by hash. Returns the file's record count
// (0 when there is no glossary, or when the heap cannot hold the keys).
uint16_t loadSpanKeys(const std::string& bookCachePath, std::vector<SpanKey>& out);

}  // namespace RubyGlossary
