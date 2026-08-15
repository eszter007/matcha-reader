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

  /// Resolve an SD card font ID from family name + reader point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Declare whether the current reading context needs proper Japanese rendering (Japanese
  /// EPUB, forced vertical text, manga). The JP fallback font is only loaded while needed --
  /// opening a non-CJK book must not pay the SD font load or hold its tables in RAM.
  /// Applies immediately (loads/unloads the fallback and recomputes the global fallback).
  void setJpFallbackNeeded(GfxRenderer& renderer, bool needed);

  /// Release every resident SD font while an image-only reader owns the screen. Manga JPEG/PNG
  /// decoders need a 36-60 KB allocation and cannot coexist reliably with the selected reader
  /// font plus its size-matched UI fallbacks on the ESP32-C3 heap. The saved selection is kept;
  /// ensureLoaded() restores it when text rendering is needed again.
  void releaseForImageDecode(GfxRenderer& renderer);

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

  /// True for SD families that only widen the coverage of a family the device already offers
  /// (NotoSerifExtended over the built-in Noto Serif, PagellaIPA over an installed Pagella).
  /// The picker shows the base alone and resolveSelectedFamily() decides which of the two is
  /// resident, so one typeface is one row. A variant whose base is NOT installed stays
  /// visible: collapsing a row must never make its glyphs unreachable. `registry` is where the
  /// base is looked up; a null registry can only match the built-in bases.
  static bool isCoverageVariant(const std::string& familyName, const SdCardFontRegistry* registry);

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Lazily load the selected family's exact CJK fallback size for a native Word Lookup font.
  void ensureWordLookupFallback(GfxRenderer& renderer, int primaryFontId, uint8_t pointSize);

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

  /// Base family a coverage variant widens ("NotoSerifExtended" -> "NotoSerif"), or empty when
  /// the name carries none of the known suffixes. Does not check that the base exists.
  static std::string coverageVariantBase(const std::string& familyName);

  /// Installed variant standing in for `baseName`, or nullptr when the card has none.
  const SdCardFontFamilyInfo* findCoverageVariant(const std::string& baseName) const;

  /// Family that should be resident for the current selection and reading context. Empty means
  /// "the built-in reader font, nothing to load". This is a runtime substitution only —
  /// SETTINGS.sdFontFamilyName keeps naming what the user actually picked.
  std::string resolveSelectedFamily() const;

  void ensureJpFallback(GfxRenderer& renderer, uint8_t pointSize);
  void updateGlobalFallback(GfxRenderer& renderer);
  bool loadedFamilyCovers(const SdCardFontManager& mgr, const std::string& name, uint32_t cp) const;

  SdCardFontManager fallbackManager_;
  const EpdFontFamily* defaultGlobalFallback_ = nullptr;
  bool jpFallbackNeeded_ = false;
  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched CJK fallback for the corresponding UI font, so CJK book
  // titles/list rows render at the same size as the surrounding Latin UI text.
  // No-op when no SD family is loaded. Safe to call repeatedly (sizes already
  // loaded are reused).
  void setupUiFallbacks(GfxRenderer& renderer);

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
