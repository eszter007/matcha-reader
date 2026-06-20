#include "WordLookup.h"

#include <cstddef>

#include "Deinflector.h"

namespace {

size_t utf8CharBytes(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  return 4;
}

// Advance `pos` by `n` UTF-8 characters within `text`.
// Returns the new byte position, or text.size() if past end.
size_t advanceChars(const std::string& text, size_t pos, int n) {
  for (int i = 0; i < n && pos < text.size(); i++) {
    pos += utf8CharBytes(static_cast<unsigned char>(text[pos]));
  }
  return pos;
}

}  // namespace

bool WordLookup::lookup(const std::string& paragraphText, size_t byteOffset, WordLookupResult& out) {
  if (byteOffset >= paragraphText.size()) return false;

  // Try decreasing window lengths: 8 characters down to 1.
  // For each window, try the raw text first, then deinflected candidates.
  for (int windowChars = MAX_WINDOW_CHARS; windowChars >= 1; windowChars--) {
    const size_t windowEnd = advanceChars(paragraphText, byteOffset, windowChars);
    if (windowEnd <= byteOffset) continue;

    const std::string window = paragraphText.substr(byteOffset, windowEnd - byteOffset);

    // Try exact match first.
    DictEntry entry;
    if (DictIndex::lookupExact(window.c_str(), entry)) {
      out.entry = std::move(entry);
      out.matchLength = windowEnd - byteOffset;
      return true;
    }

    // Try deinflected candidates.
    auto candidates = Deinflector::deinflect(window);
    // Skip index 0 — that's the raw surface form we already tried.
    for (size_t i = 1; i < candidates.size(); i++) {
      if (DictIndex::lookupExact(candidates[i].text.c_str(), entry)) {
        out.entry = std::move(entry);
        out.matchLength = windowEnd - byteOffset;
        return true;
      }
    }
  }

  return false;
}
