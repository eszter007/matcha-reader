#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <map>
#include <string>

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
    void release() { active_ = false; }
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
  std::string scanText_;
  uint32_t scanStyleCounts_[4] = {};
  int scanFontId_ = -1;
};
