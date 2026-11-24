#pragma once

#include <Epub.h>
#include <Epub/PageLink.h>
#include <Logging.h>

#include <optional>
#include <vector>

#include "ProgressFile.h"

namespace EpubReaderUtils {

// progress.bin byte 7 holds the book's furigana choices. Low nibble: the on/off override (0xF
// unset, 0 off, 1 on). High nibble: 1 + the Furigana Size override (0 or 0xF unset). Files
// written before sizes existed hold 0xFF, 0 or 1, which read the same as before.
inline int8_t packFurigana(const int8_t on, const int8_t size) {
  const uint8_t low = on < 0 ? 0x0F : static_cast<uint8_t>(on ? 1 : 0);
  const uint8_t high = size >= 0 ? static_cast<uint8_t>(size + 1) : (on < 0 ? 0x0F : 0);
  return static_cast<int8_t>(static_cast<uint8_t>((high << 4) | low));
}
inline void unpackFurigana(const uint8_t stored, int8_t& on, int8_t& size) {
  const uint8_t low = stored & 0x0F;
  const uint8_t high = stored >> 4;
  on = low == 0x0F ? -1 : static_cast<int8_t>(low != 0 ? 1 : 0);
  size = high >= 1 && high <= 4 ? static_cast<int8_t>(high - 1) : -1;
}

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

inline const PageLink* linkAtPoint(const std::vector<PageLink>& links, const int x, const int y, const int marginLeft,
                                   const int marginTop) {
  // Finger slop, plus a floor on the target width: a note marker is often a single superscript
  // digit only a few pixels wide. The box is never grown vertically beyond its own line, so
  // taps on the lines above and below still reach the page-turn zones.
  constexpr int TOUCH_SLOP = 6;
  constexpr int MIN_TOUCH_WIDTH = 28;
  const int pageX = x - marginLeft;
  const int pageY = y - marginTop;
  for (const auto& link : links) {
    if (link.contains(pageX, pageY, TOUCH_SLOP, MIN_TOUCH_WIDTH)) {
      return &link;
    }
  }
  return nullptr;
}

}  // namespace EpubReaderUtils
