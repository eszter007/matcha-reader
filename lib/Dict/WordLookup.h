#pragma once

#include <cstdint>
#include <string>

#include "DictIndex.h"

struct WordLookupResult {
  DictEntry entry;
  size_t matchLength;  // number of UTF-8 bytes from the start position that matched
};

// Given a paragraph's source text and a byte offset into it, tries
// decreasing-length windows (8 characters down to 1) at that position,
// looking up each window — both raw and deinflected — in the dictionary
// index.  Returns the longest match found.
//
// This mirrors Yomitan's scanning approach: try the longest candidate
// first, which produces the most specific dictionary hit.
class WordLookup {
 public:
  // Look up the word starting at byteOffset in paragraphText.
  // Returns true and fills `out` if a match was found.
  static bool lookup(const std::string& paragraphText, size_t byteOffset, WordLookupResult& out);

  // Maximum number of characters to consider in the scanning window.
  static constexpr int MAX_WINDOW_CHARS = 8;
};
