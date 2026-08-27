#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>

class FontDecompressor;
class SdCardFont;

class FontCacheManager {
 public:
  FontCacheManager(const std::map<int, EpdFontFamily>& fontMap, const std::map<int, SdCardFont*>& sdCardFonts);

  void setFontDecompressor(FontDecompressor* d);

  void clearCache();
  // clearCache() plus the FontDecompressor's persistent glyph slab (~24KB). For memory-critical
  // moments (chapter builds, image extraction, TLS setup) where contiguous heap matters more
  // than warm glyphs; the slab re-fills lazily afterwards. Ordinary per-render cache hygiene
  // should keep calling clearCache() so non-Latin UI navigation stays fast.
  void releaseAllFontMemory();
  void prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F);
  // True if fontId is backed by an SD-card font (SdCardFont::prewarm(), one-open bulk-load path)
  // rather than a built-in compressed font (FontDecompressor's own group-cache prewarm, which has
  // a separate, much more limited concurrent-prewarm-buffer budget -- see prewarmCache() callers
  // that need to avoid competing with normal rendering's own use of that path).
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }

  // Companion/fallback SD font (nullptr when none): prewarmCache() warms it with the
  // codepoints the requested font can't cover, so rendering fallback glyphs doesn't hit the
  // per-glyph on-demand SD loader on every page turn.
  void setFallbackSdFont(SdCardFont* font) { fallbackSdFont_ = font; }
  void logStats(const char* label = "render");
  void resetStats();

  // Scan-mode API: called by GfxRenderer::drawText() during scan pass
  bool isScanning() const;
  void recordText(const char* text, int fontId, EpdFontFamily::Style style);

  // The FontDecompressor pointer, needed by GfxRenderer::getGlyphBitmap()
  FontDecompressor* getDecompressor() const { return fontDecompressor_; }

  // RAII scope for two-pass prewarm pattern
  class PrewarmScope {
   public:
    explicit PrewarmScope(FontCacheManager& manager);
    ~PrewarmScope();
    void endScanAndPrewarm();
    // Keep the warmed glyphs resident after this scope ends instead of clearing them on
    // destruction. Call after endScanAndPrewarm() when the warm is meant to outlive the
    // scope -- e.g. the idle next-page prewarm, which warms a page the reader will only turn
    // to later. Ordinary single-render scopes do NOT release, so they clear (warm-for-one-
    // render) and keep the cache honest for the next scan.
    //
    // Finalizes the scan first: calling release() before endScanAndPrewarm() would otherwise
    // strand the manager in scan mode (drawText would keep recording instead of drawing) and
    // never prewarm. endScanAndPrewarm() is a no-op when the scan was already ended.
    void release() {
      endScanAndPrewarm();
      active_ = false;
    }
    PrewarmScope(PrewarmScope&& other) noexcept;
    PrewarmScope& operator=(PrewarmScope&&) = delete;
    PrewarmScope(const PrewarmScope&) = delete;
    PrewarmScope& operator=(const PrewarmScope&) = delete;

   private:
    FontCacheManager* manager_;
    bool active_ = true;
  };
  PrewarmScope createPrewarmScope();

 private:
  SdCardFont* fallbackSdFont_ = nullptr;
  const std::map<int, EpdFontFamily>& fontMap_;
  const std::map<int, SdCardFont*>& sdCardFonts_;
  FontDecompressor* fontDecompressor_ = nullptr;

  enum class ScanMode : uint8_t { None, Scanning };
  ScanMode scanMode_ = ScanMode::None;

  // A render pass touches at most a handful of font ids. Codepoints are packed
  // with a compact font slot and resolved style, then grouped for prewarming.
  static constexpr uint8_t MAX_SCAN_FONTS = 4;
  static constexpr uint16_t MAX_SCAN_CODEPOINTS = 512;
  static constexpr uint8_t SCAN_STYLE_SHIFT = 21;
  static constexpr uint8_t SCAN_FONT_SHIFT = SCAN_STYLE_SHIFT + 2;
  static constexpr uint32_t SCAN_CODEPOINT_MASK = (1U << SCAN_STYLE_SHIFT) - 1;
  static constexpr uint8_t SCAN_GROUP_COUNT = MAX_SCAN_FONTS * 4;

  uint8_t resolveScanStyle(int fontId, EpdFontFamily::Style style) const;
  int scanFontIds_[MAX_SCAN_FONTS] = {};
  uint32_t scanCodepoints_[MAX_SCAN_CODEPOINTS + 1] = {};
  uint16_t scanGroupCounts_[SCAN_GROUP_COUNT] = {};
  uint16_t scanCodepointCount_ = 0;
  uint8_t scanFontCount_ = 0;
  bool scanOverflowWarned_ = false;
};
