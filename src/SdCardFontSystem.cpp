#include "SdCardFontSystem.h"

#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <algorithm>
#include <cctype>
#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

// Point the reader font size at a size the given family actually ships, and
// persist the change so the settings UI and the loaded font never disagree.
// Guarded by the value-change check: a no-op snap must not write SPIFFS.
void snapFontPointSizeTo(const uint8_t availablePointSize) {
  if (availablePointSize == 0 || availablePointSize == SETTINGS.fontPointSize) return;
  LOG_DBG("SDFS", "Font size %u unavailable, snapping to %u", SETTINGS.fontPointSize, availablePointSize);
  SETTINGS.fontPointSize = availablePointSize;
  SETTINGS.saveToFile();
}

// Built-in UI fonts and their physical point sizes (at 150 DPI, matching the
// SD-font converter). Each is paired with a same-size SD fallback so CJK UI
// text matches the surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  // The built-in jōyō-subset fallback installed by main.cpp -- the floor we return to when
  // no CJK-capable SD font is available.
  defaultGlobalFallback_ = EpdFontFamily::getGlobalFallback();
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
        snapFontPointSizeTo(manager_.currentPointSize());
        setupUiFallbacks(renderer);
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.clearSdFontFamily();
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.clearSdFontFamily();
    }
  }

  ensureJpFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  ensureSelectedLoaded(renderer);
  ensureJpFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);
}

void SdCardFontSystem::ensureSelectedLoaded(GfxRenderer& renderer) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  // A JP extension family must never be the SELECTED reader font: it is the
  // Japanese half of a built-in Noto entry and is hidden from both pickers, so a
  // selection carried over from an older build would be stuck and would render
  // Latin books in the JP face. Revert it to the matching built-in and let
  // ensureJpFallback() bring the extension back as the companion where needed.
  if (SETTINGS.sdFontFamilyName[0] != '\0' && isBuiltinJpExtension(SETTINGS.sdFontFamilyName)) {
    std::string norm;
    for (const char* c = SETTINGS.sdFontFamilyName; *c; ++c) {
      if (std::isalnum(static_cast<unsigned char>(*c))) norm.push_back(static_cast<char>(std::tolower(*c)));
    }
    SETTINGS.fontFamily = norm == "notoserifjp" ? CrossPointSettings::NOTOSERIF : CrossPointSettings::NOTOSANS;
    LOG_INF("SDFS", "Reverting hidden JP extension selection '%s' to built-in", SETTINGS.sdFontFamilyName);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;

  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family, which exists only at BUILTIN_READER_POINT_SIZES:
    // a size inherited from an SD family has to come back into that set.
    snapFontPointSizeTo(snapToNearestPointSize(BUILTIN_READER_POINT_SIZES, std::size(BUILTIN_READER_POINT_SIZES),
                                               SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship.
    snapFontPointSizeTo(wantedPt);
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  // Free the JP fallback font BEFORE loading the newly selected family: two SD fonts' interval
  // and kern tables don't reliably coexist on this heap (UDDigiKyokasho's sparse-coverage
  // interval table is the known worst case), and a failed load silently clears the user's
  // selection. ensureJpFallback() re-establishes the fallback afterwards if still needed.
  if (!fallbackManager_.currentFamilyName().empty()) {
    fallbackManager_.unloadAll(renderer);
  }
  // Under fragmentation, hand the font decompressor's buffers to the load as well.
  if (ESP.getMaxAllocHeap() < 32 * 1024) {
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
      snapFontPointSizeTo(manager_.currentPointSize());
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.clearSdFontFamily();
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on CJK codepoints, so a Latin-only family
  // can never act as a fallback and its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script: Han, Hiragana, Katakana, Hangul.
  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
      // ...and give that SD font the built-in family of the SAME size as its own next stop.
      // Redirecting a string here is all-or-nothing, so whatever the SD font lacks (a CJK-only
      // family like UDDigiKyokasho has no Latin) would otherwise fall through to the global
      // fallback -- the companion loaded at the READER's point size. Device case: a 12pt Home
      // title drew its kanji at 12pt and the ASCII "11" beside them at 14pt from a third
      // typeface. With this, the miss lands on the matching built-in instead.
      const auto& fontMap = renderer.getFontMap();
      const auto builtinIt = fontMap.find(ui.fontId);
      if (builtinIt != fontMap.end()) {
        renderer.setFamilyFallback(sdFontId, &builtinIt->second);
      }
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  return manager_.getFontId(familyName);
}

bool SdCardFontSystem::isBuiltinJpExtension(const std::string& familyName) {
  std::string norm;
  norm.reserve(familyName.size());
  for (const char c : familyName) {
    if (std::isalnum(static_cast<unsigned char>(c))) norm.push_back(static_cast<char>(std::tolower(c)));
  }
  return norm == "notosansjp" || norm == "notoserifjp";
}

bool SdCardFontSystem::loadedFamilyCovers(const SdCardFontManager& mgr, const std::string& name,
                                          const uint32_t cp) const {
  if (mgr.currentFamilyName() != name) return false;
  const SdCardFont* font = mgr.loadedFont();
  // Coverage must come from the font FILE's full interval table -- EpdFont::hasGlyph on SD
  // fonts only answers which glyphs happen to be resident right now.
  return font && font->coversCodepoint(cp);
}

void SdCardFontSystem::ensureJpFallback(GfxRenderer& renderer, const uint8_t pointSize) {
  // Companion-font need is coverage-driven in BOTH directions:
  //  - selected font lacks Japanese and the book needs it (jpFallbackNeeded_) -> companion
  //  - selected font lacks LATIN (UDDigiKyokasho ships cjk-ext only: English words, digits
  //    and UI text would render blank) -> companion, regardless of book language
  // The JP extension fonts (NotoSansJP/NotoSerifJP, latin-ext + cjk-ext) cover both holes.
  const std::string& selected = manager_.currentFamilyName();
  const bool selectedHasCjk = !selected.empty() && loadedFamilyCovers(manager_, selected, 0x3042);
  const bool selectedHasLatin = selected.empty()  // built-ins always have Latin
                                    ? true
                                    : loadedFamilyCovers(manager_, selected, 'a');
  // Only load a companion for a book that actually needs Japanese. A Latin book read
  // with a CJK-only family (UDDigiKyokasho) does NOT: the reader already substitutes
  // the built-in Noto Serif/Sans for it (effectiveReaderFontId). Loading a companion
  // anyway sets fallbackSdFont_ and redirects the global fallback to a JP family,
  // which then prices/draws the built-in font's glyphs -- collapsing the word spaces
  // and making Latin text render as if it were Japanese. jpFallbackNeeded_ is the
  // book-level signal; within a Japanese book a companion still covers either hole
  // (no CJK in the selected font, or no Latin for embedded English).
  const bool needsCompanion = jpFallbackNeeded_ && (!selectedHasCjk || !selectedHasLatin);
  if (!needsCompanion) {
    if (!fallbackManager_.currentFamilyName().empty()) fallbackManager_.unloadAll(renderer);
    return;
  }

  // Selected font (built-in, or a Latin-only SD font) can't render Japanese: load the best
  // CJK family from the card at the reader size. Extension families (NotoSansJP/NotoSerifJP)
  // first -- they exist exactly for this -- then any other family that proves CJK-capable
  // when loaded. When both extensions are installed, match the selected style: built-in
  // Noto Serif pairs with NotoSerifJP, everything else with NotoSansJP.
  const bool preferSerif = SETTINGS.sdFontFamilyName[0] == '\0' && SETTINGS.fontFamily == CrossPointSettings::NOTOSERIF;
  auto extensionRank = [preferSerif](const std::string& name) {
    std::string norm;
    for (const char c : name) {
      if (std::isalnum(static_cast<unsigned char>(c))) norm.push_back(static_cast<char>(std::tolower(c)));
    }
    const bool isSerifExt = norm == "notoserifjp";
    return isSerifExt == preferSerif ? 0 : 1;  // 0 = style-matched extension
  };
  std::vector<const SdCardFontFamilyInfo*> candidates;
  for (const auto& fam : registry_.getFamilies()) {
    if (fam.name == selected) continue;
    if (isBuiltinJpExtension(fam.name)) candidates.push_back(&fam);
  }
  std::sort(candidates.begin(), candidates.end(),
            [&extensionRank](const SdCardFontFamilyInfo* a, const SdCardFontFamilyInfo* b) {
              return extensionRank(a->name) < extensionRank(b->name);
            });
  for (const auto& fam : registry_.getFamilies()) {
    if (fam.name == selected || isBuiltinJpExtension(fam.name)) continue;
    candidates.push_back(&fam);
  }

  for (const auto* fam : candidates) {
    // Already loaded at the right size? Keep it.
    if (fallbackManager_.currentFamilyName() == fam->name) {
      const auto* wanted = fam->findNearestSize(pointSize);
      if (wanted && wanted->pointSize == fallbackManager_.currentPointSize()) return;
    }
    if (!fallbackManager_.loadFamily(*fam, renderer, pointSize)) continue;
    if (loadedFamilyCovers(fallbackManager_, fam->name, 0x3042) &&
        loadedFamilyCovers(fallbackManager_, fam->name, 'a')) {
      LOG_DBG("SDFS", "Companion fallback font: %s", fam->name.c_str());
      return;
    }
    // Loaded fine but doesn't cover both scripts -- not a useful companion.
    fallbackManager_.unloadAll(renderer);
  }

  if (!fallbackManager_.currentFamilyName().empty()) fallbackManager_.unloadAll(renderer);
}

void SdCardFontSystem::updateGlobalFallback(GfxRenderer& renderer) {
  // Deterministic recompute instead of save/restore bookkeeping (which broke when load/unload
  // interleaved): exactly one of three states holds at any time.
  const EpdFontFamily* target = defaultGlobalFallback_;
  const std::string& selected = manager_.currentFamilyName();
  const std::string& fallback = fallbackManager_.currentFamilyName();
  if (!fallback.empty()) {
    // A companion is loaded exactly because something (Latin or CJK) is missing from the
    // selected font -- it covers both scripts, so it is the most capable last resort.
    target = &renderer.getFontMap().at(fallbackManager_.getFontId(fallback));
  } else if (!selected.empty() && loadedFamilyCovers(manager_, selected, 0x3042) &&
             loadedFamilyCovers(manager_, selected, 'a')) {
    // Fully self-sufficient SD font: also serves rare glyphs for the built-in UI fonts.
    target = &renderer.getFontMap().at(manager_.getFontId(selected));
  }
  EpdFontFamily::setGlobalFallback(target);
  // Keep the renderer's measurement hook in sync: layout prices missing glyphs from the
  // companion's advance table instead of loading their bitmaps one by one from SD.
  SdCardFont* companion = !fallback.empty() ? fallbackManager_.loadedFont() : nullptr;
  renderer.setFallbackSdFont(companion);
  if (auto* fcm = renderer.getFontCacheManager()) fcm->setFallbackSdFont(companion);
}

void SdCardFontSystem::setJpFallbackNeeded(GfxRenderer& renderer, const bool needed) {
  if (jpFallbackNeeded_ == needed) return;
  LOG_DBG("SDFS", "JP fallback needed: %d", needed);
  jpFallbackNeeded_ = needed;
  ensureJpFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);
}

int SdCardFontSystem::companionFontId() const {
  const std::string& fallback = fallbackManager_.currentFamilyName();
  return fallback.empty() ? 0 : fallbackManager_.getFontId(fallback);
}

bool SdCardFontSystem::selectedFontCovers(const uint32_t cp) const {
  const std::string& selected = manager_.currentFamilyName();
  if (selected.empty()) {
    // Built-in reader fonts: full Latin, no proper CJK (the jōyō subset is a last resort).
    return cp < 0x2E80;
  }
  return loadedFamilyCovers(manager_, selected, cp);
}
