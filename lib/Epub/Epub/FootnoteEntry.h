#pragma once

#include <HalStorage.h>

#include <algorithm>
#include <cstring>
#include <string>

#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 256
// The href's fixed width is the ON-DISK field only. In RAM it is a std::string: a
// 288-byte fixed record made every footnote container a large-element vector, and the
// section-wide table (128 entries) then needed 37KB *contiguous* to grow into -- which
// aborts through operator new once a reading session has fragmented the heap. As a
// string the record is ~36 bytes, so the same table wants ~4.6KB contiguous plus one
// small allocation per href.
static_assert(FOOTNOTE_HREF_LEN % 32 == 0, "href padding is written/read in 32-byte chunks");

struct FootnoteEntry {
  char number[FOOTNOTE_NUMBER_LEN];
  std::string href;

  FootnoteEntry() { number[0] = '\0'; }
};

// Fixed-width href I/O, so the section-file format is unchanged by the in-RAM shrink.
// Both helpers work in 32-byte chunks rather than on a FOOTNOTE_HREF_LEN stack buffer.
inline bool writeFootnoteHref(HalFile& f, const std::string& href) {
  const size_t len = std::min(href.size(), static_cast<size_t>(FOOTNOTE_HREF_LEN - 1));
  if (len > 0 && f.write(href.data(), len) != len) return false;
  char zeros[32] = {};
  for (size_t left = FOOTNOTE_HREF_LEN - len; left > 0;) {
    const size_t n = std::min(left, sizeof(zeros));
    if (f.write(zeros, n) != n) return false;
    left -= n;
  }
  return true;
}

inline bool readFootnoteHref(HalFile& f, std::string& href) {
  href.clear();
  char chunk[32];
  bool ended = false;
  for (size_t done = 0; done < FOOTNOTE_HREF_LEN; done += sizeof(chunk)) {
    if (f.read(chunk, sizeof(chunk)) != static_cast<int>(sizeof(chunk))) return false;
    if (ended) continue;
    const size_t n = strnlen(chunk, sizeof(chunk));
    href.append(chunk, n);
    if (n < sizeof(chunk)) ended = true;
  }
  return true;
}
