#pragma once

#include <HalStorage.h>  // HalFile (kept open for streamed TTFs)
#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>
#include <VectorFontSupport.h>

#if CROSSPOINT_VECTOR_FONTS
#include <FontPsram.h>  // PsramVector for resident TTF bytes
#endif

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class EpdFontFamily;
class GfxRenderer;
class TtfEpdFont;

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  // Constructor and destructor are out-of-line (defined in the .cpp where
  // TtfEpdFont is a complete type) so the std::unique_ptr<TtfEpdFont> member
  // can be constructed/destroyed with only a forward declaration visible here.
  SdCardFontSystem();
  ~SdCardFontSystem();
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

  /// Release every resident SD font -- the selected family, its companion fallback, their
  /// size-matched UI fallback registrations, and the glyph slabs FontCacheManager holds for
  /// them. For any screen that needs a large allocation and does not render book text: manga
  /// JPEG/PNG decoders (a 36-60 KB block) and the WiFi-backed font catalog (esp_wifi_init plus
  /// two TLS sessions) both call it. Strictly more than FontCacheManager::releaseAllFontMemory(),
  /// which frees the glyph slabs but leaves the SdCardFont objects themselves allocated. The
  /// saved selection is kept, and so is the JP-fallback policy; ensureLoaded() restores both when
  /// text rendering is needed again. To drop the Japanese companion for good, the caller says so
  /// with setJpFallbackNeeded(renderer, false) -- releasing memory does not decide policy.
  void releaseAllResidentFonts(GfxRenderer& renderer);

  /// Font ID of the loaded companion/fallback font (0 when none). See effective-reader-font
  /// substitution in EpubReaderActivity: when the SELECTED font can't carry a book's primary
  /// script, the companion becomes the reader font for that book so all layout and vertical
  /// positioning derives from a font that actually contains the glyphs.
  int companionFontId() const;

  /// The font a book of this script actually renders with: the selected face when it can carry
  /// the script, and the substitute when it cannot -- the companion for a Japanese book whose
  /// font has no CJK, the built-in serif for a Latin book whose font has no Latin (a CJK-only
  /// family; the companion is chosen for Japanese and would set an English book in a Japanese
  /// face). EVERY site that renders book text must ask this rather than getReaderFontId():
  /// layout, drawing and the settings preview alike, or they disagree about both face and size.
  int effectiveReaderFontId(bool jpBook) const;

  /// True when the currently selected reader font covers the codepoint. Built-in fonts are
  /// treated as Latin-complete and CJK-less (their CJK subset is a degraded fallback, not
  /// proper coverage).
  bool selectedFontCovers(uint32_t cp) const;

  /// True for SD families that are the CJK extension of a built-in family (NotoSansJP,
  /// NotoSerifJP): hidden from font pickers and used automatically as the Japanese glyph
  /// fallback instead of being selected directly.
  static bool isBuiltinJpExtension(const std::string& familyName);

  /// Families hidden from the picker that can nonetheless end up rendering the row named by
  /// `sdFamilyName` (empty for the built-in family `fontFamily`): the coverage variant that
  /// stands in for it (resolveSelectedFamily), and on a built-in row the JP companion that
  /// carries a Japanese book (ensureJpFallback + EpubReaderActivity::effectiveReaderFontId).
  /// Their installed sizes are therefore selectable on that row -- see readerFontPointSizes().
  ///
  /// Writes up to `cap` entries into `out` and returns how many. Static and registry-driven so
  /// the settings UI can ask without owning a font system.
  static constexpr uint8_t MAX_STAND_INS = 2;
  static uint8_t readerStandInFamilies(const SdCardFontRegistry* registry, const char* sdFamilyName, uint8_t fontFamily,
                                       const SdCardFontFamilyInfo** out, uint8_t cap);

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
  /// Unload what ensureWordLookupFallback() loaded, when the lookup session ends. A font loaded
  /// mid-session lands at the top of a busy heap; left resident it caps the largest free block
  /// for the rest of the reading session (measured: 51 KB -> 31 KB after one lookup).
  void releaseWordLookupFallback(GfxRenderer& renderer);

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
  static const SdCardFontFamilyInfo* findCoverageVariant(const SdCardFontRegistry* registry,
                                                         const std::string& baseName);

  /// Point sizes the current reader row offers: its own family's sizes widened by its
  /// stand-ins'. The same set the pickers show, so a size the user can select is never snapped
  /// away by a load.
  std::vector<uint8_t> rowPointSizes() const;

  /// Family that should be resident for the current selection and reading context. Empty means
  /// "the built-in reader font, nothing to load". This is a runtime substitution only —
  /// SETTINGS.sdFontFamilyName keeps naming what the user actually picked.
  std::string resolveSelectedFamily() const;
  // True when the named face ships `pt` exactly (empty name = a built-in family). Drives the
  // stand-in choice in resolveSelectedFamily(): a size only a stand-in has must render with it.
  bool faceShipsSize(const std::string& familyName, uint8_t pt) const;

  /// Below this largest-free-block figure, ensureJpFallback() drops the glyph caches before
  /// loading the companion. Set above the biggest single block that load asks for -- a broad CJK
  /// face's interval table at a large point size, measured at 26,592 B for NotoSansJP 20 -- so
  /// the release happens while it can still help rather than after the failure.
  static constexpr uint32_t COMPANION_LOAD_HEADROOM = 40 * 1024;

  void ensureJpFallback(GfxRenderer& renderer, uint8_t pointSize);
  void updateGlobalFallback(GfxRenderer& renderer);
  bool loadedFamilyCovers(const SdCardFontManager& mgr, const std::string& name, uint32_t cp) const;

  SdCardFontManager fallbackManager_;
  const EpdFontFamily* defaultGlobalFallback_ = nullptr;
  bool jpFallbackNeeded_ = false;
  // Load the active SD family at the built-in UI point sizes and register each
  // as a size-matched script fallback for the corresponding UI font, so book
  // titles/list rows in scripts the built-ins lack (CJK, Greek, Cyrillic, ...)
  // render at the same size as the surrounding Latin UI text. No-op when no SD
  // family is loaded. Safe to call repeatedly (sizes already loaded are
  // reused).
  void setupUiFallbacks(GfxRenderer& renderer);

#if CROSSPOINT_VECTOR_FONTS
  // --- Vector (.ttf/.otf) font path (FreeInkFont via TtfEpdFont) -------------
  // Load/refresh the selected TTF family at the current reader size, register
  // it with the renderer, and track it so ensureSdCardFontReady() rebuilds its
  // glyph set per page. registryWasDirty forces a reload even if unchanged.
  void loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, bool registryWasDirty);
  // Unregister + free the active TTF font (and its UI-size fallbacks), if any.
  void unloadTtf(GfxRenderer& renderer);
  // Register the loaded TTF at each built-in UI size as a script fallback, so UI
  // text (book titles, list rows, menus, status bar) in scripts the built-in
  // fonts lack renders in the chosen TTF. Mirrors setupUiFallbacks for .cpfont.
  void setupTtfUiFallbacks(GfxRenderer& renderer);
  // Open one style source file (resident if small, streamed if large) into
  // ttfSources_[style]. Returns false on open/read failure.
  bool openTtfSource(uint8_t style, const std::string& path);
  // Register every present source with `font` (shared bytes / file handles).
  void addTtfSources(TtfEpdFont& font);
  // Close/free all style sources.
  void freeTtfSources();
  // ReadFn for streamed sources: serves the PSRAM prefix cache first, SD after.
  static unsigned long prefixRead(void* ctx, unsigned long offset, unsigned char* buffer, unsigned long count);
#endif  // CROSSPOINT_VECTOR_FONTS

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  // Fonts ensureWordLookupFallback() itself loaded (not ones it found resident), with the
  // built-in font each stands in for. Two: the definition body and the panel header.
  struct LookupExtra {
    int primaryFontId = 0;
    int sdFontId = 0;
  };
  LookupExtra lookupExtras_[2];
  std::atomic<bool> registryDirty_{false};

#if CROSSPOINT_VECTOR_FONTS
  // One style source file. SMALL files are read fully into `bytes` (resident,
  // PSRAM when present); LARGE files stream from `file` (kept open) so a multi-MB
  // file never sits in RAM. All faces (reader + UI sizes) share these sources.
  struct TtfSource {
    // Resident form: the whole file. Streamed form: a PSRAM prefix cache of the
    // file head (cmap/loca/hmtx) — empty when PSRAM couldn't fund it.
    freeink::font::PsramVector<uint8_t> bytes;
    HalFile file;  // open handle (streamed form)
    bool streamed = false;
    unsigned long size = 0;
    bool present = false;
  };

  // Active TTF font (at most one reader-size vector family loaded at a time).
  std::unique_ptr<TtfEpdFont> ttf_;
  // Up to 4 style sources: 0=regular (required), 1=bold, 2=italic, 3=bold-italic.
  TtfSource ttfSources_[4];
  std::string ttfFamily_;     // loaded vector family name ("" = none)
  int ttfFontId_ = 0;         // renderer font id for ttf_ (0 = none)
  uint8_t ttfPointSize_ = 0;  // size ttf_ was built at
  // UI-size TTF fallbacks (share ttfSources_); parallel to their renderer font ids.
  std::vector<std::unique_ptr<TtfEpdFont>> ttfUi_;
  std::vector<int> ttfUiIds_;
#endif  // CROSSPOINT_VECTOR_FONTS
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
