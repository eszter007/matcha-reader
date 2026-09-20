#pragma once

#include <string>

#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 256
// The href's fixed width is the ON-DISK field only. In RAM it is a std::string: a
// 288-byte fixed record made every footnote container a large-element vector, and the
// section-wide table (128 entries) then needed 37KB *contiguous* to grow into -- which
// aborts through operator new once a reading session has fragmented the heap. As a
// string the record is ~36 bytes, so the same table wants ~4.6KB contiguous plus one
// small allocation per href.
// The fixed-width read/write helpers live in FootnoteHrefIo.h.
static_assert(FOOTNOTE_HREF_LEN % 32 == 0, "href padding is written/read in 32-byte chunks");

struct FootnoteEntry {
  char number[FOOTNOTE_NUMBER_LEN];
  std::string href;

  FootnoteEntry() { number[0] = '\0'; }
};
