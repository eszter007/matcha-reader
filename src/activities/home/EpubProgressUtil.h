#pragma once

#include <string>

// Whole-book progress percentage for an EPUB from its cache directory alone (progress.bin +
// book.bin) -- no Epub load. Weighted by spine item BYTE SIZES, matching the reader's
// Epub::calculateProgress(), so the Library/Home number agrees with the percentage shown
// inside the book. (The previous equal-weight-per-spine-item approximation lagged well behind
// in books with many tiny front-matter items and few huge chapters -- typical Japanese EPUBs.)
//
// Returns -1 when progress.bin is missing/unreadable (caller decides how to present "never
// opened"). Falls back to the equal-weight approximation if book.bin's LUT can't be read, and
// to section-only progress if book.bin is missing entirely.
namespace EpubProgress {
int percentFromCache(const std::string& cachePath, const char* logTag);
}
