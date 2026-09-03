#include "SdCardFontSystem.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemoryProbe.h>
#include <Logging.h>
#include <SdCardFont.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

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

  // Load whatever the saved selection resolves to. Shared with ensureLoaded() so a selection
  // written by an older build is migrated, and a coverage variant stands in, at boot too.
  ensureSelectedLoaded(renderer);

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
    SETTINGS.fontFamily = normalizedFamilyKey(SETTINGS.sdFontFamilyName) == "notoserifjp"
                              ? CrossPointSettings::NOTOSERIF
                              : CrossPointSettings::NOTOSANS;
    LOG_INF("SDFS", "Reverting hidden JP extension selection '%s' to built-in", SETTINGS.sdFontFamilyName);
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

void SdCardFontSystem::ensureWordLookupFallback(GfxRenderer& renderer, const int primaryFontId,
                                                const uint8_t pointSize) {
  // Tiny intentionally stays on the compact built-in path; avoid loading an SD font for it.
  if (pointSize <= 8 || manager_.currentFamilyName().empty()) return;
  const auto* family = registry_.findFamily(manager_.currentFamilyName());
  if (!family) return;

  const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, pointSize);
  if (sdFontId == 0) return;
  renderer.setFallbackFont(primaryFontId, sdFontId);
  const auto builtinIt = renderer.getFontMap().find(primaryFontId);
  if (builtinIt != renderer.getFontMap().end()) renderer.setFamilyFallback(sdFontId, &builtinIt->second);
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
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

bool SdCardFontSystem::isBuiltinJpExtension(const std::string& familyName) {
  const std::string key = normalizedFamilyKey(familyName);
  return key == "notosansjp" || key == "notoserifjp";
}

uint8_t SdCardFontSystem::readerStandInFamilies(const SdCardFontRegistry* registry, const char* sdFamilyName,
                                                const uint8_t fontFamily, const SdCardFontFamilyInfo** out,
                                                const uint8_t cap) {
  if (!registry || !out || cap == 0) return 0;
  uint8_t count = 0;

  const bool builtinRow = sdFamilyName == nullptr || sdFamilyName[0] == '\0';
  const std::string base = builtinRow ? builtinFamilyDirName(fontFamily) : sdFamilyName;
  if (const auto* variant = findCoverageVariant(registry, base)) out[count++] = variant;

  // Built-in rows only. ensureJpFallback() loads the companion for a Japanese book just when the
  // row's own face lacks CJK, which is true of the built-ins by definition (selectedFontCovers
  // treats them as Latin-complete and CJK-less) but unknowable here for an SD family: coverage
  // lives in its .cpfont interval table and is only readable once resident. An SD row would
  // otherwise offer sizes that a self-sufficient CJK family never renders at.
  //
  // Matched on the normalized key, like every other family comparison here, so a folder named
  // "noto sans jp" also pairs, and on the extension ensureJpFallback() ranks first, so the size
  // offered is the size that loads.
  if (builtinRow && count < cap) {
    const char* wanted = fontFamily == CrossPointSettings::NOTOSANS ? "notosansjp" : "notoserifjp";
    for (const auto& fam : registry->getFamilies()) {
      if (normalizedFamilyKey(fam.name) == wanted) {
        out[count++] = &fam;
        break;
      }
    }
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

std::string SdCardFontSystem::resolveSelectedFamily() const {
  const std::string selected = SETTINGS.sdFontFamilyName;
  // A book that needs Japanese keeps the base as-is. The companion ensureJpFallback() is about
  // to load carries this book's text, so standing a coverage variant in here would only make it
  // the second resident SD font -- the pairing that does not reliably fit this heap. Leaving the
  // base built-in also keeps its `preferSans` ranking working, which reads an empty selection.
  if (jpFallbackNeeded_) return selected;
  const std::string base = selected.empty() ? builtinFamilyDirName(SETTINGS.fontFamily) : selected;
  const auto* variant = findCoverageVariant(&registry_, base);
  return variant ? variant->name : selected;
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

  HalMemoryProbe::sample("ensureJpFallback-start");

  // Selected font (built-in, or a Latin-only SD font) can't render Japanese: pair a
  // built-in Noto face with its matching JP extension, then try the other extension.
  const bool preferSans = selected.empty() && SETTINGS.fontFamily == CrossPointSettings::NOTOSANS;
  auto extensionRank = [preferSans](const std::string& name) {
    return normalizedFamilyKey(name) == (preferSans ? "notosansjp" : "notoserifjp") ? 0 : 1;
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
      if (wanted && wanted->pointSize == fallbackManager_.currentPointSize()) {
        HalMemoryProbe::sample("ensureJpFallback-already-loaded");
        HalMemoryProbe::flush("ensureJpFallback");
        return;
      }
    }
    if (!fallbackManager_.loadFamily(*fam, renderer, pointSize)) continue;
    if (loadedFamilyCovers(fallbackManager_, fam->name, 0x3042) &&
        loadedFamilyCovers(fallbackManager_, fam->name, 'a')) {
      LOG_DBG("SDFS", "Companion fallback font: %s", fam->name.c_str());
      HalMemoryProbe::sample("ensureJpFallback-settled");
      HalMemoryProbe::flush("ensureJpFallback");
      return;
    }
    // Loaded fine but doesn't cover both scripts -- not a useful companion.
    fallbackManager_.unloadAll(renderer);
    HalMemoryProbe::sample("after-reject-unload");
  }

  if (!fallbackManager_.currentFamilyName().empty()) fallbackManager_.unloadAll(renderer);
  HalMemoryProbe::sample("ensureJpFallback-none-settled");
  HalMemoryProbe::flush("ensureJpFallback");
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
  // resolveSelectedFamily() reads this flag: a collapsed entry is the base plus a JP companion
  // for a Japanese book and the wider variant for any other, so the selection is re-resolved
  // here rather than only at book open. Called at book/activity boundaries, never mid-render.
  ensureSelectedLoaded(renderer);
  ensureJpFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);
}

void SdCardFontSystem::releaseForImageDecode(GfxRenderer& renderer) {
  const uint32_t freeBefore = ESP.getFreeHeap();
  const uint32_t maxBefore = ESP.getMaxAllocHeap();

  // Drop the companion first, then the selected family. manager_.unloadAll() also removes the
  // size-matched UI fallback registrations before deleting their backing SdCardFont objects.
  jpFallbackNeeded_ = false;
  if (!fallbackManager_.currentFamilyName().empty()) fallbackManager_.unloadAll(renderer);
  if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
  updateGlobalFallback(renderer);

  // Glyph slabs and hot groups are owned by FontCacheManager rather than either SD-font manager.
  // Release them too so the decoder receives one coalesced block, not merely enough total bytes.
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();

  LOG_INF("SDFS", "Image decode font release: free %u->%u, maxAlloc %u->%u", freeBefore, ESP.getFreeHeap(), maxBefore,
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
