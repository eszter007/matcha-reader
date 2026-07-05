#include "EpubProgressUtil.h"

#include <HalStorage.h>

#include <algorithm>

namespace EpubProgress {

int percentFromCache(const std::string& cachePath, const char* logTag) {
  HalFile f;
  if (!Storage.openFileForRead(logTag, cachePath + "/progress.bin", f)) return -1;
  uint8_t data[6];
  if (f.read(data, 6) != 6) return -1;
  f.close();
  const uint16_t spineIndex = data[0] | (data[1] << 8);
  const uint16_t currentPage = data[2] | (data[3] << 8);
  const uint16_t pageCount = data[4] | (data[5] << 8);
  if (pageCount == 0) return 0;
  const float sectionProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);

  HalFile bookFile;
  if (Storage.openFileForRead(logTag, cachePath + "/book.bin", bookFile)) {
    uint8_t version;
    uint32_t lutOffset;
    uint16_t spineCount;
    if (bookFile.read(&version, 1) == 1) {
      bookFile.read(reinterpret_cast<uint8_t*>(&lutOffset), 4);
      bookFile.read(reinterpret_cast<uint8_t*>(&spineCount), 2);
      if (spineCount > 0 && spineIndex < spineCount) {
        // Byte-weighted progress, same math as Epub::calculateProgress(). book.bin stores the
        // cumulative size on each spine entry: a u32 LUT of entry offsets at lutOffset; each
        // entry is a u32-length-prefixed href followed by u32 cumulativeSize. Three targeted
        // seeks fetch everything.
        auto readCumulativeSize = [&bookFile, lutOffset](const uint16_t idx, uint32_t& out) {
          uint32_t entryOffset = 0;
          if (!bookFile.seek(lutOffset + idx * 4u)) return false;
          if (bookFile.read(reinterpret_cast<uint8_t*>(&entryOffset), 4) != 4) return false;
          if (!bookFile.seek(entryOffset)) return false;
          uint32_t hrefLen = 0;
          if (bookFile.read(reinterpret_cast<uint8_t*>(&hrefLen), 4) != 4) return false;
          if (!bookFile.seek(entryOffset + 4u + hrefLen)) return false;
          return bookFile.read(reinterpret_cast<uint8_t*>(&out), 4) == 4;
        };
        uint32_t totalSize = 0, curCumulative = 0, prevCumulative = 0;
        if (readCumulativeSize(spineCount - 1, totalSize) && totalSize > 0 &&
            readCumulativeSize(spineIndex, curCumulative) &&
            (spineIndex == 0 || readCumulativeSize(spineIndex - 1, prevCumulative)) &&
            curCumulative >= prevCumulative) {
          const float bytesRead = static_cast<float>(prevCumulative) +
                                  sectionProgress * static_cast<float>(curCumulative - prevCumulative);
          const int pct = static_cast<int>(bytesRead / static_cast<float>(totalSize) * 100.0f + 0.5f);
          return std::clamp(pct, 0, 100);
        }
      }
      if (spineCount > 0) {
        // book.bin readable but LUT walk failed: equal-weight approximation.
        const float overall = (static_cast<float>(spineIndex) + sectionProgress) / static_cast<float>(spineCount);
        return std::clamp(static_cast<int>(overall * 100.0f + 0.5f), 0, 100);
      }
    }
  }

  // No usable book.bin: section-only progress.
  return std::clamp(static_cast<int>(sectionProgress * 100.0f + 0.5f), 0, 100);
}

}  // namespace EpubProgress
