#include "WordLookup.h"

#include <cstddef>

#include "Deinflector.h"

namespace {

size_t utf8CharBytes(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  return 4;
}

// Advance `pos` by `n` UTF-8 characters within `text`.
// Returns the new byte position, or text.size() if past end.
size_t advanceChars(const std::string& text, size_t pos, int n) {
  for (int i = 0; i < n && pos < text.size(); i++) {
    pos += utf8CharBytes(static_cast<unsigned char>(text[pos]));
  }
  return pos;
}

// Decode the codepoint at byte `pos`.
uint32_t decodeCp(const std::string& s, size_t pos) {
  auto c0 = static_cast<unsigned char>(s[pos]);
  if (c0 < 0x80) return c0;
  if ((c0 & 0xE0) == 0xC0) return ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(s[pos + 1]) & 0x3F);
  if ((c0 & 0xF0) == 0xE0)
    return ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(s[pos + 2]) & 0x3F);
  return ((c0 & 0x07) << 18) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12) |
         ((static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(s[pos + 3]) & 0x3F);
}

// True if the window contains any character a proper name (jmnedict) could plausibly be looked up
// by: a CJK kanji or a katakana. jmnedict's pure-hiragana headwords are name READINGS
// (ながおかずこ...), and matching page text against those produces spurious mid-sentence fragments
// (にはな, さな) rather than real words -- so hiragana-only windows skip jmnedict. Improves
// segmentation AND removes the jmnedict search (~2 SD reads) for most windows of a page.
bool hasNameChar(const std::string& text) {
  for (size_t pos = 0; pos < text.size();) {
    const uint32_t cp = decodeCp(text, pos);
    const bool kanji =
        (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF);
    const bool katakana = (cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0xFF66 && cp <= 0xFF9D);
    if (kanji || katakana) return true;
    pos += utf8CharBytes(static_cast<unsigned char>(text[pos]));
  }
  return false;
}

}  // namespace

bool WordLookup::lookup(const std::string& paragraphText, size_t byteOffset, WordLookupResult& out,
                        bool needDefinition) {
  if (byteOffset >= paragraphText.size()) return false;

  // Try decreasing window lengths: 8 characters down to 1.
  // For each window, try the raw text first, then deinflected candidates.
  for (int windowChars = MAX_WINDOW_CHARS; windowChars >= 1; windowChars--) {
    const size_t windowEnd = advanceChars(paragraphText, byteOffset, windowChars);
    if (windowEnd <= byteOffset) continue;

    const std::string window = paragraphText.substr(byteOffset, windowEnd - byteOffset);

    // Try exact match first. jmdict and grammar always (grammar has single-char entries, so no
    // length gating); jmnedict only when the window could actually be a name.
    uint8_t dictMask = DictIndex::DICT_JMDICT | DictIndex::DICT_GRAMMAR;
    if (hasNameChar(window)) dictMask |= DictIndex::DICT_NAMES;

    DictEntry entry;
    if (DictIndex::lookupExact(window.c_str(), entry, dictMask, needDefinition)) {
      out.entry = std::move(entry);
      out.matchLength = windowEnd - byteOffset;
      out.deinflected = false;
      return true;
    }

    // Deinflection only strips hiragana conjugation suffixes (た, て, ます, ない, ...). If the
    // window doesn't end in hiragana, no rule can fire and deinflect() would just return the raw
    // form -- so skip the ~150-rule scan entirely. Exactly equivalent, saves real CPU on the many
    // kanji-/katakana-final windows of a page.
    size_t lastCharStart = window.size();
    while (lastCharStart > 0 && (static_cast<unsigned char>(window[lastCharStart - 1]) & 0xC0) == 0x80) {
      lastCharStart--;
    }
    if (lastCharStart > 0) lastCharStart--;
    const uint32_t lastCp = decodeCp(window, lastCharStart);
    if (lastCp < 0x3040 || lastCp > 0x309F) continue;  // not hiragana-final -> no deinflection

    // Try deinflected candidates.
    auto candidates = Deinflector::deinflect(window);
    // Skip index 0 — that's the raw surface form we already tried.
    for (size_t i = 1; i < candidates.size(); i++) {
      // Deinflected forms are conjugated verbs/adjectives -> only ever in jmdict.
      if (DictIndex::lookupExact(candidates[i].text.c_str(), entry, DictIndex::DICT_JMDICT, needDefinition)) {
        out.entry = std::move(entry);
        out.matchLength = windowEnd - byteOffset;
        out.deinflected = true;
        return true;
      }
    }
  }

  return false;
}
