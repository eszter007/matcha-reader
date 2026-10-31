#pragma once

#include <Arduino.h>
#ifdef BOARD_HAS_PSRAM
#include <HalMemory.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>

// How much of a paragraph the vertical layout sees at once, and where its buffers come from.
namespace vertical_batch {

// The layout cannot see past one streamed batch (a word or a bracket split across the boundary
// is set without its other half). Boards with PSRAM stream 4x more text per batch: the text and
// stream buffers grow past 4 KB and are placed in PSRAM. The run count is not scaled: each run's
// strings are small blocks that stay in internal heap. 4x keeps a batch at half of the 16 KB
// paragraph limit, which would otherwise cut long paragraphs into columns.
#ifdef BOARD_HAS_PSRAM
inline constexpr size_t kScale = 4;
#else
inline constexpr size_t kScale = 1;
#endif

// Allocations from this size up go to PSRAM on PSRAM boards (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL).
inline constexpr size_t kPsramThreshold = 4096;

// The largest block an allocation of `bytes` can get: internal heap, or PSRAM for one large
// enough to be placed there. ESP.getMaxAllocHeap() alone counts internal heap only.
inline uint32_t largestBlockFor(const size_t bytes) {
#ifdef BOARD_HAS_PSRAM
  if (bytes >= kPsramThreshold) {
    return std::max<uint32_t>(ESP.getMaxAllocHeap(),
                              static_cast<uint32_t>(HalMemory::getPsramHeap().largestBlockBytes));
  }
#else
  (void)bytes;
#endif
  return ESP.getMaxAllocHeap();
}

}  // namespace vertical_batch
