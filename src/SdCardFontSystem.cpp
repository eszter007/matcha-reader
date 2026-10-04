#include "SdCardFontSystem.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <TtfEpdFont.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

namespace {

#if CROSSPOINT_VECTOR_FONTS
// Stable, non-zero renderer font id for a vector family at a size (FNV-1a of
// name + size). 0 is the "not found" sentinel, so bump collisions to 1.
int computeTtfFontId(const char* familyName, uint8_t pointSize) {
  uint32_t hash = 2166136261u;
  for (const char* p = familyName; p && *p; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619u;
  }
  hash ^= pointSize;
  hash *= 16777619u;
  hash ^= 0x54544600u;  // "TTF\0" salt to avoid colliding with cpfont ids
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}
#endif  // CROSSPOINT_VECTOR_FONTS

}  // namespace

// Out-of-line ctor/dtor: TtfEpdFont is complete here, so unique_ptr<TtfEpdFont>
// can be constructed/destroyed. (Declared in the header where it is only
// forward-declared.)
SdCardFontSystem::SdCardFontSystem() = default;
SdCardFontSystem::~SdCardFontSystem() = default;

namespace {
// Family names are compared case- and separator-insensitively: the same face reaches us as
// "NotoSansJP", "notosans-jp" or "Noto Sans JP" depending on who wrote the directory.
std::string normalizedFamilyKey(const std::string& familyName) {
  std::string key;
  key.reserve(familyName.size());
  for (const char c : familyName) {
    // Both <cctype> calls take the unsigned value: char is signed on this target, and passing a
    // negative one is undefined. UTF-8 continuation bytes reach here for any non-ASCII name.
    const auto byte = static_cast<unsigned char>(c);
    if (std::isalnum(byte)) key.push_back(static_cast<char>(std::tolower(byte)));
  }
  return key;
}

// Suffixes marking a family as a wider-coverage cut of another one, ordered widest-first:
// a base with both variants installed resolves to the earlier entry. "Extended" adds Greek,
// Cyrillic and the phonetic block on top of a Latin face; "IPA" adds the phonetic block alone.
constexpr const char* kCoverageVariantSuffixes[] = {"Extended", "IPA"};

// Directory name of a built-in reader family, so a variant can name one as its base even
// though the built-ins are compiled in rather than discovered on the card.
const char* builtinFamilyDirName(const uint8_t fontFamily) {
  return fontFamily == CrossPointSettings::NOTOSANS ? "NotoSans" : "NotoSerif";
}

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
// SD-font converter). Each is paired with a same-size SD fallback so UI text
// in scripts the built-ins lack (CJK, Greek, Cyrillic, ...) matches the
// surrounding Latin. See SdCardFontSystem::setupUiFallbacks.
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};
constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};
// The size a companion is loaded at when only the UI needs it: the largest UI size, and the
// smallest the CJK cuts ship.
constexpr uint8_t UI_FALLBACK_POINT_SIZE = 12;
static_assert(std::size(kUiFontSizes) == 3, "SdCardFontSystem::lookupUiPrevious_ holds one entry per UI font");

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

  // Load whatever the saved selection resolves to. Shared with ensureLoaded() so a selection
  // written by an older build is migrated, and a coverage variant stands in, at boot too.
  ensureSelectedLoaded(renderer);

  ensureCjkFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);

  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  ensureSelectedLoaded(renderer);
  ensureCjkFallback(renderer, SETTINGS.fontPointSize);
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

  // A CJK extension family must never be the SELECTED reader font: it is the
  // CJK half of a built-in Noto entry and is hidden from both pickers, so a
  // selection carried over from an older build would be stuck and would render
  // Latin books in the CJK face. Revert it to the matching built-in and let
  // ensureCjkFallback() bring the extension back as the companion where needed.
  if (SETTINGS.sdFontFamilyName[0] != '\0' && isBuiltinCjkExtension(SETTINGS.sdFontFamilyName)) {
    SETTINGS.fontFamily = normalizedFamilyKey(SETTINGS.sdFontFamilyName).rfind("notoserif", 0) == 0
                              ? CrossPointSettings::NOTOSERIF
                              : CrossPointSettings::NOTOSANS;
    LOG_INF("SDFS", "Reverting hidden CJK extension selection '%s' to built-in", SETTINGS.sdFontFamilyName);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }

  // Likewise for a coverage variant: the picker no longer offers it, so a selection saved by an
  // older build would name a row that is gone. Point the setting at the base it widens -- the
  // row that now stands for both -- and let resolveSelectedFamily() bring the variant back where
  // it fits.
  if (SETTINGS.sdFontFamilyName[0] != '\0' && isCoverageVariant(SETTINGS.sdFontFamilyName, &registry_)) {
    const std::string baseKey = coverageVariantBase(SETTINGS.sdFontFamilyName);
    const SdCardFontFamilyInfo* base = nullptr;
    for (const auto& fam : registry_.getFamilies()) {
      if (normalizedFamilyKey(fam.name) == baseKey) {
        base = &fam;
        break;
      }
    }
    LOG_INF("SDFS", "Migrating hidden variant selection '%s' to its base", SETTINGS.sdFontFamilyName);
    if (base) {
      strncpy(SETTINGS.sdFontFamilyName, base->name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
    } else {
      SETTINGS.fontFamily = baseKey == "notosans" ? CrossPointSettings::NOTOSANS : CrossPointSettings::NOTOSERIF;
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  }

  // What the user picked stays in SETTINGS; what is resident may be the wider variant standing
  // in for it. A stand-in that fails to load falls back to the base rather than clearing the
  // selection -- the user never chose the variant, so it must not be able to unpick their font.
  const std::string wantedFamily = resolveSelectedFamily();
  const bool standingIn = wantedFamily != SETTINGS.sdFontFamilyName;

  const std::string& currentFamily = manager_.currentFamilyName();

  if (wantedFamily.empty()) {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    // Back on a built-in family: a size inherited from an SD family has to come back into the
    // set that row offers -- BUILTIN_READER_POINT_SIZES widened by its stand-ins, not the
    // built-in set alone, which would undo a stand-in size the picker legitimately offers.
    snapFontPointSizeTo(snapToNearestPointSize(rowPointSizes(), SETTINGS.fontPointSize));
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s%s", wantedFamily.c_str(),
              standingIn ? " (keeping base selection)" : " (clearing)");
      manager_.unloadAll(renderer);
      if (!standingIn) SETTINGS.clearSdFontFamily();
      return;
    }
    const auto* selected = family->findNearestSize(SETTINGS.fontPointSize);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    // Snap before the early return: the wanted size can already be loaded while
    // the setting still names a size this family does not ship. Only persist it when the ROW
    // does not offer the size either -- `family` may be a stand-in that happens to lack a size
    // another family for this row can render, and writing its nearest down would silently
    // demote the user's choice the first time a book pulled the stand-in in.
    const auto rowSizes = rowPointSizes();
    if (std::find(rowSizes.begin(), rowSizes.end(), SETTINGS.fontPointSize) == rowSizes.end()) {
      snapFontPointSizeTo(wantedPt);
    }
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u%s", wantedFamily.c_str(), manager_.currentPointSize(), wantedPt,
            registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  // Free the JP fallback font BEFORE loading the newly selected family: two SD fonts' interval
  // and kern tables don't reliably coexist on this heap (UDDigiKyokasho's sparse-coverage
  // interval table is the known worst case), and a failed load silently clears the user's
  // selection. ensureCjkFallback() re-establishes the fallback afterwards if still needed.
  if (!fallbackManager_.currentFamilyName().empty()) {
    fallbackManager_.unloadAll(renderer);
  }
  // Under fragmentation, hand the font decompressor's buffers to the load as well.
  if (ESP.getMaxAllocHeap() < 32 * 1024) {
    if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
#if CROSSPOINT_VECTOR_FONTS
    if (family->vector) {
      // Vector (.ttf/.otf) families load through the FreeInkFont path. The .cpfont manager
      // below rejects them ("Invalid magic bytes") and the failure branch would clear the
      // user's selection on every boot; loadTtfFamily keeps it on transient failures and
      // registers the UI fallbacks itself.
      loadTtfFamily(*family, renderer, registryWasDirty);
      return;
    }
#endif
    if (manager_.loadFamily(*family, renderer, SETTINGS.fontPointSize)) {
      // Persisted only when the row cannot offer the setting at all — see the matching guard on
      // the already-resident path above.
      const auto rowSizes = rowPointSizes();
      if (std::find(rowSizes.begin(), rowSizes.end(), SETTINGS.fontPointSize) == rowSizes.end()) {
        snapFontPointSizeTo(manager_.currentPointSize());
      }
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily.c_str());
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s%s", wantedFamily.c_str(),
              standingIn ? " (keeping base selection)" : " (clearing)");
      if (!standingIn) SETTINGS.clearSdFontFamily();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s%s", wantedFamily.c_str(),
            standingIn ? " (keeping base selection)" : " (clearing)");
    if (!standingIn) SETTINGS.clearSdFontFamily();
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;  // no SD family loaded — nothing to fall back to

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on codepoints the built-in UI fonts lack,
  // so a family with no coverage beyond theirs can never act as a fallback and
  // its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script the built-in fonts may lack:
  // Han, Hiragana, Katakana, Hangul, Greek, Cyrillic, Hebrew, Arabic, Thai,
  // Devanagari.
  static constexpr uint32_t kFallbackProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00, 0x03B1,
                                                 0x0430, 0x05D0, 0x0627, 0x0E01, 0x0905};
  bool hasFallbackScript = false;
  for (const uint32_t cp : kFallbackProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasFallbackScript = true;
      break;
    }
  }
  if (!hasFallbackScript) {
    LOG_DBG("SDFS", "%s has no fallback-script coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  registerUiSizes(manager_, *family, renderer);
}

void SdCardFontSystem::registerUiSizes(SdCardFontManager& mgr, const SdCardFontFamilyInfo& family,
                                       GfxRenderer& renderer) {
  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = mgr.loadFamilyExtraSize(family, renderer, ui.pointSize);
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
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, family.name.c_str());
    }
  }
}

void SdCardFontSystem::lendCompanionToUiFonts(GfxRenderer& renderer, const int sdFontId, const uint8_t pointSize) {
  // The companion is resident at ONE size -- the reader's in a book, 12 pt for the UI alone --
  // and the CJK cuts ship nothing below 12 pt. Unmapped, an 8 or 10 pt UI font takes each
  // glyph the built-in subset lacks from the global fallback at that size: a Chinese title, a
  // menu row or a rare kanji in a chapter name draws half its characters larger than the rest.
  // So every UI font is sent to the resident font, scaled to its own size: no second table in
  // RAM, and one size across the string.
  if (sdFontId == 0 || pointSize == 0) return;
  for (const auto& ui : kUiFontSizes) {
    renderer.setFallbackFont(ui.fontId, sdFontId, static_cast<uint16_t>(ui.pointSize * 256u / pointSize));
  }
}

void SdCardFontSystem::ensureWordLookupFallback(GfxRenderer& renderer, const int primaryFontId,
                                                const uint8_t pointSize) {
  // Tiny intentionally stays on the compact built-in path; avoid loading an SD font for it.
  if (pointSize <= 8) return;
  // The selected family when it draws the book's script; otherwise the companion that was loaded
  // because it does not. Without either, the panel takes the companion's reader-size glyphs
  // through the global fallback, and an entry mixes two sizes wherever the built-in subset
  // stops: every line of a Chinese entry, a rare kanji of a Japanese one.
  const std::string& selected = manager_.currentFamilyName();
  const bool selectedCovers = !selected.empty() && loadedFamilyCovers(manager_, selected, cjkProbe());
  const bool useCompanion = !fallbackManager_.currentFamilyName().empty() && !selectedCovers;
  SdCardFontManager& mgr = useCompanion ? fallbackManager_ : manager_;
  if (mgr.currentFamilyName().empty()) return;
  const auto* family = registry_.findFamily(mgr.currentFamilyName());
  if (!family) return;

  const bool wasResident = mgr.hasSize(pointSize);
  const int sdFontId = mgr.loadFamilyExtraSize(*family, renderer, pointSize);
  if (sdFontId == 0) return;
  renderer.setFallbackFont(primaryFontId, sdFontId);
  const auto builtinIt = renderer.getFontMap().find(primaryFontId);
  if (builtinIt != renderer.getFontMap().end()) {
    if (useCompanion) {
      // The companion has Latin of its own, in another face and without italics. Keep the
      // built-in's: a line sent here for one hanzi must not change typeface around it. The
      // font may be shared (the UI's 12 pt, the reader's own size), so what it pointed at before
      // is put back when the session ends.
      const auto sdIt = renderer.getFontMap().find(sdFontId);
      for (auto& saved : lookupFallbackSaved_) {
        if (saved.sdFontId == sdFontId) break;
        if (saved.sdFontId != 0 || sdIt == renderer.getFontMap().end()) continue;
        saved = {sdFontId, sdIt->second.getFallback(), sdIt->second.fallbackIsFirstForNonCjk()};
        break;
      }
    }
    renderer.setFamilyFallback(sdFontId, &builtinIt->second, /*nonCjkFirst=*/useCompanion);
  }
  // Remember it for releaseWordLookupFallback(): the font when this call loaded it, and in any
  // case the mapping, which must not outlive a font another primary's release unloads.
  const bool ownedAlready = std::any_of(std::begin(lookupExtras_), std::end(lookupExtras_),
                                        [sdFontId](const LookupExtra& e) { return e.sdFontId == sdFontId; });
  if (!wasResident || ownedAlready) {
    for (auto& extra : lookupExtras_) {
      if (extra.primaryFontId == primaryFontId || extra.primaryFontId == 0) {
        extra = {primaryFontId, sdFontId, useCompanion};
        break;
      }
    }
  }
  // The panel's footer, reading and tag lines are set in the UI fonts, which in a book borrow
  // the companion at the reader's size (see ensureCjkFallback). Lend them this one for the
  // session instead: it is the panel's own size, and it is already paid for.
  if (useCompanion && !lookupUiLent_) {
    for (size_t i = 0; i < std::size(kUiFontSizes); i++) {
      lookupUiPrevious_[i] = {renderer.fallbackFontFor(kUiFontSizes[i].fontId),
                              renderer.fallbackScaleFor(kUiFontSizes[i].fontId)};
      renderer.setFallbackFont(kUiFontSizes[i].fontId, sdFontId,
                               static_cast<uint16_t>(kUiFontSizes[i].pointSize * 256u / pointSize));
    }
    lookupUiLent_ = true;
  }
}

void SdCardFontSystem::releaseWordLookupFallback(GfxRenderer& renderer) {
  if (lookupUiLent_) {
    for (size_t i = 0; i < std::size(kUiFontSizes); i++) {
      if (lookupUiPrevious_[i].fontId != 0) {
        renderer.setFallbackFont(kUiFontSizes[i].fontId, lookupUiPrevious_[i].fontId, lookupUiPrevious_[i].scale);
      } else {
        renderer.clearFallbackFont(kUiFontSizes[i].fontId);
      }
    }
    lookupUiLent_ = false;
  }
  for (auto& saved : lookupFallbackSaved_) {
    // A font this session loaded is unloaded below; one that was resident gets its fallback back.
    if (saved.sdFontId != 0) renderer.setFamilyFallback(saved.sdFontId, saved.fallback, saved.nonCjkFirst);
    saved = {};
  }
  for (auto& extra : lookupExtras_) {
    if (extra.sdFontId == 0) continue;
    renderer.clearFallbackFont(extra.primaryFontId);
    // a no-op the second time for a shared font
    (extra.companion ? fallbackManager_ : manager_).unloadExtra(extra.sdFontId, renderer);
    extra = {};
  }
}

int SdCardFontSystem::effectiveReaderFontId(const CjkScript script) const {
  const bool cjkBook = script != CjkScript::None;
  if (!selectedFontCovers(cjkBook ? cjk::probeCodepoint(script) : 'a')) {
    if (cjkBook) {
      const int companion = companionFontId();
      // 0 means no companion is resident -- it can fail to load under heap pressure. Fall
      // through to the selected font rather than returning 0, which reads as "no font".
      if (companion != 0) return companion;
    } else {
      return SETTINGS.getBuiltinSerifReaderFontId();
    }
  }
  return SETTINGS.getReaderFontId();
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
#if CROSSPOINT_VECTOR_FONTS
  // A loaded vector (.ttf) family answers first — it isn't in the .cpfont manager.
  if (ttfFontId_ != 0 && familyName && ttfFamily_ == familyName) return ttfFontId_;
#endif
  // The manager holds exactly one reader-size font, already selected for
  // SETTINGS.fontPointSize, so the size argument is implicit — always return
  // that font's ID. ensureLoaded() must have run for the current settings first.
  //
  // An empty name is the built-in selection asking what stands in for it: the resident family,
  // when a coverage variant was loaded in the built-in's place, and 0 (use the built-in) when
  // nothing is. Runs in the page render loop via getReaderFontId(), so it must not allocate.
  if (familyName == nullptr || familyName[0] == '\0') {
    const std::string& resident = manager_.currentFamilyName();
    return resident.empty() ? 0 : manager_.getFontId(resident);
  }
  return manager_.getFontId(familyName);
}

CjkScript SdCardFontSystem::extensionScript(const std::string& familyName) {
  const std::string key = normalizedFamilyKey(familyName);
  std::string suffix;
  if (key.rfind("notosans", 0) == 0) {
    suffix = key.substr(8);
  } else if (key.rfind("notoserif", 0) == 0) {
    suffix = key.substr(9);
  } else {
    return CjkScript::None;
  }
  if (suffix == "jp") return CjkScript::Japanese;
  if (suffix == "sc") return CjkScript::SimplifiedChinese;
  if (suffix == "tc" || suffix == "hk") return CjkScript::TraditionalChinese;
  return CjkScript::None;
}

bool SdCardFontSystem::isBuiltinCjkExtension(const std::string& familyName) {
  return extensionScript(familyName) != CjkScript::None;
}

uint8_t SdCardFontSystem::readerStandInFamilies(const SdCardFontRegistry* registry, const char* sdFamilyName,
                                                const uint8_t fontFamily, const SdCardFontFamilyInfo** out,
                                                const uint8_t cap) {
  if (!registry || !out || cap == 0) return 0;
  uint8_t count = 0;

  const bool builtinRow = sdFamilyName == nullptr || sdFamilyName[0] == '\0';
  const std::string base = builtinRow ? builtinFamilyDirName(fontFamily) : sdFamilyName;
  if (const auto* variant = findCoverageVariant(registry, base)) out[count++] = variant;

  // Built-in rows only. ensureCjkFallback() loads the companion for a CJK book just when the
  // row's own face lacks CJK, which is true of the built-ins by definition (selectedFontCovers
  // treats them as Latin-complete and CJK-less) but unknowable here for an SD family: coverage
  // lives in its .cpfont interval table and is only readable once resident. An SD row would
  // otherwise offer sizes that a self-sufficient CJK family never renders at.
  //
  // Matched on the normalized key, like every other family comparison here, so a folder named
  // "noto sans jp" also pairs. The JP cut is preferred when several extensions are installed;
  // every cut ships the same sizes, so whichever ensureCjkFallback() picks loads at that size.
  if (builtinRow && count < cap) {
    const char* wantedStem = fontFamily == CrossPointSettings::NOTOSANS ? "notosans" : "notoserif";
    const SdCardFontFamilyInfo* best = nullptr;
    for (const auto& fam : registry->getFamilies()) {
      const std::string key = normalizedFamilyKey(fam.name);
      if (key.rfind(wantedStem, 0) != 0 || extensionScript(fam.name) == CjkScript::None) continue;
      if (!best || extensionScript(fam.name) == CjkScript::Japanese) best = &fam;
    }
    if (best) out[count++] = best;
  }
  return count;
}

std::string SdCardFontSystem::coverageVariantBase(const std::string& familyName) {
  const std::string key = normalizedFamilyKey(familyName);
  for (const char* suffix : kCoverageVariantSuffixes) {
    const std::string suffixKey = normalizedFamilyKey(suffix);
    // Strictly longer: a family named exactly "IPA" is its own face, not a suffix on nothing.
    if (key.size() <= suffixKey.size()) continue;
    if (key.compare(key.size() - suffixKey.size(), suffixKey.size(), suffixKey) == 0) {
      return key.substr(0, key.size() - suffixKey.size());
    }
  }
  return {};
}

const SdCardFontFamilyInfo* SdCardFontSystem::findCoverageVariant(const SdCardFontRegistry* registry,
                                                                  const std::string& baseName) {
  if (!registry) return nullptr;
  const std::string baseKey = normalizedFamilyKey(baseName);
  if (baseKey.empty()) return nullptr;
  for (const char* suffix : kCoverageVariantSuffixes) {
    const std::string wanted = baseKey + normalizedFamilyKey(suffix);
    for (const auto& fam : registry->getFamilies()) {
      if (normalizedFamilyKey(fam.name) == wanted) return &fam;
    }
  }
  return nullptr;
}

bool SdCardFontSystem::isCoverageVariant(const std::string& familyName, const SdCardFontRegistry* registry) {
  const std::string baseKey = coverageVariantBase(familyName);
  if (baseKey.empty()) return false;
  // The base has to actually exist, otherwise the variant is the only carrier of its glyphs
  // and hiding it would put them out of reach. Built-in bases are always present.
  if (baseKey == "notoserif" || baseKey == "notosans") return true;
  if (registry == nullptr) return false;
  for (const auto& fam : registry->getFamilies()) {
    if (normalizedFamilyKey(fam.name) == baseKey) return true;
  }
  return false;
}

std::vector<uint8_t> SdCardFontSystem::rowPointSizes() const {
  const SdCardFontFamilyInfo* standIns[MAX_STAND_INS];
  const uint8_t count =
      readerStandInFamilies(&registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily, standIns, MAX_STAND_INS);
  return readerFontPointSizes(&registry_, SETTINGS.sdFontFamilyName, standIns, count);
}

// Does the face this row renders with ship `pt` itself? An empty name is a built-in family,
// which exists at exactly BUILTIN_READER_POINT_SIZES.
bool SdCardFontSystem::faceShipsSize(const std::string& familyName, const uint8_t pt) const {
  if (familyName.empty()) {
    return std::find(std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES), pt) !=
           std::end(BUILTIN_READER_POINT_SIZES);
  }
  const auto* family = registry_.findFamily(familyName);
  return family && family->hasSize(pt);
}

std::string SdCardFontSystem::resolveSelectedFamily() const {
  const std::string selected = SETTINGS.sdFontFamilyName;
  // A book that needs CJK keeps the base as-is. The companion ensureCjkFallback() is about
  // to load carries this book's text, so standing a coverage variant in here would only make it
  // the second resident SD font -- the pairing that does not reliably fit this heap. Leaving the
  // base built-in also keeps its `preferSans` ranking working, which reads an empty selection.
  if (cjkFallbackNeeded()) return selected;
  const std::string base = selected.empty() ? builtinFamilyDirName(SETTINGS.fontFamily) : selected;
  const auto* variant = findCoverageVariant(&registry_, base);
  const std::string resolved = variant ? variant->name : selected;
  if (faceShipsSize(resolved, SETTINGS.fontPointSize)) return resolved;

  // The row offers sizes its own face does not have: readerFontPointSizes() widens the list with
  // every size the stand-ins ship, because a book one of them renders is rendered at exactly that
  // size. Nothing used to reach that for a Latin book -- the stand-in was consulted for Japanese
  // only -- so picking such a size snapped silently back to the nearest built-in one and 18 and
  // 20 drew the same pixels. Render with the stand-in that has the size instead.
  //
  // Safe for any book: a stand-in is a superset of the row's Latin coverage by construction -- a
  // coverage variant widens its base, and the CJK extensions are latin-ext + cjk-ext.
  const SdCardFontFamilyInfo* standIns[MAX_STAND_INS];
  const uint8_t count =
      readerStandInFamilies(&registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily, standIns, MAX_STAND_INS);
  for (uint8_t i = 0; i < count; i++) {
    if (standIns[i] && faceShipsSize(standIns[i]->name, SETTINGS.fontPointSize)) return standIns[i]->name;
  }
  return resolved;
}

bool SdCardFontSystem::loadedFamilyCovers(const SdCardFontManager& mgr, const std::string& name,
                                          const uint32_t cp) const {
#if CROSSPOINT_VECTOR_FONTS
  // A vector (.ttf/.otf) family is not loaded through the .cpfont manager at all, so the name
  // below never matches and every coverage question about it answered "no". The reader then
  // concluded the selected face had neither CJK nor Latin and attached a companion, which took
  // over pricing and drawing -- text in the chosen face at another font's metrics. Ask the face.
  if (ttf_ && !ttfFamily_.empty() && ttfFamily_ == name) {
    return ttf_->coversCodepoint(cp);
  }
#endif
  if (mgr.currentFamilyName() != name) return false;
  const SdCardFont* font = mgr.loadedFont();
  // Coverage must come from the font FILE's full interval table -- EpdFont::hasGlyph on SD
  // fonts only answers which glyphs happen to be resident right now.
  return font && font->coversCodepoint(cp);
}

void SdCardFontSystem::ensureCjkFallback(GfxRenderer& renderer, const uint8_t pointSize) {
  // Companion-font need is coverage-driven in BOTH directions:
  //  - selected font lacks the book's CJK script and the book needs it (cjkScript_) -> companion
  //  - selected font lacks LATIN (UDDigiKyokasho ships cjk-ext only: English words, digits
  //    and UI text would render blank) -> companion, regardless of book language
  // The CJK extension fonts (NotoSansJP/SC/TC and the serifs, latin-ext + cjk-ext) cover both.
  // The selected family may be a vector (.ttf/.otf) one, which loads through the FreeInkFont
  // path rather than the .cpfont manager -- so the manager's name is empty for it. Reading the
  // name from the manager alone made a TTF look like "no SD font selected", short-circuiting
  // both coverage tests to "lacks CJK", and a companion was attached over a face that covers
  // Japanese perfectly well. The companion then priced and drew the text: glyphs from the
  // chosen face at another font's advances, which reads as wrong spacing and wrong pagination.
  std::string selected = manager_.currentFamilyName();
#if CROSSPOINT_VECTOR_FONTS
  if (selected.empty() && ttf_ && !ttfFamily_.empty()) selected = ttfFamily_;
#endif
  const bool selectedHasCjk = !selected.empty() && loadedFamilyCovers(manager_, selected, cjkProbe());
  const bool selectedHasLatin = selected.empty()  // built-ins always have Latin
                                    ? true
                                    : loadedFamilyCovers(manager_, selected, 'a');
  // Only load a companion for a book that actually needs CJK. A Latin book read
  // with a CJK-only family (UDDigiKyokasho) does NOT: the reader already substitutes
  // the built-in Noto Serif/Sans for it (effectiveReaderFontId). Loading a companion
  // anyway sets fallbackSdFont_ and redirects the global fallback to a CJK family,
  // which then prices/draws the built-in font's glyphs -- collapsing the word spaces
  // and making Latin text render as if it were Japanese. cjkScript_ is the
  // book-level signal; within a CJK book a companion still covers either hole
  // (no CJK in the selected font, or no Latin for embedded English).
  const bool needsCompanion = cjkFallbackNeeded() && (!selectedHasCjk || !selectedHasLatin);
  if (!needsCompanion) {
    if (!fallbackManager_.currentFamilyName().empty()) {
      // Unloading clears every UI fallback registration, the selected family's included; put
      // those back so list rows keep its glyphs after a companion comes and goes.
      fallbackManager_.unloadAll(renderer);
      setupUiFallbacks(renderer);
    }
    return;
  }

  // Selected font (built-in, or a Latin-only SD font) can't render the book: pair a built-in
  // Noto face with the extension cut for the book's script in its own style, then that script
  // in the other style, then any other CJK cut (a JP font still carries every common hanzi, in
  // Japanese glyph forms), then anything else on the card.
  const bool preferSans = selected.empty() && SETTINGS.fontFamily == CrossPointSettings::NOTOSANS;
  const CjkScript wantScript = activeCjkScript();
  auto extensionRank = [preferSans, wantScript](const std::string& name) {
    const bool styleMatch = (normalizedFamilyKey(name).rfind("notosans", 0) == 0) == preferSans;
    const bool scriptMatch = extensionScript(name) == wantScript;
    return (scriptMatch ? 0 : 2) + (styleMatch ? 0 : 1);
  };
  std::vector<const SdCardFontFamilyInfo*> candidates;
  for (const auto& fam : registry_.getFamilies()) {
    if (fam.name == selected) continue;
    if (isBuiltinCjkExtension(fam.name)) candidates.push_back(&fam);
  }
  std::sort(candidates.begin(), candidates.end(),
            [&extensionRank](const SdCardFontFamilyInfo* a, const SdCardFontFamilyInfo* b) {
              return extensionRank(a->name) < extensionRank(b->name);
            });
  for (const auto& fam : registry_.getFamilies()) {
    if (fam.name == selected || isBuiltinCjkExtension(fam.name)) continue;
    candidates.push_back(&fam);
  }

  // Asked for by the UI alone (Home, the file browser): only the list rows need it, so load the
  // family at the UI size instead of the reader's. That is the one size registerUiSizes() wants
  // anyway, and it keeps a 16pt interval table out of a screen that also holds cover thumbnails.
  // The next book open asks again at the reader size and the family reloads at it.
  const uint8_t loadPt = cjkScript_ != CjkScript::None ? pointSize : UI_FALLBACK_POINT_SIZE;
  for (const auto* fam : candidates) {
    // Which size the companion will actually be asked for: findNearestSize() may land below the
    // point size the picker offered, which is the difference between a size change taking effect
    // and silently doing nothing.
    const auto* want = fam->findNearestSize(loadPt);
    LOG_DBG("SDFS", "Companion candidate %s: asked %u -> nearest %u", fam->name.c_str(), loadPt,
            want ? want->pointSize : 0);
    // Already loaded at the right size? Keep it.
    if (fallbackManager_.currentFamilyName() == fam->name) {
      if (want && want->pointSize == fallbackManager_.currentPointSize()) return;
    }
    // Make room before asking, the same way ensureSelectedLoaded() does for the selected family.
    // The companion's interval table is one contiguous block -- 26 KB for a broad CJK face at a
    // large size -- and this load runs while a book is open, so the heap is fragmented rather
    // than empty: 70 KB free behind a 20 KB largest block was enough to fail. The glyph slabs
    // are a cache and are refilled on demand, so releasing them costs redraw time, not content.
    if (ESP.getMaxAllocHeap() < COMPANION_LOAD_HEADROOM) {
      if (auto* fcm = renderer.getFontCacheManager()) {
        const uint32_t before = ESP.getMaxAllocHeap();
        fcm->releaseAllFontMemory();
        LOG_DBG("SDFS", "Freed font caches for companion load: maxAlloc %u -> %u", before,
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
      }
    }
    if (!fallbackManager_.loadFamily(*fam, renderer, loadPt)) {
      LOG_ERR("SDFS", "Companion %s failed to load at %u (free=%u largest=%u)", fam->name.c_str(), loadPt,
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      continue;
    }
    if (loadedFamilyCovers(fallbackManager_, fam->name, cjkProbe()) &&
        loadedFamilyCovers(fallbackManager_, fam->name, 'a')) {
      LOG_DBG("SDFS", "Companion fallback font: %s", fam->name.c_str());
      // Asked for by the UI with a built-in family selected: nothing else serves the list rows'
      // own sizes, so let the companion (loaded at the UI size above). In a book the companion
      // sits at the reader size and the global fallback serves the few UI glyphs; a second size
      // table beside it is RAM a page build needs.
      // The selected family serves the UI sizes itself when it can draw the script; a built-in
      // face or a Latin-only one (Literata) leaves the UI's hanzi to the companion.
      if (!selectedHasCjk) {
        lendCompanionToUiFonts(renderer, fallbackManager_.getFontId(fam->name), fallbackManager_.currentPointSize());
      }
      return;
    }
    // Loaded fine but doesn't cover both scripts -- not a useful companion.
    fallbackManager_.unloadAll(renderer);
  }

  LOG_ERR("SDFS",
          "No companion could be loaded at %u -- a CJK book will fall back to the "
          "selected font and its size",
          pointSize);
  if (!fallbackManager_.currentFamilyName().empty()) {
    fallbackManager_.unloadAll(renderer);
    setupUiFallbacks(renderer);  // unloadAll() cleared the selected family's UI registrations too
  }
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
  } else if (!selected.empty() && loadedFamilyCovers(manager_, selected, cjkProbe()) &&
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

CjkScript SdCardFontSystem::activeCjkScript() const {
  if (cjkScript_ != CjkScript::None) return cjkScript_;
  if (uiCjkScript_ != CjkScript::None) return uiCjkScript_;
  switch (I18N.getLanguage()) {
    case Language::ZHS:
      return CjkScript::SimplifiedChinese;
    case Language::ZHT:
      return CjkScript::TraditionalChinese;
    default:
      return CjkScript::None;
  }
}

void SdCardFontSystem::setCjkFallbackNeeded(GfxRenderer& renderer, const CjkScript script) {
  // A book decides from here on; the UI's own request is over until Home asks again.
  if (cjkScript_ == script && uiCjkScript_ == CjkScript::None) return;
  LOG_DBG("SDFS", "CJK fallback script: %d", static_cast<int>(script));
  cjkScript_ = script;
  uiCjkScript_ = CjkScript::None;
  // resolveSelectedFamily() reads this flag: a collapsed entry is the base plus a CJK companion
  // for a CJK book and the wider variant for any other, so the selection is re-resolved
  // here rather than only at book open. Called at book/activity boundaries, never mid-render.
  ensureSelectedLoaded(renderer);
  ensureCjkFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);
}

void SdCardFontSystem::setUiCjkNeeded(GfxRenderer& renderer, const CjkScript script) {
  if (uiCjkScript_ == script) return;
  LOG_DBG("SDFS", "UI CJK script: %d", static_cast<int>(script));
  uiCjkScript_ = script;
  ensureSelectedLoaded(renderer);
  ensureCjkFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);
}

bool SdCardFontSystem::builtinCjkCovers(const uint32_t cp) const {
  return defaultGlobalFallback_ != nullptr && defaultGlobalFallback_->hasCodepoint(cp);
}

CjkScript SdCardFontSystem::uiCjkScriptFor(const char* utf8) const {
  if (!utf8) return CjkScript::None;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    const uint32_t cp = utf8NextCodepoint(&p);
    if (cjk::isHan(cp) && !builtinCjkCovers(cp)) return CjkScript::SimplifiedChinese;
  }
  return CjkScript::None;
}

void SdCardFontSystem::releaseAllResidentFonts(GfxRenderer& renderer) {
  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t maxBefore = ESP.getMaxAllocHeap();

  // Drop the companion first, then the selected family. manager_.unloadAll() also removes the
  // size-matched UI fallback registrations before deleting their backing SdCardFont objects.
  // cjkScript_ is deliberately left alone: it is policy ("this book wants a CJK companion"),
  // not residency, and ensureLoaded() reads it to decide what to restore. Clearing it here would
  // quietly demote a CJK book's fallback for any caller that only wanted the memory back.
  // Callers that mean to change the policy call setCjkFallbackNeeded().
  if (!fallbackManager_.currentFamilyName().empty()) fallbackManager_.unloadAll(renderer);
  if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
  updateGlobalFallback(renderer);

  // Glyph slabs and hot groups are owned by FontCacheManager rather than either SD-font manager.
  // Release them too so the caller receives one coalesced block, not merely enough total bytes.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();

  LOG_INF("SDFS", "Resident font release: free %u->%u, maxAlloc %u->%u", freeBefore, ESP.getFreeHeap(), maxBefore,
          ESP.getMaxAllocHeap());
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

#if CROSSPOINT_VECTOR_FONTS

void SdCardFontSystem::freeTtfSources() {
  for (auto& s : ttfSources_) {
    s.bytes.clear();
    freeink::font::PsramVector<uint8_t>().swap(s.bytes);  // actually release
    if (s.file) s.file.close();
    s.streamed = false;
    s.size = 0;
    s.present = false;
  }
}

void SdCardFontSystem::unloadTtf(GfxRenderer& renderer) {
  if (ttfFamily_.empty() && ttfFontId_ == 0 && ttfUiIds_.empty()) return;
  // UI-size fallbacks first (they borrow ttfSources_).
  for (const int id : ttfUiIds_) {
    renderer.unregisterTtfFont(id);
    renderer.removeFont(id);
  }
  ttfUiIds_.clear();
  ttfUi_.clear();
  renderer.clearFallbackFonts();
  if (ttfFontId_ != 0) {
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);  // drop from the renderer's fontMap
  }
  ttf_.reset();  // frees the FT faces first (they read ttfSources_)
  freeTtfSources();
  ttfFamily_.clear();
  ttfFontId_ = 0;
  ttfPointSize_ = 0;
}

bool SdCardFontSystem::openTtfSource(const uint8_t style, const std::string& path) {
  if (style >= 4) return false;
  // Small fonts are read fully into RAM (fastest, fewest SD reads; PSRAM when
  // present). Large fonts (e.g. multi-MB variable/CJK) STREAM from SD so the
  // whole file never sits in RAM — the handle is kept open for the font's life.
  // PSRAM boards only (this whole path is vector-font gated), so size the
  // resident cap for the 8MB parts: a 4.5MB variable font held resident gets
  // GPOS kerning (streamed faces skip it, and GPOS-only fonts like
  // Merriweather VF lose ALL kerning when streamed) and skips per-glyph SD
  // reads. The heap gate below still falls back to streaming when PSRAM
  // can't fund the buffer.
  static constexpr size_t kResidentMax = 6 * 1024 * 1024;
  // Working headroom that must remain in internal DRAM after a resident load
  // (FreeType face setup, glyph caches, and the rest of the system).
  static constexpr size_t kInternalHeadroom = 96 * 1024;
  HalFile f = Storage.open(path.c_str());
  if (!f) {
    LOG_ERR("SDFS", "Failed to open TTF: %s", path.c_str());
    return false;
  }
  const size_t len = f.size();
  if (len == 0) {
    LOG_ERR("SDFS", "Empty TTF: %s", path.c_str());
    f.close();
    return false;
  }
  TtfSource& s = ttfSources_[style];
  // A resident buffer lands in PSRAM when fiFontMalloc can place it there;
  // otherwise it competes with everything else in internal DRAM. PsramAlloc
  // aborts on OOM, so this gate is load-bearing on no-PSRAM boards (X4/C3):
  // fall back to streaming instead of attempting an allocation that can fail.
  bool resident = len <= kResidentMax;
  if (resident && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < len) {
    const size_t internalFree = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internalFree < len + kInternalHeadroom) {
      LOG_DBG("SDFS", "TTF %s (%u KB) too large for DRAM (largest block %u KB), streaming", path.c_str(),
              static_cast<unsigned>(len / 1024), static_cast<unsigned>(internalFree / 1024));
      resident = false;
    }
  }
  if (resident) {
    s.bytes.resize(len);
    const int got = f.read(s.bytes.data(), len);
    f.close();
    if (static_cast<size_t>(got) != len) {
      LOG_ERR("SDFS", "Short read on TTF %s (%d/%u)", path.c_str(), got, static_cast<unsigned>(len));
      s.bytes.clear();
      return false;
    }
    s.streamed = false;
  } else {
    s.file = std::move(f);  // kept open; prefixRead() reads it on demand
    s.streamed = true;
    // Cache the file's head in PSRAM: an sfnt's per-glyph-fault tables (cmap,
    // loca, hmtx) sit before the multi-MB glyf table, so serving the first
    // 1 MB from RAM turns each glyph fault's 4-6 scattered SD seeks into one
    // glyf read. Gated per source so a small-PSRAM board takes what fits.
    static constexpr size_t kStreamPrefix = 1024 * 1024;
    const size_t prefix = len < kStreamPrefix ? len : kStreamPrefix;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > prefix + 256 * 1024) {
      s.bytes.resize(prefix);
      if (s.file.seek(0) && static_cast<size_t>(s.file.read(s.bytes.data(), prefix)) == prefix) {
        LOG_DBG("SDFS", "Cached %u KB TTF prefix in PSRAM", static_cast<unsigned>(prefix / 1024));
      } else {
        s.bytes.clear();  // fall back to pure streaming
      }
    }
    LOG_DBG("SDFS", "Streaming TTF %s (%u KB) from SD", path.c_str(), static_cast<unsigned>(len / 1024));
  }
  s.size = static_cast<unsigned long>(len);
  s.present = true;
  return true;
}

// Streamed-source read: serve from the PSRAM prefix cache when the range is
// there, hit SD only for the tail (glyf outlines). A read straddling the
// boundary splits across both.
unsigned long SdCardFontSystem::prefixRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                           const unsigned long count) {
  auto* s = static_cast<TtfSource*>(ctx);
  const unsigned long cached = s->bytes.size();
  if (offset < cached) {
    const unsigned long fromCache = (offset + count <= cached) ? count : cached - offset;
    if (count == 0) return 0;  // seek probe
    memcpy(buffer, s->bytes.data() + offset, fromCache);
    if (fromCache == count) return count;
    return fromCache +
           SdCardFontRegistry::halFileRead(&s->file, offset + fromCache, buffer + fromCache, count - fromCache);
  }
  return SdCardFontRegistry::halFileRead(&s->file, offset, buffer, count);
}

void SdCardFontSystem::addTtfSources(TtfEpdFont& font) {
  for (uint8_t st = 0; st < 4; ++st) {
    TtfSource& s = ttfSources_[st];
    if (!s.present) continue;
    if (s.streamed) {
      font.addStreamSource(st, &SdCardFontSystem::prefixRead, &s, s.size);
    } else {
      font.addResidentSource(st, s.bytes.data(), static_cast<uint32_t>(s.bytes.size()));
    }
  }
}

void SdCardFontSystem::setupTtfUiFallbacks(GfxRenderer& renderer) {
  if (ttfFamily_.empty()) return;
  // Small caches: UI strings (titles/rows) are short. Each UI family is 4-style
  // but LAZY, so only the regular face is ever built for UI text — the bold/
  // italic faces cost nothing. All faces share the reader's sources (streamed
  // handles or resident bytes), so no extra copy of any font file.
  // Each fallback instance carries its own FreeType face and lazy glyph
  // arena. Without PSRAM those compete with the reader's section build for
  // internal DRAM, and the build must win: below this floor, skip the
  // fallback (built-in bitmap UI fonts keep covering Latin UI text).
  static constexpr size_t kUiFallbackMinInternalHeap = 160 * 1024;
  for (const auto& ui : kUiFontSizes) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) == 0) {
      const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (internalFree < kUiFallbackMinInternalHeap) {
        LOG_DBG("SDFS", "Skipping TTF UI fallback @%upt (%u KB internal free)", ui.pointSize,
                static_cast<unsigned>(internalFree / 1024));
        continue;
      }
    }
    auto f = makeUniqueNoThrow<TtfEpdFont>();
    if (!f) {
      LOG_ERR("SDFS", "OOM: TtfEpdFont for UI fallback @%upt", ui.pointSize);
      continue;  // built-in bitmap UI fonts keep covering this size
    }
    addTtfSources(*f);
    const bool ok = f->load(ui.pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/16 * 1024, /*maxGlyphs=*/384);
    if (!ok) continue;
    LOG_DBG("SDFS", "TTF UI fallback @%upt loaded (heap free %u)", ui.pointSize, (unsigned)ESP.getFreeHeap());
    // Distinct id from the reader-size font: a UI size can equal the reader size
    // (e.g. both 12pt), which would collide on computeTtfFontId and be dropped
    // as a duplicate. Salt the UI family name to separate the id spaces.
    const int id = computeTtfFontId((ttfFamily_ + "\x01ui").c_str(), ui.pointSize);
    renderer.insertFont(id, f->family());
    renderer.registerTtfFont(id, f.get());
    renderer.setFallbackFont(ui.fontId, id);
    ttfUiIds_.push_back(id);
    ttfUi_.push_back(std::move(f));
  }
}

void SdCardFontSystem::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                     const bool registryWasDirty) {
  // Keep sizes inherited from other families within the selectable vector range.
  snapFontPointSizeTo(
      snapToNearestPointSize(VECTOR_READER_POINT_SIZES, std::size(VECTOR_READER_POINT_SIZES), SETTINGS.fontPointSize));
  const uint8_t size = SETTINGS.fontPointSize;

  // Already loaded, same family + size, and disk unchanged → nothing to do.
  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name && ttfPointSize_ == size) return;

  // Reader-face glyph-cache budget (used by both the resize fast path and the
  // full load below): the default 32 KB holds ~90 CJK glyphs, but a CJK page
  // uses 300+, so the cache flush-cycles mid-page and every page turn
  // re-rasterizes the whole page through streamed SD reads (multi-second
  // turns). The arenas are PSRAM-backed (FontPsram); 1 MB / 4096 glyphs holds
  // a whole Japanese novel's working set (~3000 unique kanji+kana at ~350 B
  // each), so the flush-everything ceiling is never hit and warm page turns
  // are pure cache hits. Without PSRAM keep the internal-DRAM-safe default.
  const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
  const size_t cacheBytes = havePsram ? 1024 * 1024 : 32 * 1024;
  const uint16_t maxGlyphs = havePsram ? 4096 : 768;

  // Same family, only the reader size changed (size preview): the open style
  // sources and the size-independent UI fallbacks don't need rebuilding — just
  // re-drive the reader face at the new size, reusing the already-open files
  // instead of reopening all four and rebuilding every UI fallback.
  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name) {
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);
    if (ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs)) {
      ttf_->build(" ");
      ttfFontId_ = computeTtfFontId(family.name.c_str(), size);
      renderer.insertFont(ttfFontId_, ttf_->family());
      renderer.registerTtfFont(ttfFontId_, ttf_.get());
      ttfPointSize_ = size;
      return;
    }
    // Resize failed: fall through to a clean full reload.
  }

  unloadTtf(renderer);

  if (family.files.empty()) {
    LOG_ERR("SDFS", "Vector family %s has no file", family.name.c_str());
    SETTINGS.clearSdFontFamily();
    return;
  }

  // Open each style source the family ships (0=regular, 1=bold, 2=italic,
  // 3=bold-italic). A single-file family (loose .ttf, or a folder with one file)
  // supplies only regular; TtfEpdFont then derives bold/italic from the wght axis
  // or an oblique shear. Extra files upgrade those styles to the real designs.
  for (const auto& file : family.files) {
    const uint8_t role = file.style < 4 ? file.style : 0;
    if (ttfSources_[role].present) continue;  // registry already deduped by role
    openTtfSource(role, file.path);
  }
  if (!ttfSources_[0].present) {
    // Possibly a transient SD read failure: keep the user's selection so the
    // next ensureLoaded() retries; this session falls back to the built-in.
    LOG_ERR("SDFS", "Vector family %s: regular file failed to open (keeping selection)", family.name.c_str());
    freeTtfSources();
    return;
  }

  ttf_ = makeUniqueNoThrow<TtfEpdFont>();
  if (!ttf_) {
    // Transient OOM: keep the user's selection (unlike a parse failure) so the
    // next ensureLoaded() can retry once heap pressure passes.
    LOG_ERR("SDFS", "OOM: TtfEpdFont for %s", family.name.c_str());
    freeTtfSources();
    return;
  }
  addTtfSources(*ttf_);
  const bool ok = ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs);
  if (!ok) {
    // init failure is ambiguous (corrupt font vs. transient OOM inside
    // FreeType): keep the selection and retry next ensureLoaded() rather than
    // silently reverting the user to the built-in font. A genuinely broken
    // font costs one failed load per reader entry, visible in the log.
    LOG_ERR("SDFS", "FreeInkFont could not parse %s (keeping selection)", family.name.c_str());
    ttf_.reset();
    freeTtfSources();
    return;
  }
  // Seed the regular face's glyph cache; other styles + glyphs fault on demand.
  ttf_->build(" ");

  ttfFontId_ = computeTtfFontId(family.name.c_str(), size);
  renderer.insertFont(ttfFontId_, ttf_->family());
  renderer.registerTtfFont(ttfFontId_, ttf_.get());
  ttfFamily_ = family.name;
  ttfPointSize_ = size;
  LOG_DBG("SDFS", "Reader TTF face loaded (heap free %u, max block %u)", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  setupTtfUiFallbacks(renderer);  // CJK/script UI fallback at the built-in UI sizes
  LOG_DBG("SDFS", "Loaded TTF font: %s @ %upt (id %d)", family.name.c_str(), size, ttfFontId_);
}

#endif  // CROSSPOINT_VECTOR_FONTS
