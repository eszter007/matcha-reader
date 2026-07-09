#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

class EpdFontFamily;
class GfxRenderer;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Discover SD card fonts and load user's saved selection. Call once during setup.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  /// Resolve an SD card font ID from family name + fontSize enum.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /// Declare whether the current reading context needs proper Japanese rendering (Japanese
  /// EPUB, forced vertical text, manga). The JP fallback font is only loaded while needed --
  /// opening a non-CJK book must not pay the SD font load or hold its tables in RAM.
  /// Applies immediately (loads/unloads the fallback and recomputes the global fallback).
  void setJpFallbackNeeded(GfxRenderer& renderer, bool needed);

  /// Font ID of the loaded companion/fallback font (0 when none). See effective-reader-font
  /// substitution in EpubReaderActivity: when the SELECTED font can't carry a book's primary
  /// script, the companion becomes the reader font for that book so all layout and vertical
  /// positioning derives from a font that actually contains the glyphs.
  int companionFontId() const;

  /// True when the currently selected reader font covers the codepoint. Built-in fonts are
  /// treated as Latin-complete and CJK-less (their CJK subset is a degraded fallback, not
  /// proper coverage).
  bool selectedFontCovers(uint32_t cp) const;

  /// True for SD families that are the CJK extension of a built-in family (NotoSansJP,
  /// NotoSerifJP): hidden from font pickers and used automatically as the Japanese glyph
  /// fallback instead of being selected directly.
  static bool isBuiltinJpExtension(const std::string& familyName);

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /// If the registry is dirty, re-scan the SD card now and clear the flag.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() {
    if (registryDirty_.exchange(false, std::memory_order_acquire)) {
      registry_.discover();
    }
  }

private:
  /// Keep the global glyph fallback correct for the current selection:
  ///  - selected SD font renders Japanese -> it IS the fallback (any glyph on demand)
  ///  - otherwise (built-in or Latin-only SD font) -> auto-load the best CJK family from the
  ///    card (extension families first) at the reader size and use that
  ///  - no CJK family on the card -> the built-in jōyō-subset fallback captured at begin()
  void ensureSelectedLoaded(GfxRenderer& renderer);
  void ensureJpFallback(GfxRenderer& renderer, uint8_t sizeEnum);
  void updateGlobalFallback(GfxRenderer& renderer);
  bool loadedFamilyCovers(const SdCardFontManager& mgr, const std::string& name, uint32_t cp) const;

  SdCardFontManager fallbackManager_;
  const EpdFontFamily* defaultGlobalFallback_ = nullptr;
  bool jpFallbackNeeded_ = false;

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
