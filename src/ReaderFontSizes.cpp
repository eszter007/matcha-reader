#include "ReaderFontSizes.h"

#include <algorithm>
#include <iterator>

std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName,
                                          const SdCardFontFamilyInfo* const* standIns, const size_t standInCount) {
  std::vector<uint8_t> sizes;
  size_t extra = 0;
  for (size_t i = 0; i < standInCount; i++) {
    if (standIns[i]) extra += standIns[i]->files.size();
  }

  const SdCardFontFamilyInfo* own = nullptr;
  if (registry && sdFamilyName && sdFamilyName[0] != '\0') own = registry->findFamily(sdFamilyName);
  if (own && !own->files.empty()) {
    // Copied element-wise, not move-assigned: assigning the temporary would swap in its buffer
    // and throw away the reservation the stand-in loop below depends on.
    const std::vector<uint8_t> ownSizes = own->availableSizes();
    sizes.reserve(ownSizes.size() + extra);
    sizes.assign(ownSizes.begin(), ownSizes.end());
  } else {
    sizes.reserve(std::size(BUILTIN_READER_POINT_SIZES) + extra);
    sizes.assign(std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES));
  }

  for (size_t i = 0; i < standInCount; i++) {
    if (!standIns[i]) continue;
    const auto& files = standIns[i]->files;
    std::transform(files.begin(), files.end(), std::back_inserter(sizes),
                   [](const SdCardFontFileInfo& file) { return file.pointSize; });
  }
  // A family lists one file per (size, style), so its own sizes repeat before this runs.
  std::sort(sizes.begin(), sizes.end());
  sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
  return sizes;
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
