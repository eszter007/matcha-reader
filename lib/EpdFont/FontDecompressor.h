#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;  // One per font style (R/B/I/BI)

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffer + hot group). The glyph slab below deliberately SURVIVES
  // this: clearCache() is called as per-render pool hygiene all over the UI, while the slab's
  // whole purpose is persistence across renders.
  void clearCache();

  // Free the persistent glyph slab too (plus everything clearCache() frees). For memory-critical
  // moments only -- chapter builds, TLS setup -- where every KB of contiguous heap counts. The
  // slab re-fills lazily afterwards.
  void freeGlyphSlab();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

  // Monotonic count of glyphs that could not be served because the heap had no room for their
  // group. Sampled either side of a long build so the caller can tell that its measurements ran
  // blind -- see VerticalSection's stale-stamp path. Never reset by clearCache(), so a sample
  // pair is always comparable.
  uint32_t getStarvedGlyphCount() const { return starvedGlyphs; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // Hot group: last decompressed group (byte-aligned) for non-prewarmed fallback path.
  // Kept in byte-aligned format; individual glyphs are compacted on demand into hotGlyphBuf.
  // Nothrow high-water malloc buffers, NOT std::vector: getBitmap() runs on the render path,
  // and under -fno-exceptions a vector resize that hits OOM abort()s the firmware instead of
  // failing (field crash: hotGroup.resize() -> std::bad_alloc -> abort with ~11 KB free).
  // ensureCapacity() returns false on OOM so the caller can skip the glyph gracefully.
  const EpdFontData* hotGroupFont = nullptr;
  uint16_t hotGroupIndex = UINT16_MAX;
  // Raw malloc'd buffer (not std::vector): group.uncompressedSize can run up to ~64KB and this is
  // reallocated on every cache-miss group swap. With -fno-exceptions, vector::resize()'s internal
  // operator new aborts the process on OOM instead of failing gracefully -- malloc + null-check lets
  // us log and degrade instead of crashing. Capacity is retained across swaps to avoid a malloc/free
  // cycle on every glyph lookup that lands in a different group.
  uint8_t* hotGroup = nullptr;  // owned; freed in freeHotGroup()/dtor
  uint32_t hotGroupCapacity = 0;

  // Scratch buffer for compacting a single glyph from the hot group.
  // Valid until the next getBitmap() call. Same ownership/OOM contract as hotGroup.
  uint8_t* hotGlyphBuf = nullptr;
  uint32_t hotGlyphBufCapacity = 0;

  // Back-off latch for a failed hot-group allocation. Without it, a heap too tight for one group
  // is re-probed once per glyph: a device log of a vertical chapter build showed 203 consecutive
  // "Failed to allocate 16357 bytes" over 6.8 seconds, each one a free()+malloc()+LOG_ERR that
  // could not have succeeded. The state is RAM-only and self-clearing -- a retry is allowed the
  // moment the largest block grows past what was available when we failed, or a smaller group is
  // asked for -- so a transient shortage is never recorded as a permanent "this font is
  // unavailable". Cleared outright whenever the buffers are released.
  uint32_t hotGroupFailNeeded = 0;    // size that failed; 0 = no latch
  uint32_t hotGroupFailMaxAlloc = 0;  // largest block at the time of that failure
  uint32_t starvedGlyphs = 0;         // glyphs returned as nullptr for want of a group buffer

  // Grow (never shrink) an owned buffer to at least `needed` bytes; false on OOM, buffer freed.
  static bool ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed);

  // Persistent glyph-bitmap slab: compacted bitmaps of glyphs served via the hot-group fallback,
  // keyed by (fontData, glyphIndex). Makes REPEAT renders of the same non-Latin UI text (cursor
  // navigation with the UI language set to Japanese, Japanese file names) hit RAM instead of
  // re-decompressing a ~64KB group per glyph. Lazily allocated on first use; reset generationally
  // when full; freed only by freeGlyphSlab()/deinit(). Prewarmed glyphs never land here (the page
  // slots answer first), so during reading it only ever holds stray fallback glyphs.
  static constexpr uint32_t SLAB_BYTES = 24 * 1024;
  static constexpr uint16_t SLAB_MAX_ENTRIES = 256;
  struct SlabEntry {
    const EpdFontData* fontData;
    uint32_t glyphIndex;
    uint32_t offset;  // into slabBuf
  };
  uint8_t* slabBuf = nullptr;
  SlabEntry* slabEntries = nullptr;
  uint16_t slabEntryCount = 0;
  uint32_t slabUsed = 0;

  const uint8_t* slabLookup(const EpdFontData* fontData, uint32_t glyphIndex) const;
  const uint8_t* slabInsert(const EpdFontData* fontData, uint32_t glyphIndex, const uint8_t* data, uint32_t len);

  void freePageBuffer();
  void freeHotGroup();
  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};
