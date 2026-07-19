#pragma once

#include <string>
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

}  // namespace RubyGlossary
