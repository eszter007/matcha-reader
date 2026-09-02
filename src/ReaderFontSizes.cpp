#include "ReaderFontSizes.h"

#include <algorithm>
#include <iterator>

std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName,
                                          const SdCardFontFamilyInfo* companion) {
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') {
    if (const auto* family = registry->findFamily(sdFamilyName)) {
      auto sizes = family->availableSizes();
      if (!sizes.empty()) return sizes;
    }
  }

  std::vector<uint8_t> sizes;
  sizes.reserve(std::size(BUILTIN_READER_POINT_SIZES) + (companion ? companion->files.size() : 0));
  sizes.assign(std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES));
  if (!companion) return sizes;

  for (const auto& file : companion->files) {
    if (std::find(sizes.begin(), sizes.end(), file.pointSize) == sizes.end()) sizes.push_back(file.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

uint8_t snapToBuiltinPointSize(const SdCardFontFamilyInfo* companion, const uint8_t pt) {
  uint8_t best = snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES), pt);
  if (!companion) return best;

  uint8_t bestDelta = best > pt ? best - pt : pt - best;
  for (const auto& file : companion->files) {
    const uint8_t delta = file.pointSize > pt ? file.pointSize - pt : pt - file.pointSize;
    // Ties resolve to the smaller size, matching snapToNearestPointSize().
    if (delta < bestDelta || (delta == bestDelta && file.pointSize < best)) {
      best = file.pointSize;
      bestDelta = delta;
    }
  }
  return best;
}

uint8_t snapToNearestPointSize(const uint8_t* sizes, const size_t count, const uint8_t pt) {
  if (!sizes || count == 0) return pt;

  uint8_t best = sizes[0];
  uint8_t bestDelta = best > pt ? best - pt : pt - best;
  for (size_t i = 1; i < count; i++) {
    const uint8_t delta = sizes[i] > pt ? sizes[i] - pt : pt - sizes[i];
    // Strictly-less keeps the smaller size on a tie, since `sizes` is ascending.
    if (delta < bestDelta) {
      best = sizes[i];
      bestDelta = delta;
    }
  }
  return best;
}
