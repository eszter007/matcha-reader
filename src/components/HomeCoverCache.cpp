#include "HomeCoverCache.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>

#include <algorithm>

#include "UITheme.h"

namespace fui = freeink::ui;

void HomeCoverCache::begin() {
  // Cover regions do not overlap. Each may widen by one physical byte per row.
  // Boards without PSRAM run the grid uncached: paint() re-decodes each thumb instead of
  // restoring it. Not an error -- don't attempt the allocation there at all, so the C3 boards
  // don't log a failure on every Home entry.
  if (HalMemory::getPsramHeap().totalBytes == 0) {
    coverCacheCapacity = 0;
    return;
  }
  coverCacheCapacity = renderer.getRegionByteSize(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()) +
                       MAX_COVERS * std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
  coverCache = HalMemory::allocatePsram(coverCacheCapacity);
  if (!coverCache) {
    LOG_DBG("HOME", "PSRAM cover cache unavailable (%u bytes); rendering uncached", unsigned(coverCacheCapacity));
    coverCacheCapacity = 0;
  }
}

void HomeCoverCache::invalidate() {
  coverCacheUsed = 0;
  cachedCovers.fill(CachedCover{});
}

void HomeCoverCache::invalidate(size_t index) {
  if (index < cachedCovers.size()) cachedCovers[index].valid = false;
}

void HomeCoverCache::prepare() {
  const int orientation = static_cast<int>(renderer.getOrientation());
  // Night mode stores images pre-inverted, so a snapshot from the other
  // polarity would paste back as a negative.
  const bool inverted = display.isInverted();
  if (coverCacheOrientation != orientation || coverCacheInverted != inverted) {
    invalidate();
    coverCacheOrientation = orientation;
    coverCacheInverted = inverted;
  }
}

bool HomeCoverCache::paint(fui::Rect rect, size_t index, const std::string& path, const std::string& title,
                           const int progressPercent) {
  if (index >= cachedCovers.size()) return false;
  auto& cached = cachedCovers[index];
  uint8_t* const cacheData = coverCache.get();
  if (coverCache && cached.valid && cached.rect.x == rect.x && cached.rect.y == rect.y &&
      cached.rect.width == rect.width && cached.rect.height == rect.height &&
      renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, cacheData + cached.offset, cached.bytes)) {
    return true;
  }
  cached.valid = false;
  // The Library grid's painter: cover art or the book icon over its title, the outline, the drop
  // shadow and the progress badge. Drawn here so the PSRAM snapshot below captures all of it.
  UITheme::drawBookCover(renderer, Rect{rect.x, rect.y, rect.width, rect.height}, path, title, progressPercent);
  if (coverCache) {
    const size_t needed = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
    if (needed > cached.bytes && needed <= coverCacheCapacity - coverCacheUsed) {
      cached.offset = coverCacheUsed;
      cached.bytes = needed;
      coverCacheUsed += needed;
    }
    if (needed > 0 && needed <= cached.bytes) {
      cached.rect = rect;
      cached.valid =
          renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height, cacheData + cached.offset, cached.bytes);
    }
  }
  return true;  // The cover slot is painted, including fallback art.
}
