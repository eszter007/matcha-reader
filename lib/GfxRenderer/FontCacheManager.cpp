#include "FontCacheManager.h"

#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

char* appendUtf8Codepoint(char* output, const uint32_t codepoint) {
  if (codepoint < 0x80) {
    *output++ = static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    *output++ = static_cast<char>(0xC0 | (codepoint >> 6));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    *output++ = static_cast<char>(0xE0 | (codepoint >> 12));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    *output++ = static_cast<char>(0xF0 | (codepoint >> 18));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return output;
}

}  // namespace

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::releaseAllFontMemory() {
  clearCache();
  // The emergency path also surrenders the persistent advance tables (up to ~6KB per active
  // style per font since the 1536-entry cap): they are pure measurement caches that rebuild
  // from SD on the next ensureSdCardFontReady, and on the tight X3 heap their residency is
  // exactly the margin the caller is trying to reclaim. Rendering itself never needs them.
  for (auto& [id, font] : sdCardFonts_) {
    font->clearPersistentCache();
  }
  if (fontDecompressor_) fontDecompressor_->freeGlyphSlab();
}

namespace {
void appendUtf8(std::string& out, const uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}
}  // namespace

void FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask) {
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed = it->second->prewarm(utf8Text, styleMask);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
      // Warm the companion with exactly the codepoints this font lacks (a CJK-only reader
      // font showing Latin text, or vice versa) -- otherwise every one of them is loaded
      // through the on-demand SD miss handler again on EVERY page render.
      if (fallbackSdFont_ && fallbackSdFont_ != it->second) {
        std::string missing;
        const auto* p = reinterpret_cast<const unsigned char*>(utf8Text);
        uint32_t cp;
        while ((cp = utf8NextCodepoint(&p)) != 0) {
          if (!it->second->coversCodepoint(cp)) appendUtf8(missing, cp);
        }
        if (!missing.empty()) fallbackSdFont_->prewarm(missing.c_str(), styleMask);
      }
    }
    return;
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return;

  const auto& family = fontMap_.at(fontId);
  for (uint8_t i = 0; i < 4; i++) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = family.getData(style);
    if (data && data->groups) {
      int missed = fontDecompressor_->prewarmCache(data, utf8Text);
      if (missed > 0) {
        LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
      }
    }
    if (family.getFallback()) {
      const EpdFontData* fbData = family.getFallback()->getData(style);
      if (fbData && fbData != data && fbData->groups) {
        fontDecompressor_->prewarmCache(fbData, utf8Text);
      }
    }
  }

  // Codepoints no built-in font in the chain covers (e.g. every kanji when the reader font is
  // built-in and the JP companion provides the glyphs): warm the companion SD font once here
  // instead of per-glyph on-demand loads on every page render.
  if (fallbackSdFont_) {
    std::string missing;
    const auto* p = reinterpret_cast<const unsigned char*>(utf8Text);
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&p)) != 0) {
      if (!family.getGlyphResident(cp)) appendUtf8(missing, cp);
    }
    if (!missing.empty()) fallbackSdFont_->prewarm(missing.c_str(), styleMask);
  }
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

uint8_t FontCacheManager::resolveScanStyle(int fontId, EpdFontFamily::Style style) const {
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;

  const auto sdFont = sdCardFonts_.find(fontId);
  if (sdFont != sdCardFonts_.end()) return sdFont->second->resolveStyle(baseStyle);

  const auto font = fontMap_.find(fontId);
  if (font == fontMap_.end()) return baseStyle;

  const EpdFontData* resolvedData = font->second.getData(static_cast<EpdFontFamily::Style>(baseStyle));
  for (uint8_t candidate = 0; candidate < 4; candidate++) {
    if (font->second.getData(static_cast<EpdFontFamily::Style>(candidate)) == resolvedData) return candidate;
  }
  return baseStyle;
}

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || *text == '\0') return;

  uint8_t fontSlot = scanFontCount_;
  for (uint8_t i = 0; i < scanFontCount_; i++) {
    if (scanFontIds_[i] == fontId) {
      fontSlot = i;
      break;
    }
  }
  if (fontSlot == scanFontCount_) {
    if (scanFontCount_ >= MAX_SCAN_FONTS) return;
    scanFontIds_[scanFontCount_++] = fontId;
  }

  const uint8_t resolvedStyle = resolveScanStyle(fontId, style);
  const uint8_t group = fontSlot * 4 + resolvedStyle;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    const uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (codepoint == 0) break;

    const uint32_t packed = (static_cast<uint32_t>(fontSlot) << SCAN_FONT_SHIFT) |
                            (static_cast<uint32_t>(resolvedStyle) << SCAN_STYLE_SHIFT) | codepoint;
    bool found = false;
    for (uint16_t i = 0; i < scanCodepointCount_; i++) {
      if (scanCodepoints_[i] == packed) {
        found = true;
        break;
      }
    }
    if (found) continue;

    if (scanCodepointCount_ >= MAX_SCAN_CODEPOINTS) {
      if (!scanOverflowWarned_) {
        LOG_DBG("FCM", "Scan codepoint cap (%u) reached; excess glyphs will load on demand",
                static_cast<unsigned>(MAX_SCAN_CODEPOINTS));
        scanOverflowWarned_ = true;
      }
      continue;
    }

    scanCodepoints_[scanCodepointCount_++] = packed;
    scanGroupCounts_[group]++;
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager) : manager_(&manager) {
  manager_->scanMode_ = ScanMode::Scanning;
  manager_->clearCache();
  manager_->resetStats();
  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  manager_->scanOverflowWarned_ = false;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
}

void FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanCodepointCount_ == 0) return;

  std::sort(manager_->scanCodepoints_, manager_->scanCodepoints_ + manager_->scanCodepointCount_);

  uint16_t groupStarts[SCAN_GROUP_COUNT] = {};
  for (uint8_t group = 1; group < SCAN_GROUP_COUNT; group++) {
    groupStarts[group] = groupStarts[group - 1] + manager_->scanGroupCounts_[group - 1];
  }

  // Each packed entry provides four bytes, enough for one UTF-8 codepoint.
  // Encoding high groups first means a terminator can overwrite only a group
  // that has already been prewarmed; unread lower groups remain intact.
  for (int group = SCAN_GROUP_COUNT - 1; group >= 0; group--) {
    const uint16_t groupCount = manager_->scanGroupCounts_[group];
    if (groupCount == 0) continue;

    const uint16_t groupStart = groupStarts[group];
    char* const utf8Text = reinterpret_cast<char*>(manager_->scanCodepoints_ + groupStart);
    char* output = utf8Text;
    for (uint16_t i = 0; i < groupCount; i++) {
      const uint32_t codepoint = manager_->scanCodepoints_[groupStart + i] & SCAN_CODEPOINT_MASK;
      output = appendUtf8Codepoint(output, codepoint);
    }
    *output = '\0';

    const uint8_t fontSlot = static_cast<uint8_t>(group) / 4;
    const uint8_t style = static_cast<uint8_t>(group) & 0x03;
    manager_->prewarmCache(manager_->scanFontIds_[fontSlot], utf8Text, 1 << style);
  }

  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    endScanAndPrewarm();  // no-op if already called
    manager_->clearCache();
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope() { return PrewarmScope(*this); }
