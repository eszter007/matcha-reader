#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Character count for the reader status bar: characters before the page on screen, out of the
// chapter's or the book's total. Furigana and whitespace are not counted.
struct CharCount {
  int32_t read = -1;       // -1 hides the item
  int32_t total = -1;
  bool estimated = false;  // some files were scaled from their size: drawn with a leading "~"

  constexpr bool visible() const { return read >= 0; }

  // Plain digits below 10000, whole thousands with a "k" above.
  static void formatValue(char* out, const size_t size, const int32_t value) {
    if (value < 10000) {
      snprintf(out, size, "%d", static_cast<int>(value));
    } else {
      snprintf(out, size, "%dk", static_cast<int>((value + 500) / 1000));
    }
  }

  void format(char* out, const size_t size) const {
    char readStr[12];
    formatValue(readStr, sizeof(readStr), std::max<int32_t>(read, 0));
    char totalStr[12];
    formatValue(totalStr, sizeof(totalStr), total);
    snprintf(out, size, "%s%s/%s", estimated ? "~" : "", readStr, totalStr);
  }

  // Page offsets count every visible codepoint, furigana and whitespace included. This turns
  // one into counted characters by the file's own ratio of the two totals.
  static constexpr uint32_t countedBefore(const uint32_t pageOffset, const uint32_t fileChars,
                                          const uint32_t fileVisibleChars) {
    if (fileVisibleChars == 0) return 0;
    const uint64_t scaled = (static_cast<uint64_t>(pageOffset) * fileChars + fileVisibleChars / 2) / fileVisibleChars;
    return static_cast<uint32_t>(std::min<uint64_t>(scaled, fileChars));
  }
};
