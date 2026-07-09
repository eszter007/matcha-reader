#include "SdCardFontSystem.h"

#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <SdCardFont.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>

#include "CrossPointSettings.h"

namespace {

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  // The built-in jōyō-subset fallback installed by main.cpp -- the floor we return to when
  // no CJK-capable SD font is available.
  defaultGlobalFallback_ = EpdFontFamily::getGlobalFallback();
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // If user has a saved SD font selection, load it
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (manager_.loadFamily(*family, renderer, fontSizeEnumFromSettings())) {
        LOG_DBG("SDFS", "Loaded SD card font family: %s", SETTINGS.sdFontFamilyName);
      } else {
        LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", SETTINGS.sdFontFamilyName);
        SETTINGS.sdFontFamilyName[0] = '\0';
      }
    } else {
      LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  }

  ensureJpFallback(renderer, fontSizeEnumFromSettings());
  updateGlobalFallback(renderer);

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  ensureSelectedLoaded(renderer);
  ensureJpFallback(renderer, fontSizeEnumFromSettings());
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

  const char* wantedFamily = SETTINGS.sdFontFamilyName;


  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
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
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
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
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
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

void SdCardFontSystem::ensureJpFallback(GfxRenderer& renderer, const uint8_t sizeEnum) {
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
  const bool needsCompanion = !selectedHasLatin || (jpFallbackNeeded_ && !selectedHasCjk);
  if (!needsCompanion) {
    if (!fallbackManager_.currentFamilyName().empty()) fallbackManager_.unloadAll(renderer);
    return;
  }

  // Selected font (built-in, or a Latin-only SD font) can't render Japanese: load the best
  // CJK family from the card at the reader size. Extension families (NotoSansJP/NotoSerifJP)
  // first -- they exist exactly for this -- then any other family that proves CJK-capable
  // when loaded. When both extensions are installed, match the selected style: built-in
  // Noto Serif pairs with NotoSerifJP, everything else with NotoSansJP.
  const bool preferSerif = SETTINGS.sdFontFamilyName[0] == '\0' &&
                           SETTINGS.fontFamily == CrossPointSettings::NOTOSERIF;
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
      const auto* wanted = fam->findClosestReaderSize(sizeEnum);
      if (wanted && wanted->pointSize == fallbackManager_.currentPointSize()) return;
    }
    if (!fallbackManager_.loadFamily(*fam, renderer, sizeEnum)) continue;
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
  jpFallbackNeeded_ = needed;
  ensureJpFallback(renderer, fontSizeEnumFromSettings());
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
