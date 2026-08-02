#pragma once

#include <Epub.h>
#include <Logging.h>

#include <optional>

#include "ProgressFile.h"

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount, int8_t verticalOverride,
                         int8_t furiganaOverride, uint8_t pageBasedPercent = 0xFF,
                         std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  // data[8]: page-based book percent (0-100) for the Library/Home labels; 0xFF = unknown,
  // readers then fall back to the legacy byte-weighted computation. Old 8-byte files stay valid.
  // data[9..12]: content-based visible-text offset (upstream #2805), written only when known;
  // presence is signaled by file size >= 13. Kept AFTER the fork's vertical/furigana/percent
  // bytes so older fork progress files parse unchanged.
  uint8_t data[13];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  data[6] = static_cast<uint8_t>(verticalOverride);
  data[7] = static_cast<uint8_t>(furiganaOverride);
  data[8] = pageBasedPercent;
  size_t dataSize = 9;
  if (visibleTextOffset.has_value()) {
    data[9] = *visibleTextOffset & 0xFF;
    data[10] = (*visibleTextOffset >> 8) & 0xFF;
    data[11] = (*visibleTextOffset >> 16) & 0xFF;
    data[12] = (*visibleTextOffset >> 24) & 0xFF;
    dataSize = sizeof(data);
  }
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, dataSize)) {
    return false;
  }
  // offsetKnown distinguishes "no offset available" from a real chapter-start offset of 0.
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d vertical=%d furigana=%d offsetKnown=%d offset=%u", spineIndex,
          pageNumber, verticalOverride, furiganaOverride, visibleTextOffset.has_value() ? 1 : 0,
          visibleTextOffset.value_or(0));
  return true;
}

}  // namespace EpubReaderUtils
