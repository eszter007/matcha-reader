#include "SdCardFontSystem.h"

#include <Arduino.h>
#include <EpdFontFamily.h>
#include <Fnv1a.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <I18n.h>
#include <IndicScripts.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <TtfEpdFont.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>

#include "CrossPointSettings.h"
#include "ReaderFontSizes.h"
#include "fontIds.h"

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

// Directory name of the built-in reader family, so a variant can name it as its base even
// though it is compiled in rather than discovered on the card.
constexpr const char* BUILTIN_FAMILY_DIR_NAME = "NotoSerif";

// Normalized key of the Noto row a CJK extension cut pairs with: the built-in serif row, or the
// SD NotoSans family. Empty for any other row.
std::string notoRowStem(const char* sdFamilyName) {
  if (sdFamilyName == nullptr || sdFamilyName[0] == '\0') return "notoserif";
  const std::string key = normalizedFamilyKey(sdFamilyName);
  return key == "notosans" ? key : std::string();
}

#if CROSSPOINT_VECTOR_FONTS
int computeTtfFontId(const char* familyName, const uint32_t contentKey, const uint8_t pointSize,
                     const bool uiFallback = false) {
  static constexpr char VERSION_SALT[] = "TTF-content-v1";
  const char* name = familyName ? familyName : "";
  uint32_t hash = fnv1a::hash(VERSION_SALT, sizeof(VERSION_SALT));
  hash = fnv1a::hash(name, strlen(name) + 1, hash);
  const uint8_t fields[] = {static_cast<uint8_t>(contentKey >> 24),
                            static_cast<uint8_t>(contentKey >> 16),
                            static_cast<uint8_t>(contentKey >> 8),
                            static_cast<uint8_t>(contentKey),
                            pointSize,
                            static_cast<uint8_t>(uiFallback)};
  hash = fnv1a::hash(fields, sizeof(fields), hash);
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

#ifdef BOARD_HAS_PSRAM
// Read big-endian uint16 / uint32 from an sfnt header buffer.
uint16_t readBe16(const uint8_t* p) { return (static_cast<uint16_t>(p[0]) << 8) | p[1]; }
uint32_t readBe32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

constexpr uint32_t sfntTag(const char a, const char b, const char c, const char d) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(d));
}

// Compute the prefix byte count needed to cover the font's lookup/metric tables
// (cmap, loca, hmtx, kern, etc.) without caching the multi-MB glyf/CFF body.
// Falls back to min(fileLen, maxPrefix) on TTC collections or unexpected layouts.
size_t computeTtfPrefixBytes(HalFile& file, const size_t fileLen, const size_t maxPrefix) {
  const size_t fallback = std::min(fileLen, maxPrefix);
  uint8_t hdr[12];
  if (!file.seek(0) || file.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return fallback;

  const uint32_t version = readBe32(hdr);
  if (version != 0x00010000u && version != sfntTag('O', 'T', 'T', 'O') && version != sfntTag('t', 'r', 'u', 'e') &&
      version != sfntTag('t', 'y', 'p', '1')) {
    return fallback;  // TTC ('ttcf') or unrecognized container
  }
  const uint16_t numTables = readBe16(hdr + 4);
  if (numTables == 0 || numTables > 128) return fallback;

  const size_t dirEnd = sizeof(hdr) + static_cast<size_t>(numTables) * 16u;
  if (dirEnd > fileLen) return fallback;

  size_t neededEnd = dirEnd;
  bool foundCoreTable = false;
  uint8_t records[16 * 16];  // 256 B stack chunk (16 table records)
  uint16_t remaining = numTables;
  while (remaining > 0) {
    const uint16_t batch = remaining < 16 ? remaining : 16;
    const size_t bytes = static_cast<size_t>(batch) * 16u;
    if (file.read(records, bytes) != static_cast<int>(bytes)) return fallback;
    for (uint16_t i = 0; i < batch; ++i) {
      const uint8_t* rec = records + static_cast<size_t>(i) * 16u;
      const uint32_t tag = readBe32(rec);
      const uint32_t offset = readBe32(rec + 8);
      const uint32_t length = readBe32(rec + 12);
      if (offset > fileLen || length > fileLen - offset) continue;
      const size_t end = static_cast<size_t>(offset) + length;
      switch (tag) {
        case sfntTag('h', 'e', 'a', 'd'):
        case sfntTag('h', 'h', 'e', 'a'):
        case sfntTag('m', 'a', 'x', 'p'):
        case sfntTag('O', 'S', '/', '2'):
        case sfntTag('c', 'm', 'a', 'p'):
        case sfntTag('l', 'o', 'c', 'a'):
        case sfntTag('h', 'm', 't', 'x'):
        case sfntTag('k', 'e', 'r', 'n'):
        case sfntTag('f', 'v', 'a', 'r'):
        case sfntTag('a', 'v', 'a', 'r'):
        case sfntTag('c', 'v', 't', ' '):
        case sfntTag('f', 'p', 'g', 'm'):
        case sfntTag('p', 'r', 'e', 'p'):
          if (end > maxPrefix) {
            if (offset < maxPrefix) return fallback;
            break;
          }
          neededEnd = std::max(neededEnd, end);
          foundCoreTable = true;
          break;
        default:
          break;
      }
    }
    remaining -= batch;
  }
  return foundCoreTable ? neededEnd : fallback;
}
#endif
#endif  // CROSSPOINT_VECTOR_FONTS

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

void SdCardFontSystem::begin(GfxRenderer& renderer, const EpdFontFamily& small, const EpdFontFamily& ui10,
                             const EpdFontFamily& ui12) {
  builtinUiFonts_[0] = &small;
  builtinUiFonts_[1] = &ui10;
  builtinUiFonts_[2] = &ui12;
  // The built-in jōyō-subset fallback installed by main.cpp -- the floor we return to when
  // no CJK-capable SD font is available.
  defaultGlobalFallback_ = EpdFontFamily::getGlobalFallback();
  refreshRegistry();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  // Load whatever the saved selection resolves to. Shared with ensureLoaded() so a selection
  // written by an older build is migrated, and a coverage variant stands in, at boot too.
  ensureSelectedLoaded(renderer, /*registryWasDirty=*/false);

  ensureCjkFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);

  // The optional UI faces must fit alongside the saved reader font.
  applyUiFont(renderer, SETTINGS.uiFontFamilyName);
  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::refreshIfDirty() {
  if (registryDirty_.exchange(false, std::memory_order_acquire)) refreshRegistry();
}

bool SdCardFontSystem::rediscoverIfDirty() {
  if (!registryDirty_.exchange(false, std::memory_order_acquire)) return false;
  LOG_DBG("SDFS", "Re-discovering fonts");
  refreshRegistry();
  return true;
}

void SdCardFontSystem::refreshRegistry() {
#if CROSSPOINT_VECTOR_FONTS
  scanSavedFamilies(/*automaticRetry=*/false);
#else
  registry_.discover();
#endif
}

#if CROSSPOINT_VECTOR_FONTS
void SdCardFontSystem::scanSavedFamilies(const bool automaticRetry) {
  // The same scan serves both roles that were already waiting for discovery.
  readerFontRetrySpent_ = automaticRetry && (readerFontRetrySpent_ || readerFontUnavailable_);
  uiFontRetrySpent_ = automaticRetry && (uiFontRetrySpent_ || uiFontRecoveryPending_);

  SdCardFontRegistry::SavedFamilies saved{SETTINGS.sdFontFamilyName, SETTINGS.uiFontFamilyName,
                                          /*retain=*/automaticRetry};
  registry_.discover(&saved);

  readerFontUnavailable_ = saved.readerUnavailable;
  if (saved.uiUnavailable) {
    uiFontRecoveryPending_ = true;
  } else if (saved.ui.empty() || !registry_.findFamily(saved.ui)) {
    uiFontRecoveryPending_ = false;
  }
}
#endif

void SdCardFontSystem::handleMissingFamily() const {
#if CROSSPOINT_VECTOR_FONTS
  if (readerFontUnavailable_) {
    LOG_DBG("SDFS", "SD font scan incomplete; keeping selection: %s", SETTINGS.sdFontFamilyName);
    return;
  }
#endif
  LOG_DBG("SDFS", "SD font family not found on card: %s (clearing)", SETTINGS.sdFontFamilyName);
  SETTINGS.clearSdFontFamily();
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, [[maybe_unused]] const bool retryUnavailable) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = rediscoverIfDirty();

#if CROSSPOINT_VECTOR_FONTS
  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const char* savedUiFamily = SETTINGS.uiFontFamilyName;
  if (!*savedUiFamily || (uiFontRecoveryPending_ && strcmp(savedUiFamily, activeUiFontName_) == 0)) {
    uiFontRecoveryPending_ = false;
  }
  if (readerFontUnavailable_ && (!*wantedFamily || registry_.findFamily(wantedFamily))) {
    readerFontUnavailable_ = false;
  }
  const bool retryReader = retryUnavailable && !registryWasDirty && readerFontUnavailable_ && !readerFontRetrySpent_;
  if (retryReader) {
    scanSavedFamilies(/*automaticRetry=*/true);
  }
#endif

  ensureSelectedLoaded(renderer, registryWasDirty);
  ensureCjkFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);

#if CROSSPOINT_VECTOR_FONTS
  if (retryUnavailable && uiFontRecoveryPending_) {
    if (!registryWasDirty && !retryReader && !uiFontRetrySpent_ && !registry_.findFamily(savedUiFamily)) {
      // A UI-only retry must not force a working reader font to reload.
      scanSavedFamilies(/*automaticRetry=*/true);
    }
    if (registry_.findFamily(savedUiFamily)) {
      // Attempt once after the reader loads, so it has first claim on memory.
      uiFontRecoveryPending_ = false;
      applyUiFont(renderer, savedUiFamily);
    }
  }
#endif
}

void SdCardFontSystem::ensureSelectedLoaded(GfxRenderer& renderer, const bool registryWasDirty) {
  // A hidden CJK extension family must never be the SELECTED reader font: it is
  // the CJK half of a Noto row and is hidden from both pickers, so a selection
  // carried over from an older build would be stuck and would render Latin books
  // in the CJK face. Revert it to the matching built-in and let
  // ensureCjkFallback() bring the extension back as the companion where needed.
  if (SETTINGS.sdFontFamilyName[0] != '\0' && isHiddenCjkExtension(SETTINGS.sdFontFamilyName, &registry_)) {
    const bool sans = normalizedFamilyKey(SETTINGS.sdFontFamilyName).rfind("notosans", 0) == 0;
    const SdCardFontFamilyInfo* sansRow = sans ? notoSansRowFamily(&registry_) : nullptr;
    LOG_INF("SDFS", "Reverting hidden CJK extension selection '%s' to its Noto row", SETTINGS.sdFontFamilyName);
    SETTINGS.fontFamily = CrossPointSettings::NOTOSERIF;
    if (sansRow) {
      strncpy(SETTINGS.sdFontFamilyName, sansRow->name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
    } else {
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
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
      SETTINGS.fontFamily = CrossPointSettings::NOTOSERIF;
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
      LOG_DBG("SDFS", "SD font family disappeared: %s", wantedFamily.c_str());
      manager_.unloadAll(renderer);
      if (standingIn) {
        LOG_DBG("SDFS", "Keeping base selection");
      } else {
        handleMissingFamily();
      }
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
#if CROSSPOINT_VECTOR_FONTS
    // An opportunistic scan can lose a family whose open source files still work.
    if (readerFontUnavailable_ && !registryWasDirty && ttf_ && ttfFamily_ == wantedFamily &&
        ttfPointSize_ == SETTINGS.fontPointSize) {
      return;
    }
#endif
    LOG_DBG("SDFS", "SD font family not found: %s", wantedFamily.c_str());
    if (standingIn) {
      LOG_DBG("SDFS", "Keeping base selection");
    } else {
      handleMissingFamily();
    }
  }
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  // Probe the already-loaded reader-size font before paying for the UI sizes:
  // resolveTextFontId only redirects on codepoints the built-in UI fonts lack,
  // so a family with no coverage beyond theirs can never act as a fallback and
  // its UI sizes would be dead weight in RAM.
  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;
  // One representative codepoint per script the built-in fonts may lack,
  // matching the non-Latin interval presets the SD-font converter ships
  // (docs/sd-card-fonts.md).
  static constexpr uint32_t kFallbackProbes[] = {
      0x4E00,  // CJK Unified Ideographs (Han)
      0x3042,  // Hiragana
      0x30A2,  // Katakana
      0xAC00,  // Hangul Syllables
      0x03B1,  // Greek
      0x0430,  // Cyrillic
      0x05D0,  // Hebrew
      0x0627,  // Arabic
      0x0531,  // Armenian
      0x10D0,  // Georgian
      0x1200,  // Ethiopic
      0x13A0,  // Cherokee
      0x2D30,  // Tifinagh
      0x0259,  // IPA Extensions (ə)
  };
  bool hasFallbackScript = false;
  for (const uint32_t cp : kFallbackProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasFallbackScript = true;
      break;
    }
  }
  // Every Indic script too.
  hasFallbackScript = hasFallbackScript ||
                      std::any_of(std::begin(indic::SCRIPTS), std::end(indic::SCRIPTS),
                                  [&](const indic::ScriptInfo& s) { return readerIt->second.hasCodepoint(s.probe); });
  if (!hasFallbackScript) {
    LOG_DBG("SDFS", "%s has no fallback-script coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  // Each extra size of a CJK family is another 12-27 KB of resident tables, three of them for the
  // UI rows. In a book the page build needs that heap (it aborted with 7 KB free); on Home a cover
  // thumbnail needs it. Lend the reader-size font to the UI rows, scaled, as the companion does.
  // A family without CJK keeps real UI cuts: its tables are small.
  const bool cjkFamily = readerIt->second.hasCodepoint(0x4E00) || readerIt->second.hasCodepoint(0x3042) ||
                         readerIt->second.hasCodepoint(0xAC00);
  if (cjkScript_ != CjkScript::None || cjkFamily) {
    if (manager_.extraCount() > 0) {
      const uint32_t freeBefore = ESP.getFreeHeap();
      const size_t extras = manager_.extraCount();
      manager_.unloadExtras(renderer);
      LOG_DBG("SDFS", "Dropped %u UI sizes of %s (free %u -> %u)", static_cast<unsigned>(extras), familyName.c_str(),
              static_cast<unsigned>(freeBefore), static_cast<unsigned>(ESP.getFreeHeap()));
    }
    lendCompanionToUiFonts(renderer, manager_.getFontId(familyName), manager_.currentPointSize());
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
  // The companion, or a selected CJK family, is resident at ONE size -- the reader's in a book,
  // 12 pt for the UI alone -- and the CJK cuts ship nothing below 12 pt. Unmapped, an 8 or 10 pt UI font takes each
  // glyph the built-in subset lacks from the global fallback at that size: a Chinese title, a
  // menu row or a rare kanji in a chapter name draws half its characters larger than the rest.
  // So every UI font is sent to the resident font, scaled to its own size: no second table in
  // RAM, and one size across the string.
  if (sdFontId == 0 || pointSize == 0) return;
  for (const auto& ui : kUiFontSizes) {
    const auto scale = static_cast<uint16_t>(ui.pointSize * 256u / pointSize);
    renderer.setFallbackFont(ui.fontId, sdFontId, scale, scale);
  }
}

void SdCardFontSystem::releaseForStorage(GfxRenderer& renderer) {
  releaseAllResidentFonts(renderer);
  applyUiFont(renderer, "");
}

const char* SdCardFontSystem::activeUiFontName() const {
#if CROSSPOINT_VECTOR_FONTS
  if (uiTtfFailed()) return "";
#endif
  return activeUiFontName_;
}

bool SdCardFontSystem::supportsUiFont(const SdCardFontFamilyInfo& family) {
  if (family.vector) return CROSSPOINT_VECTOR_FONTS;
  return std::all_of(std::begin(kUiFontSizes), std::end(kUiFontSizes),
                     [&](const UiFontSize& ui) { return family.hasSize(ui.pointSize); });
}

bool SdCardFontSystem::loadUiCpfonts(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  static constexpr size_t kMinInternalFree = 50 * 1024;
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    const auto& ui = kUiFontSizes[i];
    renderer.removeFont(ui.fontId);
    if (!uiCpfonts_.loadFamilyVariant(family, renderer, ui.pointSize, ui.fontId, builtinUiFonts_[i])) return false;
    if (HalMemory::getInternalHeap().freeBytes < kMinInternalFree) {
      LOG_ERR("SDFS", "Not enough internal heap for the UI bitmap font");
      return false;
    }
  }
  return true;
}

// The UI fallbacks (renderer.setFallbackFont on the UI font ids) are keyed by id, not by font, so
// they stay in place for a custom UI font: a CJK title still redirects to the size-matched SD font.
bool SdCardFontSystem::applyUiFont(GfxRenderer& renderer, const char* want) {
#if CROSSPOINT_VECTOR_FONTS
  unloadUiTtf(renderer);
  uiTtfFailed_.store(false, std::memory_order_relaxed);
#endif
  // unloadFonts(), not unloadAll(): that would also clear the renderer's UI fallback mappings.
  uiCpfonts_.unloadFonts(renderer);
  activeUiFontName_[0] = '\0';
  if (*want) {
    const auto* family = registry_.findFamily(want);
    if (family && supportsUiFont(*family)) {
#if CROSSPOINT_VECTOR_FONTS
      const bool loaded = family->vector ? loadUiTtf(*family, renderer) : loadUiCpfonts(*family, renderer);
#else
      const bool loaded = loadUiCpfonts(*family, renderer);
#endif
      if (loaded) {
#if CROSSPOINT_VECTOR_FONTS
        uiFontRecoveryPending_ = false;
#endif
        snprintf(activeUiFontName_, sizeof(activeUiFontName_), "%s", want);
        LOG_DBG("SDFS", "UI font: %s", want);
        return true;
      }
    }
    LOG_ERR("SDFS", "UI font '%s' missing or failed to load; using built-in", want);
#if CROSSPOINT_VECTOR_FONTS
    unloadUiTtf(renderer);
    uiTtfFailed_.store(false, std::memory_order_relaxed);
#endif
    uiCpfonts_.unloadFonts(renderer);
  }
  restoreBuiltinUiFonts(renderer);
  return *want == '\0';
}

void SdCardFontSystem::restoreBuiltinUiFonts(GfxRenderer& renderer) {
  for (size_t i = 0; i < std::size(kUiFontSizes); ++i) {
    renderer.removeFont(kUiFontSizes[i].fontId);
    renderer.insertFont(kUiFontSizes[i].fontId, *builtinUiFonts_[i]);
  }
}

void SdCardFontSystem::rememberLookupMapping(const int primaryFontId, const int sdFontId, const bool companion,
                                             const bool loaded) {
  const auto slot = std::find_if(std::begin(lookupExtras_), std::end(lookupExtras_), [primaryFontId](const auto& e) {
    return e.primaryFontId == primaryFontId || e.primaryFontId == 0;
  });
  if (slot != std::end(lookupExtras_)) *slot = {primaryFontId, sdFontId, companion, loaded};
}

void SdCardFontSystem::lendUiFontsForLookup(GfxRenderer& renderer, const int sdFontId, const uint8_t pointSize,
                                            const uint8_t latinPointSize) {
  if (lookupUiLent_) return;
  for (size_t i = 0; i < std::size(kUiFontSizes); i++) {
    const auto& ui = kUiFontSizes[i];
    const auto previous = renderer.fallbackScaleFor(ui.fontId);
    lookupUiPrevious_[i] = {renderer.fallbackFontFor(ui.fontId), previous.cjk, previous.nonCjk};
    renderer.setFallbackFont(ui.fontId, sdFontId, static_cast<uint16_t>(ui.pointSize * 256u / pointSize),
                             static_cast<uint16_t>(ui.pointSize * 256u / latinPointSize));
  }
  lookupUiLent_ = true;
}

void SdCardFontSystem::borrowFamilyFallbackForLookup(GfxRenderer& renderer, const int sdFontId,
                                                     const EpdFontFamily* builtin) {
  // The companion has Latin of its own, in another face and without italics. Keep the
  // built-in's: a line sent here for one hanzi must not change typeface around it. The font is
  // shared (the UI's 12 pt, the reader's own size), so what it pointed at before is put back
  // when the session ends.
  const auto sdIt = renderer.getFontMap().find(sdFontId);
  for (auto& saved : lookupFallbackSaved_) {
    if (saved.sdFontId == sdFontId) break;
    if (saved.sdFontId != 0 || sdIt == renderer.getFontMap().end()) continue;
    saved = {sdFontId, sdIt->second.getFallback(), sdIt->second.fallbackIsFirstForNonCjk()};
    break;
  }
  renderer.setFamilyFallback(sdFontId, builtin, /*nonCjkFirst=*/true);
}

void SdCardFontSystem::ensureWordLookupFallback(GfxRenderer& renderer, const int primaryFontId,
                                                const uint8_t pointSize) {
  // Tiny intentionally stays on the compact built-in path; avoid loading an SD font for it.
  if (pointSize <= 8) return;
  ensureWordLookupFonts(renderer, primaryFontId, pointSize);
  // After the panel's own font: the variant is optional and must not take the block that one needs.
  loadLookupVariant(renderer, pointSize);
}

void SdCardFontSystem::ensureWordLookupFonts(GfxRenderer& renderer, const int primaryFontId, const uint8_t pointSize) {
  // The selected family when it draws the book's script; otherwise the companion that was loaded
  // because it does not. Without either, the panel takes the companion's reader-size glyphs
  // through the global fallback, and an entry mixes two sizes wherever the built-in subset
  // stops: every line of a Chinese entry, a rare kanji of a Japanese one.
  const std::string& selected = manager_.currentFamilyName();
  const bool selectedCovers = !selected.empty() && loadedFamilyCovers(manager_, selected, cjkProbe());
  const bool useCompanion = !fallbackManager_.currentFamilyName().empty() && !selectedCovers;
  SdCardFontManager& mgr = useCompanion ? fallbackManager_ : manager_;
#if CROSSPOINT_VECTOR_FONTS
  // A TTF reader font is not in the .cpfont manager. In a book with no companion it is the face
  // the reader draws, so the panel borrows it the same way.
  if (!useCompanion && mapTtfForLookup(renderer, primaryFontId, pointSize)) return;
#endif
  if (mgr.currentFamilyName().empty()) return;
  const auto* family = registry_.findFamily(mgr.currentFamilyName());
  if (!family) return;

  const auto builtinIt = renderer.getFontMap().find(primaryFontId);
  const EpdFontFamily* builtin = builtinIt != renderer.getFontMap().end() ? &builtinIt->second : nullptr;

  if (useCompanion) {
    // The companion is already resident at the reader's size. Draw it scaled to the panel's
    // rather than loading a second table beside it: with the selected family, its panel-size
    // cut and the companion all in RAM, that fourth font leaves an X4 too little heap to load
    // the entry's glyphs, and the panel opens blank.
    const int residentId = fallbackManager_.getFontId(family->name);
    const uint8_t residentPt = fallbackManager_.currentPointSize();
    if (residentId != 0 && residentPt != 0 && builtin != nullptr) {
      // One built-in supplies the Latin for every font mapped here (the first to ask, the
      // definition body); the others take it scaled from that size.
      if (lookupLatinPointSize_ == 0) {
        borrowFamilyFallbackForLookup(renderer, residentId, builtin);
        lookupLatinPointSize_ = pointSize;
      }
      renderer.setFallbackFont(primaryFontId, residentId, static_cast<uint16_t>(pointSize * 256u / residentPt),
                               static_cast<uint16_t>(pointSize * 256u / lookupLatinPointSize_));
      rememberLookupMapping(primaryFontId, residentId, /*companion=*/true, /*loaded=*/false);
      lendUiFontsForLookup(renderer, residentId, residentPt, lookupLatinPointSize_);
      return;
    }
  }

  const bool wasResident = mgr.hasSize(pointSize);
  const int sdFontId = mgr.loadFamilyExtraSize(*family, renderer, pointSize);
  if (sdFontId == 0) {
    // No cut at the panel's size on the card: draw the resident reader size scaled to it, as the
    // companion path does. Unmapped, the glyphs reach the panel unscaled through the global
    // fallback, and the ones not yet loaded measure as zero width, so lines overrun the panel.
    const int residentId = mgr.getFontId(family->name);
    const uint8_t residentPt = mgr.currentPointSize();
    if (residentId == 0 || residentPt == 0) return;
    // Latin that reaches this family is scaled too, so it matches the panel's size. Clamped so an
    // odd profile size can never ask for more than double or less than a quarter.
    const uint16_t scale = static_cast<uint16_t>(std::clamp(pointSize * 256u / residentPt, 64u, 512u));
    renderer.setFallbackFont(primaryFontId, residentId, scale, scale);
    rememberLookupMapping(primaryFontId, residentId, /*companion=*/false, /*loaded=*/false);
    return;
  }
  renderer.setFallbackFont(primaryFontId, sdFontId);
  if (builtin != nullptr) {
    if (useCompanion) {
      borrowFamilyFallbackForLookup(renderer, sdFontId, builtin);
    } else {
      renderer.setFamilyFallback(sdFontId, builtin);
    }
  }
  // Remember it for releaseWordLookupFallback(): the font when this call loaded it, and in any
  // case the mapping, which must not outlive a font another primary's release unloads.
  const bool ownedAlready =
      std::any_of(std::begin(lookupExtras_), std::end(lookupExtras_),
                  [sdFontId](const LookupExtra& e) { return e.loaded && e.sdFontId == sdFontId; });
  if (!wasResident || ownedAlready) rememberLookupMapping(primaryFontId, sdFontId, useCompanion, /*loaded=*/true);
  // The panel's footer, reading and tag lines are set in the UI fonts, which in a book borrow
  // the companion at the reader's size (see ensureCjkFallback). Lend them this one for the
  // session instead: it is the panel's own size, and it is already paid for.
  if (useCompanion) lendUiFontsForLookup(renderer, sdFontId, pointSize, pointSize);
}

void SdCardFontSystem::loadLookupVariant(GfxRenderer& renderer, const uint8_t pointSize) {
  // A Chinese entry lists the other script's form beside the headword (說 / 说), and nothing a
  // Simplified book loads draws a traditional-only character: the built-in subset is Japanese,
  // which writes 説. Load the other Chinese cut for the session as the last fallback stop.
  // Once per session: the panel renders on every cursor move, and a load that failed for heap
  // would otherwise rescan the registry and re-read the font each time.
  if (lookupVariantTried_) return;
  lookupVariantTried_ = true;
  const CjkScript script = activeCjkScript();
  const CjkScript other = script == CjkScript::SimplifiedChinese    ? CjkScript::TraditionalChinese
                          : script == CjkScript::TraditionalChinese ? CjkScript::SimplifiedChinese
                                                                    : CjkScript::None;
  if (other == CjkScript::None) return;
  const SdCardFontFamilyInfo* variant = nullptr;
  for (const auto& fam : registry_.getFamilies()) {
    if (extensionScript(fam.name) != other) continue;
    // A sans cut first, as the book's own Noto row is.
    if (!variant || normalizedFamilyKey(fam.name).rfind("notosans", 0) == 0) variant = &fam;
  }
  if (!variant) return;
  // Its packed tables, kern classes and glyph slots: about 16 KB for a sparse CJK cut.
  // The entry's own glyphs are prewarmed after it, so leave them room too.
  static constexpr uint32_t VARIANT_MIN_BLOCK = 32 * 1024;
  static constexpr uint32_t VARIANT_MIN_FREE = 48 * 1024;
  if (ESP.getMaxAllocHeap() < VARIANT_MIN_BLOCK || ESP.getFreeHeap() < VARIANT_MIN_FREE) {
    LOG_DBG("SDFS", "Skipping lookup variant %s (free=%u maxAlloc=%u)", variant->name.c_str(), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return;
  }
  if (!variantManager_.loadFamily(*variant, renderer, pointSize)) {
    LOG_ERR("SDFS", "Lookup variant %s failed to load at %u", variant->name.c_str(), pointSize);
    return;
  }
  const auto it = renderer.getFontMap().find(variantManager_.getFontId(variant->name));
  if (it == renderer.getFontMap().end()) {
    variantManager_.unloadFonts(renderer);
    return;
  }
  EpdFontFamily::setVariantFallback(&it->second);
  LOG_DBG("SDFS", "Lookup variant: %s", variant->name.c_str());
}

void SdCardFontSystem::unloadLookupVariant(GfxRenderer& renderer) {
  if (variantManager_.currentFamilyName().empty()) return;
  EpdFontFamily::setVariantFallback(nullptr);  // before the font it points at is freed
  variantManager_.unloadFonts(renderer);
}

void SdCardFontSystem::releaseWordLookupFallback(GfxRenderer& renderer) {
  unloadLookupVariant(renderer);
#if CROSSPOINT_VECTOR_FONTS
  // With PSRAM the panel's TTF faces stay for the next lookup (their glyph caches live there);
  // mapTtfForLookup() reuses them, and unloadTtf() frees them with the reader face. Without PSRAM
  // their glyph caches would hold internal heap.
  if (HalMemory::getPsramHeap().totalBytes == 0) releaseTtfLookup(renderer);
#endif
  lookupVariantTried_ = false;
  if (lookupUiLent_) {
    for (size_t i = 0; i < std::size(kUiFontSizes); i++) {
      if (lookupUiPrevious_[i].fontId != 0) {
        renderer.setFallbackFont(kUiFontSizes[i].fontId, lookupUiPrevious_[i].fontId, lookupUiPrevious_[i].scale,
                                 lookupUiPrevious_[i].nonCjkScale);
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
  lookupLatinPointSize_ = 0;
  for (auto& extra : lookupExtras_) {
    if (extra.primaryFontId == 0) continue;
    renderer.clearFallbackFont(extra.primaryFontId);
    // a no-op the second time for a shared font
    if (extra.loaded) (extra.companion ? fallbackManager_ : manager_).unloadExtra(extra.sdFontId, renderer);
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
  // The resident family is whatever ensureSelectedLoaded() resolved for the selection: the named
  // family itself, or a stand-in for it -- a coverage variant for the built-in (empty name), or
  // the book's script cut for a Noto Sans CJK row (NotoSansSC for a saved NotoSansJP). 0 (use the
  // built-in) when nothing is resident. Runs in the page render loop via getReaderFontId(), so it
  // must not allocate.
  (void)familyName;
  const std::string& resident = manager_.currentFamilyName();
  return resident.empty() ? 0 : manager_.getFontId(resident);
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

const SdCardFontFamilyInfo* SdCardFontSystem::notoSansRowFamily(const SdCardFontRegistry* registry) {
  if (registry == nullptr) return nullptr;
  const SdCardFontFamilyInfo* best = nullptr;
  int bestRank = 0;
  for (const auto& fam : registry->getFamilies()) {
    const std::string key = normalizedFamilyKey(fam.name);
    if (key == "notosans") return &fam;
    if (key.rfind("notosans", 0) != 0) continue;
    const CjkScript script = extensionScript(fam.name);
    const int rank = script == CjkScript::Japanese             ? 1
                     : script == CjkScript::SimplifiedChinese  ? 2
                     : script == CjkScript::TraditionalChinese ? 3
                                                               : 0;
    if (rank != 0 && (best == nullptr || rank < bestRank)) {
      best = &fam;
      bestRank = rank;
    }
  }
  return best;
}

bool SdCardFontSystem::isHiddenCjkExtension(const std::string& familyName, const SdCardFontRegistry* registry) {
  if (!isBuiltinCjkExtension(familyName)) return false;
  if (normalizedFamilyKey(familyName).rfind("notoserif", 0) == 0) return true;
  const SdCardFontFamilyInfo* row = notoSansRowFamily(registry);
  return row == nullptr || normalizedFamilyKey(row->name) != normalizedFamilyKey(familyName);
}

std::string SdCardFontSystem::pickerLabel(const std::string& familyName, const SdCardFontRegistry* registry) {
  const SdCardFontFamilyInfo* row = notoSansRowFamily(registry);
  if (row != nullptr && normalizedFamilyKey(row->name) == normalizedFamilyKey(familyName)) return tr(STR_NOTO_SANS);
  return familyName;
}

uint8_t SdCardFontSystem::readerStandInFamilies(const SdCardFontRegistry* registry, const char* sdFamilyName,
                                                const uint8_t /*fontFamily*/, const SdCardFontFamilyInfo** out,
                                                const uint8_t cap) {
  if (!registry || !out || cap == 0) return 0;
  uint8_t count = 0;

  const bool builtinRow = sdFamilyName == nullptr || sdFamilyName[0] == '\0';
  const std::string base = builtinRow ? BUILTIN_FAMILY_DIR_NAME : sdFamilyName;
  if (const auto* variant = findCoverageVariant(registry, base)) out[count++] = variant;

  // Noto rows only (the built-in serif and an SD NotoSans, both Latin faces without CJK).
  // ensureCjkFallback() loads the companion for a CJK book just when the row's own face lacks
  // CJK, which is unknowable here for any other SD family: coverage
  // lives in its .cpfont interval table and is only readable once resident. An SD row would
  // otherwise offer sizes that a self-sufficient CJK family never renders at.
  //
  // Matched on the normalized key, like every other family comparison here, so a folder named
  // "noto sans jp" also pairs. The JP cut is preferred when several extensions are installed;
  // every cut ships the same sizes, so whichever ensureCjkFallback() picks loads at that size.
  const std::string wantedStem = notoRowStem(sdFamilyName);
  if (!wantedStem.empty() && count < cap) {
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
  if (baseKey == "notoserif") return true;
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

// Does the face this row renders with ship `pt` itself? An empty name is a built-in family, which
// has real cuts at BUILTIN_NATIVE_POINT_SIZES only: its other sizes are the 14pt face enlarged, so
// a stand-in on the card that ships the size is the better face and is preferred.
bool SdCardFontSystem::faceShipsSize(const std::string& familyName, const uint8_t pt) const {
  if (familyName.empty()) {
    return std::find(std::begin(BUILTIN_NATIVE_POINT_SIZES), std::end(BUILTIN_NATIVE_POINT_SIZES), pt) !=
           std::end(BUILTIN_NATIVE_POINT_SIZES);
  }
  const auto* family = registry_.findFamily(familyName);
  return family && family->hasSize(pt);
}

std::string SdCardFontSystem::bookScriptCut(const SdCardFontRegistry* registry, const std::string& selected,
                                            const CjkScript bookScript) {
  const CjkScript selectedScript = extensionScript(selected);
  if (registry == nullptr || selectedScript == CjkScript::None || bookScript == CjkScript::None ||
      selectedScript == bookScript || normalizedFamilyKey(selected).rfind("notosans", 0) != 0) {
    return selected;
  }
  for (const auto& fam : registry->getFamilies()) {
    if (extensionScript(fam.name) == bookScript && normalizedFamilyKey(fam.name).rfind("notosans", 0) == 0) {
      return fam.name;
    }
  }
  return selected;
}

std::vector<uint8_t> SdCardFontSystem::selectablePointSizes(const SdCardFontRegistry* registry,
                                                            const CjkScript bookScript) {
  // A book in another script than the row's saved cut renders with that script's cut alone, so
  // only its sizes change the page: JP's 20pt offered for a Chinese book drew SC's 18.
  const std::string cut = bookScriptCut(registry, SETTINGS.sdFontFamilyName, bookScript);
  if (cut != SETTINGS.sdFontFamilyName) {
    if (const auto* family = registry->findFamily(cut)) return family->availableSizes();
  }
  const SdCardFontFamilyInfo* standIns[MAX_STAND_INS];
  const uint8_t count =
      readerStandInFamilies(registry, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily, standIns, MAX_STAND_INS);
  return readerFontPointSizes(registry, SETTINGS.sdFontFamilyName, standIns, count);
}

std::string SdCardFontSystem::resolveSelectedFamily() const {
  const std::string selected = SETTINGS.sdFontFamilyName;
  // A book that needs CJK keeps the base as-is. The companion ensureCjkFallback() is about
  // to load carries this book's text, so standing a coverage variant in here would only make it
  // the second resident SD font -- the pairing that does not reliably fit this heap. Leaving the
  // base built-in also keeps its `preferSans` ranking working, which reads an empty selection.
  if (cjkFallbackNeeded()) {
    // A Noto Sans CJK cut standing in for a missing NotoSans family covers every CJK book well
    // enough that no companion is loaded, so it would set a Chinese book in Japanese forms. The
    // cut for the book's own script takes over when it is installed.
    return bookScriptCut(&registry_, selected, activeCjkScript());
  }
  const std::string base = selected.empty() ? BUILTIN_FAMILY_DIR_NAME : selected;
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
  //
  // Except a CJK extension at a size the built-in row draws itself (its 14pt face enlarged): the
  // extension's CJK tables cost ~56 KB of resident RAM on an X4 for a Latin book, which is what
  // left a reader with the Bluetooth radio up at under 5 KB. A coverage variant is Latin-sized.
  const bool rowDrawsSize =
      selected.empty() && std::find(std::begin(BUILTIN_READER_POINT_SIZES), std::end(BUILTIN_READER_POINT_SIZES),
                                    SETTINGS.fontPointSize) != std::end(BUILTIN_READER_POINT_SIZES);
  for (uint8_t i = 0; i < count; i++) {
    if (!standIns[i] || !faceShipsSize(standIns[i]->name, SETTINGS.fontPointSize)) continue;
    if (rowDrawsSize && extensionScript(standIns[i]->name) != CjkScript::None) continue;
    return standIns[i]->name;
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
  // A face with the script's kana but not every kanji (a handwriting subset) borrows the rest from
  // the 12pt built-in: draw those at the face's own size.
  if (const int selectedId = selected.empty() ? 0 : manager_.getFontId(selected)) {
    const bool partial = !needsCompanion && selectedHasCjk && !loadedFamilyCovers(manager_, selected, 0x6F22);  // 漢
    renderer.setFaceScale(selectedId, 256, 256, /*sizeFallbacks=*/false, /*matchOwnEm=*/partial);
  }
  if (!needsCompanion) {
#if CROSSPOINT_VECTOR_FONTS
    unloadCompanionTtf(renderer);
#endif
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
  const bool preferSans = notoRowStem(selected.c_str()) == "notosans";
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
#if CROSSPOINT_VECTOR_FONTS
    if (fam->vector) {
      if (companionTtf_ && companionTtfFamily_ == fam->name && companionTtfPointSize_ == loadPt) return;
      if (!fallbackManager_.currentFamilyName().empty()) fallbackManager_.unloadAll(renderer);
      if (loadCompanionTtf(*fam, renderer, loadPt) && companionTtf_->coversCodepoint(cjkProbe()) &&
          companionTtf_->coversCodepoint('a')) {
        LOG_DBG("SDFS", "Companion fallback font (vector): %s", fam->name.c_str());
        if (!selectedHasCjk) lendCompanionToUiFonts(renderer, companionTtfFontId_, loadPt);
        return;
      }
      unloadCompanionTtf(renderer);
      continue;
    }
    unloadCompanionTtf(renderer);
#endif
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
#if CROSSPOINT_VECTOR_FONTS
  unloadCompanionTtf(renderer);
#endif
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
#if CROSSPOINT_VECTOR_FONTS
  if (companionTtfFontId_ != 0) {
    target = &renderer.getFontMap().at(companionTtfFontId_);
  } else
#endif
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
  ensureSelectedLoaded(renderer, rediscoverIfDirty());
  // A book's saved font can load before its script is known, with the UI sizes beside it, and
  // stays resident when the script turns out to be CJK; drop those sizes now.
  if (script != CjkScript::None && manager_.extraCount() > 0) setupUiFallbacks(renderer);
  ensureCjkFallback(renderer, SETTINGS.fontPointSize);
  updateGlobalFallback(renderer);
}

void SdCardFontSystem::setUiCjkNeeded(GfxRenderer& renderer, const CjkScript script) {
  if (uiCjkScript_ == script) return;
  LOG_DBG("SDFS", "UI CJK script: %d", static_cast<int>(script));
  uiCjkScript_ = script;
  ensureSelectedLoaded(renderer, rediscoverIfDirty());
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
  // uiCjkScript_ is a screen's request, not policy: forget it with the companion, or the next
  // setUiCjkNeeded() for the same script would return early and leave the glyphs missing.
  uiCjkScript_ = CjkScript::None;
  unloadLookupVariant(renderer);
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
#if CROSSPOINT_VECTOR_FONTS
  if (companionTtfFontId_ != 0) return companionTtfFontId_;
#endif
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

bool SdCardFontSystem::loadCompanionTtf(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                        const uint8_t pointSize) {
  unloadCompanionTtf(renderer);
  if (family.files.empty()) return false;
  char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
  // Regular only: bold and italic come from the wght axis or synthesis.
  const SdCardFontFileInfo* regular = &family.files.front();
  for (const auto& f : family.files) {
    if (f.style == 0) regular = &f;
  }
  if (!family.filePath(*regular, path, sizeof(path)) ||
      !openTtfSourceInto(companionTtfSource_, path, /*keepResident=*/true, /*primary=*/true))
    return false;
  companionTtf_ = makeUniqueNoThrow<TtfEpdFont>();
  if (!companionTtf_) {
    LOG_ERR("SDFS", "OOM: companion TtfEpdFont for %s", family.name.c_str());
    unloadCompanionTtf(renderer);
    return false;
  }
  TtfSource& s = companionTtfSource_;
  if (s.streamed) {
    companionTtf_->addStreamSource(0, &SdCardFontSystem::prefixRead, &s, s.size);
  } else {
    companionTtf_->addResidentSource(0, s.bytes.data(), static_cast<uint32_t>(s.bytes.size()));
  }
  // Same budget reasoning as the selected TTF (loadTtfFamily): a CJK page faults 300+ glyphs.
  const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
  if (!companionTtf_->load(pointSize, /*twoBit=*/true, havePsram ? 1024 * 1024 : 32 * 1024, havePsram ? 4096 : 768)) {
    LOG_ERR("SDFS", "FreeInkFont could not parse companion %s", family.name.c_str());
    unloadCompanionTtf(renderer);
    return false;
  }
  companionTtf_->build(" ");
  companionTtfFontId_ =
      computeTtfFontId((family.name + "\x01companion").c_str(), companionTtf_->contentKey(), pointSize);
  renderer.insertFont(companionTtfFontId_, companionTtf_->family());
  renderer.registerTtfFont(companionTtfFontId_, companionTtf_.get());
  companionTtfFamily_ = family.name;
  companionTtfPointSize_ = pointSize;
  return true;
}

void SdCardFontSystem::unloadCompanionTtf(GfxRenderer& renderer) {
  if (companionTtfFontId_ != 0) {
    renderer.unregisterTtfFont(companionTtfFontId_);
    renderer.removeFont(companionTtfFontId_);
    companionTtfFontId_ = 0;
  }
  companionTtf_.reset();
  // Only a streamed companion holds an open file; close() asserts on one that was never opened.
  if (companionTtfSource_.file) companionTtfSource_.file.close();
#ifdef BOARD_HAS_PSRAM
  releaseBlockCache(companionTtfSource_);
#endif
  companionTtfSource_.bytes.clear();
  companionTtfSource_.present = false;
  companionTtfSource_.streamed = false;
  companionTtfFamily_.clear();
  companionTtfPointSize_ = 0;
}

SdCardFontSystem::TtfSource* SdCardFontSystem::unusedTtfSources(const TtfSource* other) {
  return other == sourceSlots_[0] ? sourceSlots_[1] : sourceSlots_[0];
}

bool SdCardFontSystem::canShareTtfSources(const TtfSource* sources) const {
  if (!sources) return false;
  const uint32_t generation = sourceSlotGenerations_[sources == sourceSlots_[1]];
  return generation != 0 && generation == registry_.scanGeneration();
}

void SdCardFontSystem::releaseTtfSources(TtfSource*& sources, const TtfSource* other) {
  if (!sources || sources == other) {
    sources = nullptr;
    return;
  }
  sourceSlotGenerations_[sources == sourceSlots_[1]] = 0;
  for (uint8_t st = 0; st < 4; ++st) {
    TtfSource& s = sources[st];
    s.bytes.clear();
    freeink::font::PsramVector<uint8_t>().swap(s.bytes);  // actually release
    if (s.file) s.file.close();
#ifdef BOARD_HAS_PSRAM
    s.prefixWanted.store(0, std::memory_order_relaxed);
    s.prefixReady.store(false, std::memory_order_relaxed);
    releaseBlockCache(s);
#endif
    s.streamed = false;
    s.size = 0;
    s.present = false;
  }
  sources = nullptr;
}

bool SdCardFontSystem::mapTtfForLookup(GfxRenderer& renderer, const int primaryFontId, const uint8_t pointSize) {
  if (!ttf_ || ttfFamily_.empty()) return false;
  int id = ttfPointSize_ == pointSize ? ttfFontId_ : 0;  // the reader face at the panel's size
  for (const auto& face : ttfLookup_) {
    if (id == 0 && face.font && face.pointSize == pointSize) id = face.fontId;
  }
  if (id == 0) {
    const auto free =
        std::find_if(std::begin(ttfLookup_), std::end(ttfLookup_), [](const TtfLookupFace& f) { return !f.font; });
    if (free == std::end(ttfLookup_)) return false;
    auto f = makeUniqueNoThrow<TtfEpdFont>();
    if (!f) {
      LOG_ERR("SDFS", "OOM: TtfEpdFont for lookup @%upt", pointSize);
      return false;
    }
    // A sized variant of the reader face, sharing its FreeType faces. A definition is a few
    // hundred glyphs at most.
    if (!f->loadVariantOf(*ttf_, pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/64 * 1024, /*maxGlyphs=*/512)) {
      return false;
    }
    // Salted like the UI faces: the panel size can equal the reader's or a UI size.
    id = computeTtfFontId((ttfFamily_ + "\x01lookup").c_str(), ttf_->contentKey(), pointSize);
    renderer.insertFont(id, f->family());
    renderer.registerTtfFont(id, f.get());
    *free = {std::move(f), id, pointSize};
  }
  renderer.setFallbackFont(primaryFontId, id);
  // The release clears this mapping; releaseTtfLookup() frees the faces.
  rememberLookupMapping(primaryFontId, id, /*companion=*/false, /*loaded=*/false);
  return true;
}

void SdCardFontSystem::releaseTtfLookup(GfxRenderer& renderer) {
  for (auto& face : ttfLookup_) {
    if (!face.font) continue;
    renderer.unregisterTtfFont(face.fontId);
    renderer.removeFont(face.fontId);
    face = {};
  }
}

void SdCardFontSystem::unloadTtf(GfxRenderer& renderer) {
  releaseTtfLookup(renderer);  // variants of ttf_: they borrow its FreeType faces
  if (ttfFamily_.empty() && ttfFontId_ == 0 && ttfUiIds_.empty()) return;
  clearTtfUiFallbacks(renderer);
  if (ttfFontId_ != 0) {
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);  // drop from the renderer's fontMap
  }
  ttf_.reset();  // frees the FT faces first (they read ttfSources_)
  releaseTtfSources(ttfSources_, uiTtfSources_);
  ttfFamily_.clear();
  ttfFontId_ = 0;
  ttfPointSize_ = 0;
}

#ifdef BOARD_HAS_PSRAM
namespace {
// Outline block caches across every streamed source: the regular file's holds several CJK pages'
// outlines, and bold and italic files, rare in Japanese text, get small ones. 3 MB of PSRAM stays
// free for images, publisher fonts and the rest of the reader.
constexpr size_t kPrimaryBlockSets = 256;
constexpr size_t kStyleBlockSets = 32;
constexpr size_t kBlockCacheBudget =
    StreamBlockCache::bytesFor(kPrimaryBlockSets) + 3 * StreamBlockCache::bytesFor(kStyleBlockSets);
constexpr size_t kBlockCachePsramFloor = 3 * 1024 * 1024;
// Sources are opened, read and released with the render lock held, on the render task or the loop
// task, so this total needs no lock of its own.
size_t gBlockCacheBytes = 0;
}  // namespace

void SdCardFontSystem::releaseBlockCache(TtfSource& s) {
  s.blocks.init(nullptr, 0);
  s.blockStore.reset();
  gBlockCacheBytes -= std::min(gBlockCacheBytes, s.blockBytes);
  s.blockBytes = 0;
}

void SdCardFontSystem::allocateBlockCache(TtfSource& s, const bool primary) {
  for (const size_t sets : primary ? std::initializer_list<size_t>{kPrimaryBlockSets, 64, kStyleBlockSets}
                                   : std::initializer_list<size_t>{kStyleBlockSets}) {
    const size_t bytes = StreamBlockCache::bytesFor(sets);
    if (gBlockCacheBytes + bytes > kBlockCacheBudget) continue;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < bytes + kBlockCachePsramFloor) continue;
    s.blockStore = HalMemory::allocatePsram(bytes);
    if (!s.blockStore) continue;
    s.blocks.init(s.blockStore.get(), sets);
    s.blockBytes = bytes;
    gBlockCacheBytes += bytes;
    LOG_DBG("SDFS", "TTF block cache: %u KB in PSRAM (%u KB in all)", static_cast<unsigned>(bytes / 1024),
            static_cast<unsigned>(gBlockCacheBytes / 1024));
    return;
  }
}
#endif

bool SdCardFontSystem::openTtfSourceInto(TtfSource& s, const std::string& path, const bool keepResident,
                                         [[maybe_unused]] const bool primary) {
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
  if (keepResident) {
    // A resident buffer lands in PSRAM when fiFontMalloc can place it there; otherwise it competes
    // with everything else in internal DRAM. PsramAlloc aborts on OOM, so this gate is load-bearing:
    // fall back to streaming instead of attempting an allocation that can fail.
    static constexpr size_t kResidentMax = 6 * 1024 * 1024;
    static constexpr size_t kInternalHeadroom = 96 * 1024;  // FreeType setup, glyph caches, the rest
    bool resident = len <= kResidentMax;
    if (resident && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < len) {
      const size_t internalFree = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      resident = internalFree >= len + kInternalHeadroom;
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
      s.size = static_cast<unsigned long>(len);
      s.present = true;
      return true;
    }
    LOG_DBG("SDFS", "TTF %s (%u KB) too large to keep resident, streaming", path.c_str(),
            static_cast<unsigned>(len / 1024));
  }
  s.file = std::move(f);  // kept open; prefixRead() reads it on demand
  s.streamed = true;
  // Cache the file's head in PSRAM: an sfnt's per-glyph-fault tables (cmap,
  // loca, hmtx) sit before the multi-MB glyf table, so serving the first
  // 1 MB from RAM turns each glyph fault's 4-6 scattered SD seeks into one
  // glyf read. Gated per source so a small-PSRAM board takes what fits.
  // Filled on the first read, so a style that is never drawn costs no card read.
  freeink::font::PsramVector<uint8_t>().swap(s.bytes);
#ifdef BOARD_HAS_PSRAM
  static constexpr size_t kStreamPrefix = 1024 * 1024;
  // Only as far as the lookup and metric tables reach, read from the table directory.
  const size_t prefix = computeTtfPrefixBytes(s.file, len, kStreamPrefix);
  s.prefixReady.store(false, std::memory_order_relaxed);
  s.prefixWanted.store(prefix, std::memory_order_relaxed);
  releaseBlockCache(s);  // a cache of another file must never serve this one
  if (len > prefix) allocateBlockCache(s, primary);
#endif
  LOG_DBG("SDFS", "Streaming TTF %s (%u KB) from SD", path.c_str(), static_cast<unsigned>(len / 1024));
  s.size = static_cast<unsigned long>(len);
  s.present = true;
  return true;
}

// Streamed-source read: serve from the PSRAM prefix cache when the range is
// there, hit SD only for the tail (glyf outlines). A read straddling the
// boundary splits across both.
#ifdef BOARD_HAS_PSRAM
namespace {
// Set while a TTF family loads: opening each style's face reads its tables, and filling every
// style's prefix then would cost a card read per style even when no glyph is ever drawn.
std::atomic<bool> prefixFillDeferred{false};
struct PrefixFillDeferral {
  PrefixFillDeferral() { prefixFillDeferred.store(true, std::memory_order_relaxed); }
  ~PrefixFillDeferral() { prefixFillDeferred.store(false, std::memory_order_relaxed); }
};
}  // namespace
#endif

unsigned long SdCardFontSystem::prefixRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                           const unsigned long count) {
  auto* s = static_cast<TtfSource*>(ctx);
  const auto readFromCard = [s](const unsigned long at, unsigned char* out, const unsigned long length) {
    const auto read = SdCardFontRegistry::halFileRead(&s->file, at, out, length);
    if (s->uiReadFailed && read < length && at <= s->size && length <= s->size - at) {
      s->uiReadFailed->store(true, std::memory_order_relaxed);
    }
    return read;
  };
#ifdef BOARD_HAS_PSRAM
  bool ready = s->prefixReady.load(std::memory_order_acquire);
  if (!ready && count > 0 && !prefixFillDeferred.load(std::memory_order_relaxed) &&
      s->prefixWanted.load(std::memory_order_relaxed) > 0) {
    fillPrefix(*s);
    ready = s->prefixReady.load(std::memory_order_acquire);
  }
  const unsigned long cached = ready ? s->bytes.size() : 0;
#else
  const unsigned long cached = 0;  // no prefix cache without PSRAM
#endif
  if (offset < cached) {
    const unsigned long fromCache = (offset + count <= cached) ? count : cached - offset;
    if (count == 0) return 0;  // seek probe
    memcpy(buffer, s->bytes.data() + offset, fromCache);
    if (fromCache == count) return count;
    return fromCache + readFromCard(offset + fromCache, buffer + fromCache, count - fromCache);
  }
#ifdef BOARD_HAS_PSRAM
  if (s->blocks.active()) {
    return s->blocks.read(&SdCardFontSystem::cardRead, s, s->size, offset, buffer, count);
  }
#endif
  return readFromCard(offset, buffer, count);
}

#ifdef BOARD_HAS_PSRAM
void SdCardFontSystem::fillPrefix(TtfSource& s) {
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) <=
      s.prefixWanted.load(std::memory_order_relaxed) + 256 * 1024) {
    return;  // not now: a later read tries again
  }
  // The exchange claims the fill; a read of this source on the other task meanwhile goes to SD.
  const size_t prefix = s.prefixWanted.exchange(0, std::memory_order_relaxed);
  if (prefix == 0) return;
  freeink::font::PsramVector<uint8_t> head;
  head.resize(prefix);
  if (SdCardFontRegistry::halFileRead(&s.file, 0, head.data(), prefix) != prefix) {
    s.prefixWanted.store(prefix, std::memory_order_relaxed);  // a short read is retried later
    return;
  }
  s.bytes.swap(head);
  s.prefixReady.store(true, std::memory_order_release);
  LOG_DBG("SDFS", "Cached %u KB TTF prefix in PSRAM", static_cast<unsigned>(prefix / 1024));
}

unsigned long SdCardFontSystem::cardRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                         const unsigned long count) {
  auto* s = static_cast<TtfSource*>(ctx);
  const auto read = SdCardFontRegistry::halFileRead(&s->file, offset, buffer, count);
  if (s->uiReadFailed && read < count && offset <= s->size && count <= s->size - offset) {
    s->uiReadFailed->store(true, std::memory_order_relaxed);
  }
  return read;
}
#endif

void SdCardFontSystem::addTtfSources(TtfEpdFont& font, TtfSource* sources) {
  for (uint8_t st = 0; st < 4; ++st) {
    TtfSource& s = sources[st];
    if (!s.present) continue;
    if (s.streamed) {
      font.addStreamSource(st, &SdCardFontSystem::prefixRead, &s, s.size);
    } else {
      font.addResidentSource(st, s.bytes.data(), static_cast<uint32_t>(s.bytes.size()));
    }
  }
}

bool SdCardFontSystem::openTtfFamilySources(const SdCardFontFamilyInfo& family, TtfSource* sources) {
  // A single-file family (loose .ttf, or a folder with one file) supplies only
  // regular; TtfEpdFont then derives bold/italic from the wght axis or an
  // oblique shear. Extra files upgrade those styles to the real designs.
  char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
  const uint32_t generation = registry_.scanGeneration();
  bool complete = true;
  for (const auto& file : family.files) {
    const uint8_t role = file.style < 4 ? file.style : 0;
    if (sources[role].present) continue;  // registry already deduped by role
    if (!family.filePath(file, path, sizeof(path))) {
      LOG_ERR("SDFS", "Vector font path too long in family %s", family.name.c_str());
      complete = false;
      continue;
    }
    if (!openTtfSourceInto(sources[role], path, /*keepResident=*/false, /*primary=*/role == 0)) complete = false;
  }
  sourceSlotGenerations_[sources == sourceSlots_[1]] = complete && sources[0].present ? generation : 0;
  return sources[0].present;
}

void SdCardFontSystem::unloadUiTtf(GfxRenderer& renderer) {
  if (uiTtf_[0]) {
    for (const auto& ui : kUiFontSizes) {
      renderer.unregisterTtfFont(ui.fontId);
      renderer.removeFont(ui.fontId);
    }
  }
  // Variants borrow the master's faces, and the master reads the sources.
  for (size_t i = std::size(uiTtf_); i-- > 0;) uiTtf_[i].reset();
  if (uiTtfSources_) {
    for (size_t i = 0; i < 4; ++i) uiTtfSources_[i].uiReadFailed = nullptr;
  }
  releaseTtfSources(uiTtfSources_, ttfSources_);
}

bool SdCardFontSystem::loadUiTtf(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  static_assert(std::size(kUiFontSizes) == sizeof(uiTtf_) / sizeof(uiTtf_[0]), "one face per UI font slot");
  if (family.name == ttfFamily_ && canShareTtfSources(ttfSources_)) {
    uiTtfSources_ = ttfSources_;
  } else {
    uiTtfSources_ = unusedTtfSources(ttfSources_);
    if (!openTtfFamilySources(family, uiTtfSources_)) return false;
  }
  for (size_t i = 0; i < 4; ++i) uiTtfSources_[i].uiReadFailed = &uiTtfFailed_;
  // A UI screen shows well under 512 distinct glyphs per size; the glyph
  // arenas are PSRAM-backed (FontPsram).
  static constexpr size_t kUiGlyphCacheBytes = 64 * 1024;
  static constexpr uint16_t kUiMaxGlyphs = 512;
  for (size_t i = 0; i < std::size(uiTtf_); ++i) {
    auto font = makeUniqueNoThrow<TtfEpdFont>();
    if (!font) return false;
    const uint8_t size = kUiFontSizes[i].pointSize;
    bool ok;
    if (i == 0) {
      addTtfSources(*font, uiTtfSources_);
      ok = font->load(size, /*twoBit=*/true, kUiGlyphCacheBytes, kUiMaxGlyphs);
    } else {
      ok = font->loadVariantOf(*uiTtf_[0], size, /*twoBit=*/true, kUiGlyphCacheBytes, kUiMaxGlyphs);
    }
    if (!ok) return false;
    font->setGlyphLoadFailureFlag(&uiTtfFailed_);
    if (!font->build(" ") || uiTtfFailed()) return false;
    uiTtf_[i] = std::move(font);
  }
  // All three sizes loaded: only now replace the slots, so a failure above
  // never leaves a slot without a font.
  for (size_t i = 0; i < std::size(uiTtf_); ++i) {
    const int id = kUiFontSizes[i].fontId;
    renderer.removeFont(id);
    auto font = uiTtf_[i]->family();
    font.setGlyphFallback(builtinUiFonts_[i]);
    font.setGlyphLoadFailureFlag(&uiTtfFailed_);
    renderer.insertFont(id, font);
    renderer.registerTtfFont(id, uiTtf_[i].get());
  }
  return true;
}

void SdCardFontSystem::recoverUiFont(GfxRenderer& renderer) {
  if (!uiTtfFailed()) return;
  LOG_ERR("SDFS", "UI glyph load failed in %s; using built-in fonts", activeUiFontName_);
  applyUiFont(renderer, "");
}

void SdCardFontSystem::clearTtfUiFallbacks(GfxRenderer& renderer) {
  if (ttfUiIds_.empty()) return;
  renderer.clearFallbackFonts();
  for (const int id : ttfUiIds_) {
    renderer.unregisterTtfFont(id);
    renderer.removeFont(id);
  }
  ttfUiIds_.clear();
  ttfUi_.clear();
}

void SdCardFontSystem::setupTtfUiFallbacks(GfxRenderer& renderer) {
  if (!ttf_ || ttfFamily_.empty() || !ttfUiIds_.empty()) return;

  const uint32_t contentKey = ttf_->contentKey();
  // Small caches: UI strings (titles/rows) are short. Each UI family is a sized
  // variant of ttf_, sharing its FreeType faces: no font file is parsed again and
  // no FT_Face is allocated, only a lazy glyph arena. Without PSRAM that still
  // competes with the reader's section build for internal DRAM, and the build
  // must win: below this floor, skip the fallback (built-in bitmap UI fonts keep
  // covering Latin UI text).
  static constexpr size_t kUiFallbackMinInternalHeap = 24 * 1024;
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
    const bool ok =
        f->loadVariantOf(*ttf_, ui.pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/16 * 1024, /*maxGlyphs=*/384);
    if (!ok) continue;
    LOG_DBG("SDFS", "TTF UI fallback @%upt loaded (heap free %u)", ui.pointSize, (unsigned)ESP.getFreeHeap());
    // Separate reader and UI identities even when their point sizes match.
    const int id = computeTtfFontId(ttfFamily_.c_str(), contentKey, ui.pointSize, /*uiFallback=*/true);
    renderer.insertFont(id, f->family());
    renderer.registerTtfFont(id, f.get());
    renderer.setFallbackFont(ui.fontId, id);
    ttfUiIds_.push_back(id);
    ttfUi_.push_back(std::move(f));
  }
}

void SdCardFontSystem::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                     const bool registryWasDirty, const bool setupFallbacks) {
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
    releaseTtfLookup(renderer);  // variants of the face being re-driven; rebuilt on the next lookup
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);
    if (ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs)) {
      ttf_->build(" ");
      ttfFontId_ = computeTtfFontId(family.name.c_str(), ttf_->contentKey(), size);
      renderer.insertFont(ttfFontId_, ttf_->family());
      renderer.registerTtfFont(ttfFontId_, ttf_.get());
      ttfPointSize_ = size;
      return;
    }
    // Resize failed: fall through to a clean full reload.
  }

  unloadTtf(renderer);
#ifdef BOARD_HAS_PSRAM
  const PrefixFillDeferral deferFill;
#endif

  if (family.files.empty()) {
    LOG_ERR("SDFS", "Vector family %s has no file", family.name.c_str());
    SETTINGS.clearSdFontFamily();
    return;
  }

  const bool reuseUiSource = family.name == activeUiFontName_ && canShareTtfSources(uiTtfSources_);
  ttfSources_ = reuseUiSource ? uiTtfSources_ : unusedTtfSources(uiTtfSources_);
  if (!reuseUiSource && !openTtfFamilySources(family, ttfSources_)) {
    // Possibly a transient SD read failure: keep the user's selection so the
    // next ensureLoaded() retries; this session falls back to the built-in.
    LOG_ERR("SDFS", "Vector family %s: regular file failed to open (keeping selection)", family.name.c_str());
    releaseTtfSources(ttfSources_, uiTtfSources_);
    return;
  }

  ttf_ = makeUniqueNoThrow<TtfEpdFont>();
  if (!ttf_) {
    // Transient OOM: keep the user's selection (unlike a parse failure) so the
    // next ensureLoaded() can retry once heap pressure passes.
    LOG_ERR("SDFS", "OOM: TtfEpdFont for %s", family.name.c_str());
    releaseTtfSources(ttfSources_, uiTtfSources_);
    return;
  }
  addTtfSources(*ttf_, ttfSources_);
  const bool ok = ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs);
  if (!ok) {
    // init failure is ambiguous (corrupt font vs. transient OOM inside
    // FreeType): keep the selection and retry next ensureLoaded() rather than
    // silently reverting the user to the built-in font. A genuinely broken
    // font costs one failed load per reader entry, visible in the log.
    LOG_ERR("SDFS", "FreeInkFont could not parse %s (keeping selection)", family.name.c_str());
    ttf_.reset();
    releaseTtfSources(ttfSources_, uiTtfSources_);
    return;
  }
  // Seed the regular face's glyph cache; other styles + glyphs fault on demand.
  ttf_->build(" ");

  ttfFontId_ = computeTtfFontId(family.name.c_str(), ttf_->contentKey(), size);
  renderer.insertFont(ttfFontId_, ttf_->family());
  renderer.registerTtfFont(ttfFontId_, ttf_.get());
  ttfFamily_ = family.name;
  ttfPointSize_ = size;
  LOG_DBG("SDFS", "Reader TTF face loaded (heap free %u, max block %u)", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  if (setupFallbacks) setupTtfUiFallbacks(renderer);
  LOG_DBG("SDFS", "Loaded TTF font: %s @ %upt (id %d)", family.name.c_str(), size, ttfFontId_);
}

#endif  // CROSSPOINT_VECTOR_FONTS
