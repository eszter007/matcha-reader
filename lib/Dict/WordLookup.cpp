#include "WordLookup.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "Deinflector.h"

namespace {

constexpr char kSuru[] = "\xe3\x81\x99\xe3\x82\x8b";  // する
constexpr size_t kSuruBytes = sizeof(kSuru) - 1;

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

// A verb from its continuative stem (乗り -> 乗る, 出し -> 出す): the stem + ます, deinflected, must
// name a verb of the class the rule produced.
bool lookupVerbFromStem(const std::string& stem, DictEntry& entry, const bool needDefinition) {
  if (stem.empty() || stem.size() + 6 >= DictIndexRecord::HEADWORD_SIZE) return false;
  // A single-kanji suru verb (愛し -> 愛する, 察し -> 察する) before its godan variant (愛す).
  static constexpr char kShi[] = "\xe3\x81\x97";  // し
  if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, kShi) == 0) {
    const std::string suru = stem.substr(0, stem.size() - 3) + "\xe3\x81\x99\xe3\x82\x8b";  // する
    if (DictIndex::lookupExact(suru.c_str(), entry, DictIndex::DICT_JMDICT, needDefinition, DictIndexRecord::POS_VS)) {
      return true;
    }
  }
  const auto candidates = Deinflector::deinflect(stem + "\xe3\x81\xbe\xe3\x81\x99");  // ます
  for (size_t i = 1; i < candidates.size(); i++) {
    uint8_t posMask = 0;
    switch (candidates[i].condition) {
      case WordCondition::V1:
        posMask = DictIndexRecord::POS_V1;
        break;
      case WordCondition::V5:
        posMask = DictIndexRecord::POS_V5;
        break;
      case WordCondition::VK:
        posMask = DictIndexRecord::POS_VK;
        break;
      default:
        continue;  // only real verb classes: a stem is never a noun or adjective
    }
    if (DictIndex::lookupExact(candidates[i].text.c_str(), entry, DictIndex::DICT_JMDICT, needDefinition, posMask)) {
      return true;
    }
  }
  return false;
}

// Hiragana that end a godan continuative stem (乗り, 書き, 話し, 待ち, 死に, 飛び, 読み, 泳ぎ, 買い).
bool endsInIRow(const std::string& s) {
  static constexpr const char* kIRow[] = {"い", "き", "ぎ", "し", "ち", "に", "び", "み", "り"};
  for (const char* k : kIRow) {
    const size_t n = std::strlen(k);
    if (s.size() > n && s.compare(s.size() - n, n, k) == 0) return true;
  }
  return false;
}

// お or ご in front of a word: お出しする, ご説明する.
size_t honorificPrefixBytes(const std::string& s) {
  static constexpr char kO[] = "\xe3\x81\x8a";   // お
  static constexpr char kGo[] = "\xe3\x81\x94";  // ご
  if (s.size() > 3 && (s.compare(0, 3, kO) == 0 || s.compare(0, 3, kGo) == 0)) return 3;
  return 0;
}

}  // namespace

uint8_t WordLookup::lookupAll(const std::string& paragraphText, const size_t byteOffset,
                              uint8_t priority[MAX_WINDOW_CHARS], const int maxChars) {
  uint8_t found = 0;
  if (byteOffset >= paragraphText.size()) return found;
  const int limit = std::min(maxChars, MAX_WINDOW_CHARS);
  for (int windowChars = 1; windowChars <= limit; windowChars++) {
    const size_t windowEnd = advanceChars(paragraphText, byteOffset, windowChars);
    if (windowEnd <= byteOffset) break;
    if (windowChars > 1 && windowEnd == advanceChars(paragraphText, byteOffset, windowChars - 1))
      break;  // text ran out
    // Stack copy rather than substr(): this runs for every window of every cell of a page.
    char window[MAX_WINDOW_CHARS * 4 + 1];
    const size_t windowLen = windowEnd - byteOffset;
    if (windowLen >= sizeof(window)) break;
    std::memcpy(window, paragraphText.data() + byteOffset, windowLen);
    window[windowLen] = '\0';
    if (std::memchr(window, '\0', windowLen) != nullptr) break;
    DictEntry entry;
    if (DictIndex::lookupExact(window, entry, DictIndex::DICT_ALL, /*needDefinition=*/false)) {
      found |= static_cast<uint8_t>(1u << (windowChars - 1));
      priority[windowChars - 1] = entry.priority;
    } else if (!DictIndex::longerHeadwordMayExist()) {
      break;
    }
  }
  return found;
}

bool WordLookup::isLongConjugationTail(const std::string& text, const size_t byteOffset, const int chars) {
  size_t pos = advanceChars(text, byteOffset, MAX_WINDOW_CHARS - 1);
  for (int i = MAX_WINDOW_CHARS; i <= chars; i++) {
    if (pos >= text.size()) return false;
    const uint32_t cp = decodeCp(text, pos);
    if (cp < 0x3041 || cp > 0x3096) return false;
    pos = advanceChars(text, pos, 1);
  }
  return true;
}

namespace {
enum class SuruNoun : uint8_t { Found, Missing, Unresolved };

// JMdict files a suru verb under its noun (嘲笑, not 嘲笑する). Tried after every candidate, so a
// verb headword of its own (対する) wins over its single-kanji noun (対).
// Unresolved: the noun's entry did not load (low heap), so the caller answers nothing and the
// heap-limited retry runs.
SuruNoun lookupSuruNoun(const std::vector<DeinflectionCandidate>& candidates, const size_t matchLength,
                        const bool needDefinition, WordLookupResult& out) {
  DictEntry entry;
  for (const auto& candidate : candidates) {
    const auto& text = candidate.text;
    if (text.size() <= kSuruBytes || text.compare(text.size() - kSuruBytes, kSuruBytes, kSuru) != 0) continue;
    if (candidate.condition != WordCondition::VS && candidate.condition != WordCondition::DICT) continue;
    // lookupExact rejects keys of HEADWORD_SIZE bytes or more, so a stack key covers every stem it accepts.
    const size_t nounBytes = text.size() - kSuruBytes;
    char noun[DictIndexRecord::HEADWORD_SIZE];
    if (nounBytes >= sizeof(noun)) continue;
    std::memcpy(noun, text.data(), nounBytes);
    noun[nounBytes] = '\0';
    // Humble/honorific お/ご + stem + する is the verb (お出しする -> 出す), not a noun that happens
    // to start with お (お出し "dashi"). A real お-noun (お茶する) is still found below.
    const size_t prefix = honorificPrefixBytes(noun);
    // An お-noun that is itself a suru noun (お願いする, おしゃべりする) stays that noun. Newer
    // dictionaries flag it POS_VS; older ones only say so in its tags: one definition read, only here.
    bool suruNoun = false;
    if (prefix > 0) {
      DictEntry own;
      if (DictIndex::lookupExact(noun, own, DictIndex::DICT_JMDICT, /*needDefinition=*/false,
                                 DictIndexRecord::POS_VS)) {
        suruNoun = true;
      } else if (DictIndex::lookupExact(noun, own, DictIndex::DICT_JMDICT, /*needDefinition=*/true)) {
        const std::string_view head(own.definition.data(), std::min<size_t>(own.definition.size(), 96));
        suruNoun = head.find("[suru]") != std::string_view::npos;
      } else if (DictIndex::lookupExact(noun, own, DictIndex::DICT_JMDICT, /*needDefinition=*/false)) {
        // The noun exists but its definition did not load (low heap): leave the lookup unresolved
        // so the heap-limited retry runs, rather than answering with the verb.
        return SuruNoun::Unresolved;
      }
    }
    if (prefix > 0 && !suruNoun &&
        lookupVerbFromStem(std::string(noun + prefix, nounBytes - prefix), entry, needDefinition)) {
      out.entry = std::move(entry);
      out.matchLength = matchLength;
      out.deinflected = true;
      return SuruNoun::Found;
    }
    // Dictionaries converted before suru tags became POS_VS flag these nouns POS_OTHER. A kanji
    // or katakana stem keeps particles (にする) from matching that way; the する suffix is
    // hiragana, so testing the whole text tests the stem.
    if (hasNameChar(text) && DictIndex::lookupExact(noun, entry, DictIndex::DICT_JMDICT, needDefinition,
                                                    DictIndexRecord::POS_VS | DictIndexRecord::POS_OTHER)) {
      out.entry = std::move(entry);
      out.matchLength = matchLength;
      out.deinflected = true;
      return SuruNoun::Found;
    }
    // ご説明する: the suru noun without its honorific prefix.
    if (prefix > 0 && hasNameChar(text) &&
        DictIndex::lookupExact(noun + prefix, entry, DictIndex::DICT_JMDICT, needDefinition,
                               DictIndexRecord::POS_VS | DictIndexRecord::POS_OTHER)) {
      out.entry = std::move(entry);
      out.matchLength = matchLength;
      out.deinflected = true;
      return SuruNoun::Found;
    }
  }
  return SuruNoun::Missing;
}
}  // namespace

namespace {
// Windows past MAX_WINDOW_CHARS: a whole headword (ありがとうございました), else a verb or adjective
// conjugated past the normal window, checked against its word class. Reached only when the text
// runs on in hiragana. Headwords are probed upward and stop at the first prefix no headword
// continues, so a kana-heavy page costs about one probe per position; conjugations need a kanji or
// katakana stem.
bool lookupLongConjugation(const std::string& text, const size_t byteOffset, WordLookupResult& out,
                           const bool needDefinition) {
  int available = 0;
  // A NUL ends the text the dictionary would see (see the window trim in lookup()).
  for (size_t pos = byteOffset;
       pos < text.size() && text[pos] != '\0' && available < WordLookup::MAX_CONJUGATION_CHARS;) {
    pos = advanceChars(text, pos, 1);
    available++;
  }
  int longest = WordLookup::MAX_WINDOW_CHARS;
  while (longest < available && WordLookup::isLongConjugationTail(text, byteOffset, longest + 1)) longest++;
  if (longest == WordLookup::MAX_WINDOW_CHARS) return false;
  const auto windowOf = [&](const int chars) {
    return text.substr(byteOffset, advanceChars(text, byteOffset, chars) - byteOffset);
  };

  DictEntry entry;
  int exactChars = 0;
  for (int chars = WordLookup::MAX_WINDOW_CHARS + 1; chars <= longest; chars++) {
    const std::string window = windowOf(chars);
    if (window.size() >= DictIndexRecord::HEADWORD_SIZE) break;
    if (DictIndex::lookupExact(window.c_str(), entry, DictIndex::DICT_JMDICT | DictIndex::DICT_GRAMMAR, false)) {
      exactChars = chars;
    } else if (!DictIndex::longerHeadwordMayExist()) {
      break;
    }
  }
  if (exactChars > 0) {
    const std::string window = windowOf(exactChars);
    if (DictIndex::lookupExact(window.c_str(), entry, DictIndex::DICT_JMDICT | DictIndex::DICT_GRAMMAR,
                               needDefinition)) {
      out.entry = std::move(entry);
      out.matchLength = window.size();
      out.deinflected = false;
      return true;
    }
  }

  const uint32_t first = decodeCp(text, byteOffset);
  const bool stem = (first >= 0x4E00 && first <= 0x9FFF) || (first >= 0x3400 && first <= 0x4DBF) ||
                    (first >= 0x30A1 && first <= 0x30FA);
  if (!stem) return false;
  for (int chars = longest; chars > WordLookup::MAX_WINDOW_CHARS; chars--) {
    const std::string window = windowOf(chars);
    const auto candidates = Deinflector::deinflect(window);
    for (size_t i = 1; i < candidates.size(); i++) {
      uint8_t posMask = 0;
      switch (candidates[i].condition) {
        case WordCondition::V1:
          posMask = DictIndexRecord::POS_V1;
          break;
        case WordCondition::V5:
          posMask = DictIndexRecord::POS_V5;
          break;
        case WordCondition::VS:
          posMask = DictIndexRecord::POS_VS;
          break;
        case WordCondition::VK:
          posMask = DictIndexRecord::POS_VK;
          break;
        case WordCondition::ADJ_I:
          posMask = DictIndexRecord::POS_ADJ_I;
          break;
        case WordCondition::DICT:
          continue;  // an unclassed candidate this long is a phrase guess, not a conjugation
      }
      if (DictIndex::lookupExact(candidates[i].text.c_str(), entry, DictIndex::DICT_JMDICT, needDefinition, posMask)) {
        out.entry = std::move(entry);
        out.matchLength = window.size();
        out.deinflected = true;
        return true;
      }
    }
    switch (lookupSuruNoun(candidates, window.size(), needDefinition, out)) {
      case SuruNoun::Found:
        return true;
      case SuruNoun::Unresolved:
        return false;
      case SuruNoun::Missing:
        break;
    }
  }
  return false;
}
}  // namespace

bool WordLookup::lookup(const std::string& paragraphText, size_t byteOffset, WordLookupResult& out, bool needDefinition,
                        const uint32_t precedingCp) {
  if (byteOffset >= paragraphText.size()) return false;
  if (lookupLongConjugation(paragraphText, byteOffset, out, needDefinition)) return true;

  // Try decreasing window lengths: 8 characters down to 1.
  // For each window, try the raw text first, then deinflected candidates.
  for (int windowChars = MAX_WINDOW_CHARS; windowChars >= 1; windowChars--) {
    const size_t windowEnd = advanceChars(paragraphText, byteOffset, windowChars);
    if (windowEnd <= byteOffset) continue;

    std::string window = paragraphText.substr(byteOffset, windowEnd - byteOffset);
    // A NUL ends the window. lookupExact() takes a C string, so a NUL already truncated what the
    // dictionary saw -- but matchLength below is measured from windowEnd and would have claimed
    // the bytes past it too, handing the caller a match far longer than the text that matched.
    // Vertical layout puts one in reach: a tate-chu-yoko run is a single glyph with codepoint 0,
    // and a scanner encoding that glyph writes a NUL. Trimming here keeps the window, the
    // dictionary key and matchLength describing the same bytes.
    if (const size_t nul = window.find('\0'); nul != std::string::npos) {
      window.resize(nul);
      if (window.empty()) continue;
    }

    // Try exact match first. jmdict and grammar always (grammar has single-char entries, so no
    // length gating); jmnedict only when the window could actually be a name.
    uint8_t dictMask = DictIndex::DICT_JMDICT | DictIndex::DICT_GRAMMAR;
    if (hasNameChar(window)) dictMask |= DictIndex::DICT_NAMES;

    DictEntry entry;
    if (DictIndex::lookupExact(window.c_str(), entry, dictMask, needDefinition)) {
      // A noun that is also a verb's continuative stem, between an argument particle and a comma,
      // is the verb linking two clauses: バスに乗り、 is 乗る, not the noun 乗り "ride". After の, a
      // comma or the start of a sentence it stays the noun (夏の祭り、). Frequency cannot decide:
      // both sides of almost every pair are at the top priority.
      constexpr uint8_t kVerbBits =
          DictIndexRecord::POS_V1 | DictIndexRecord::POS_V5 | DictIndexRecord::POS_VS | DictIndexRecord::POS_VK;
      const size_t next = byteOffset + window.size();
      const bool beforeComma = next < paragraphText.size() && (decodeCp(paragraphText, next) == 0x3001 ||
                                                               decodeCp(paragraphText, next) == 0xFF0C);  // 、 ，
      bool afterArgument = false;
      uint32_t before = precedingCp;
      if (byteOffset > 0) {
        size_t prev = byteOffset - 1;
        while (prev > 0 && (static_cast<unsigned char>(paragraphText[prev]) & 0xC0) == 0x80) prev--;
        before = decodeCp(paragraphText, prev);
      }
      {
        switch (before) {
          case 0x3092:  // を
          case 0x306B:  // に
          case 0x3078:  // へ
          case 0x3067:  // で
          case 0x3068:  // と
          case 0x304C:  // が
          case 0x3082:  // も
            afterArgument = true;
            break;
          default:
            break;
        }
      }
      DictEntry verb;
      // 通り after まで or いつも is the suffix "as" (今まで通り、), far more often than 通る.
      static constexpr char kDoori[] = "\xe9\x80\x9a\xe3\x82\x8a";  // 通り
      if (beforeComma && afterArgument && window != kDoori && (entry.posFlags & kVerbBits) == 0 &&
          hasNameChar(window) && endsInIRow(window) && lookupVerbFromStem(window, verb, needDefinition)) {
        out.entry = std::move(verb);
        out.matchLength = window.size();
        out.deinflected = true;
        return true;
      }
      out.entry = std::move(entry);
      out.matchLength = window.size();
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
      // Validate the candidate against the entry's actual word class: a godan-rule candidate must
      // resolve to a godan verb, etc. Kills reading-record collisions like だとい→だとう (妥当, a
      // noun) or って→う (鵜). Condition DICT (chained contractions) carries no class -> mask 0.
      uint8_t posMask = 0;
      switch (candidates[i].condition) {
        case WordCondition::V1:
          posMask = DictIndexRecord::POS_V1;
          break;
        case WordCondition::V5:
          posMask = DictIndexRecord::POS_V5;
          break;
        case WordCondition::VS:
          posMask = DictIndexRecord::POS_VS;
          break;
        case WordCondition::VK:
          posMask = DictIndexRecord::POS_VK;
          break;
        case WordCondition::ADJ_I:
          posMask = DictIndexRecord::POS_ADJ_I;
          break;
        case WordCondition::DICT:
          posMask = 0;
          break;
      }
      // Deinflected forms are conjugated verbs/adjectives -> only ever in jmdict.
      if (DictIndex::lookupExact(candidates[i].text.c_str(), entry, DictIndex::DICT_JMDICT, needDefinition, posMask)) {
        out.entry = std::move(entry);
        // window.size(), not windowEnd - byteOffset: the deinflection path reads the same window,
        // so a NUL trimmed above must shorten this match too.
        out.matchLength = window.size();
        out.deinflected = true;
        return true;
      }
    }
    switch (lookupSuruNoun(candidates, window.size(), needDefinition, out)) {
      case SuruNoun::Found:
        return true;
      case SuruNoun::Unresolved:
        return false;
      case SuruNoun::Missing:
        break;
    }
  }

  return false;
}
