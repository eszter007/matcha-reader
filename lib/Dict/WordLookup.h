#pragma once

#include <cstdint>
#include <string>

#include "DictIndex.h"

struct WordLookupResult {
  DictEntry entry;
  size_t matchLength;        // number of UTF-8 bytes from the start position that matched
  bool deinflected = false;  // true if the hit came from a deinflection candidate, not the raw window
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
  // needDefinition=false skips fetching definition text from the .dat files (out.entry.definition
  // is left empty; headword and matchLength are still exact). Use it when only the segmentation is
  // needed (the page pre-scan) -- it saves 1-5 SD reads per match.
  static bool lookup(const std::string& paragraphText, size_t byteOffset, WordLookupResult& out,
                     bool needDefinition = true);

  // Maximum number of characters to consider in the scanning window.
  static constexpr int MAX_WINDOW_CHARS = 8;
};
