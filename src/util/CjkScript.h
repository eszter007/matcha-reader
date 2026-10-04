#pragma once

#include <cstdint>
#include <string_view>

// Which CJK typesetting and dictionary conventions a book follows, from its language tag.
// Japanese and Chinese share the scan-based word lookup (contiguous ideographs, no word
// spaces), but differ in dictionary folder, companion font and vertical punctuation.
enum class CjkScript : uint8_t { None, Japanese, SimplifiedChinese, TraditionalChinese };

namespace cjk {

inline char lowerAscii(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

inline bool subtagIs(std::string_view tag, const char* want) {
  size_t i = 0;
  for (; i < tag.size() && want[i] != '\0'; i++) {
    if (lowerAscii(tag[i]) != want[i]) return false;
  }
  return i == tag.size() && want[i] == '\0';
}

// Classify a dc:language / BCP-47 tag ("ja", "zh-Hant", "zh_TW", "cmn-Hans-CN", "yue").
// Chinese with no script or region subtag is treated as simplified, the mainland default.
inline CjkScript scriptForLanguage(std::string_view tag) {
  if (tag.empty()) return CjkScript::None;
  const size_t sep = tag.find_first_of("-_");
  const std::string_view primary = tag.substr(0, sep);
  if (subtagIs(primary, "ja") || subtagIs(primary, "jpn")) return CjkScript::Japanese;
  // yue (Cantonese) is written in traditional characters in practice.
  if (subtagIs(primary, "yue")) return CjkScript::TraditionalChinese;
  if (!subtagIs(primary, "zh") && !subtagIs(primary, "zho") && !subtagIs(primary, "chi") && !subtagIs(primary, "cmn")) {
    return CjkScript::None;
  }
  std::string_view rest = sep == std::string_view::npos ? std::string_view{} : tag.substr(sep + 1);
  while (!rest.empty()) {
    const size_t next = rest.find_first_of("-_");
    const std::string_view sub = rest.substr(0, next);
    if (subtagIs(sub, "hant") || subtagIs(sub, "tw") || subtagIs(sub, "hk") || subtagIs(sub, "mo")) {
      return CjkScript::TraditionalChinese;
    }
    if (subtagIs(sub, "hans") || subtagIs(sub, "cn") || subtagIs(sub, "sg")) return CjkScript::SimplifiedChinese;
    rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1);
  }
  return CjkScript::SimplifiedChinese;
}

// A Han ideograph: CJK Unified, Extension A, the compatibility block and Extensions B-G (rare
// hanzi, common in traditional Chinese names). The one definition the scan, the sniff and the
// font coverage probe share.
inline bool isHan(const uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x3134F);
}

inline bool isChinese(const CjkScript s) {
  return s == CjkScript::SimplifiedChinese || s == CjkScript::TraditionalChinese;
}

// Folder under /dictionaries that holds the converted (DictIndex) files for this script.
inline const char* dictIndexFolder(const CjkScript s) {
  if (s == CjkScript::Japanese) return "jp";
  if (isChinese(s)) return "zh";
  return nullptr;
}

// The folder for a book's language tag. Cantonese is written in traditional characters but is
// its own language with its own words (唔, 嘅, 佢哋), so a yue book reads /dictionaries/yue, where
// CC-Canto sits beside CC-CEDICT; every other tag follows its script.
inline const char* dictIndexFolderForLanguage(std::string_view tag) {
  const std::string_view primary = tag.substr(0, tag.find_first_of("-_"));
  if (subtagIs(primary, "yue")) return "yue";
  return dictIndexFolder(scriptForLanguage(tag));
}

// The folder a book reads: its language tag's, else the one for the script the reader settled on
// (a forced-vertical Latin book reads the Japanese folder; manga without a tag likewise).
inline const char* dictFolderFor(std::string_view tag, const CjkScript fallbackScript) {
  const char* folder = dictIndexFolderForLanguage(tag);
  return folder ? folder : dictIndexFolder(fallbackScript);
}

// A codepoint every usable font for the script must carry, for coverage probes: あ for
// Japanese, 的 for Chinese (the most frequent character, present in every Han font).
inline uint32_t probeCodepoint(const CjkScript s) { return s == CjkScript::Japanese ? 0x3042 : 0x7684; }

// Per-book language choice, stored in the book cache (language.bin) and offered in Reader Settings: 0 follows the
// book's tag (or the content sniff below), the rest force a language on a mis-tagged book.
enum LanguageChoice : uint8_t { LANG_AUTO = 0, LANG_JA = 1, LANG_ZH_HANS = 2, LANG_ZH_HANT = 3, LANG_YUE = 4 };
constexpr uint8_t LANGUAGE_CHOICE_COUNT = 5;

// The language tag a choice stands for; nullptr for Auto.
inline const char* languageTagForChoice(const uint8_t choice) {
  switch (choice) {
    case LANG_JA:
      return "ja";
    case LANG_ZH_HANS:
      return "zh-Hans";
    case LANG_ZH_HANT:
      return "zh-Hant";
    case LANG_YUE:
      return "yue";
    default:
      return nullptr;
  }
}

// Decides a book's language from a sample of its text when the EPUB carries no usable tag:
// kana make it Japanese; a text that is mostly hanzi is Chinese, traditional when the characters
// only traditional text uses outnumber the simplified-only ones. Feed it UTF-8 bytes in any
// chunking; it tolerates markup, which the sample usually still contains.
struct ScriptSniff {
  uint32_t han = 0;
  uint32_t kana = 0;
  uint32_t latin = 0;
  uint32_t simplifiedOnly = 0;
  uint32_t traditionalOnly = 0;
  // A UTF-8 sequence cut by a chunk boundary.
  uint32_t pending = 0;
  uint8_t pendingNeed = 0;
  uint8_t pendingHave = 0;
  bool inTag = false;
  bool inEntity = false;
  // Inside <head>, <style>, <script> or <title>: Latin by the byte but not text. Counting an
  // inline stylesheet would make a Chinese chapter read as Latin.
  bool skipping = false;
  char tagName[8] = {};
  uint8_t tagLen = 0;
  bool tagNameDone = false;
  uint32_t prevInTag = 0;  // the character before '>': a '/' makes the tag self-closing

  static constexpr uint32_t ENOUGH_CJK = 400;  // characters: a paragraph or two settles it
  bool enough() const { return han + kana >= ENOUGH_CJK; }

  void feed(const uint8_t* data, const size_t len) {
    for (size_t i = 0; i < len; i++) {
      const uint8_t b = data[i];
      uint32_t cp = 0;
      if (pendingNeed) {
        if ((b & 0xC0) != 0x80) {
          pendingNeed = 0;  // malformed: resync on this byte
        } else {
          pending = (pending << 6) | (b & 0x3F);
          if (++pendingHave < pendingNeed) continue;
          cp = pending;
          pendingNeed = 0;
          count(cp);
          continue;
        }
      }
      if (b < 0x80) {
        count(b);
      } else if ((b & 0xE0) == 0xC0) {
        pending = b & 0x1F;
        pendingNeed = 2;
        pendingHave = 1;
      } else if ((b & 0xF0) == 0xE0) {
        pending = b & 0x0F;
        pendingNeed = 3;
        pendingHave = 1;
      } else if ((b & 0xF8) == 0xF0) {
        pending = b & 0x07;
        pendingNeed = 4;
        pendingHave = 1;
      }
    }
  }

  CjkScript verdict() const {
    const uint32_t cjk = han + kana;
    if (cjk < 20) return CjkScript::None;
    if (kana * 50 >= cjk) return CjkScript::Japanese;  // 2% kana: no Chinese text has any
    if (han < latin) return CjkScript::None;           // a Latin book quoting a little Chinese
    return traditionalOnly > simplifiedOnly ? CjkScript::TraditionalChinese : CjkScript::SimplifiedChinese;
  }

 private:
  static bool tagIs(const char* name, const char* want) {
    for (size_t i = 0;; i++) {
      if (lowerAscii(name[i]) != want[i]) return false;
      if (want[i] == '\0') return true;
    }
  }

  void count(const uint32_t cp) {
    // Markup is skipped so tag names and attributes do not count as Latin text; the tag name is
    // kept to know when the document's non-text parts begin and end.
    if (inTag) {
      if (cp == '>') {
        inTag = false;
        tagName[tagLen] = '\0';
        const bool selfClosing = prevInTag == '/';  // <script src="x.js"/> opens nothing
        if (!selfClosing && (tagIs(tagName, "head") || tagIs(tagName, "style") || tagIs(tagName, "script") ||
                             tagIs(tagName, "title"))) {
          skipping = true;
        } else if (tagIs(tagName, "/head") || tagIs(tagName, "/style") || tagIs(tagName, "/script") ||
                   tagIs(tagName, "/title") || tagIs(tagName, "body")) {
          skipping = false;
        }
      } else if (!tagNameDone && tagLen < sizeof(tagName) - 1 &&
                 (cp == '/' || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9'))) {
        tagName[tagLen++] = static_cast<char>(cp);
      } else {
        tagNameDone = true;
      }
      prevInTag = cp;
      return;
    }
    if (cp == '<') {
      inTag = true;
      tagLen = 0;
      tagNameDone = false;
      prevInTag = 0;
      return;
    }
    // &nbsp; and friends: four Latin letters per indent in many Chinese EPUBs.
    if (inEntity) {
      if (cp == ';' || !((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= '0' && cp <= '9') || cp == '#'))
        inEntity = false;
      return;
    }
    if (cp == '&') {
      inEntity = true;
      return;
    }
    if (skipping) return;
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= 0x00C0 && cp <= 0x024F)) {
      latin++;
    } else if ((cp >= 0x3040 && cp <= 0x30FF)) {
      kana++;
    } else if (isHan(cp)) {
      han++;
      if (isSimplifiedOnly(cp)) simplifiedOnly++;
      if (isTraditionalOnly(cp)) traditionalOnly++;
    }
  }

  // The commonest characters that exist in only one of the two scripts, in matching pairs.
  static bool isSimplifiedOnly(const uint32_t cp) {
    static constexpr uint16_t kSimplified[] = {
        0x8FD9, 0x8BF4, 0x4EEC, 0x4E2A, 0x4E48, 0x65F6, 0x56FD, 0x6765, 0x5BF9, 0x4F1A, 0x53D1, 0x4E3A, 0x8FD8, 0x6CA1,
        0x8FC7, 0x6837, 0x5F00, 0x5B66, 0x73B0, 0x540E, 0x70B9, 0x89C1, 0x95EE, 0x4E1C, 0x95E8, 0x8F66, 0x4E66, 0x957F,
        0x51E0, 0x5E94, 0x4E24, 0x8BA4, 0x8BA9, 0x7ECF, 0x5173, 0x5B9E, 0x8BDD, 0x542C, 0x4ECE, 0x5934, 0x5C14, 0x4E1A,
        0x7231, 0x56FE, 0x7535, 0x673A, 0x4F53, 0x8BD5, 0x5199, 0x8BFB, 0x9A6C, 0x9E1F, 0x9F99, 0x53F6, 0x4E07, 0x4E0E};
    for (const uint16_t c : kSimplified) {
      if (c == cp) return true;
    }
    return false;
  }
  static bool isTraditionalOnly(const uint32_t cp) {
    static constexpr uint16_t kTraditional[] = {
        0x9019, 0x8AAA, 0x5011, 0x500B, 0x9EBC, 0x6642, 0x570B, 0x4F86, 0x5C0D, 0x6703, 0x767C, 0x70BA, 0x9084, 0x6C92,
        0x904E, 0x6A23, 0x958B, 0x5B78, 0x73FE, 0x5F8C, 0x9EDE, 0x898B, 0x554F, 0x6771, 0x9580, 0x8ECA, 0x66F8, 0x9577,
        0x5E7E, 0x61C9, 0x5169, 0x8A8D, 0x8B93, 0x7D93, 0x95DC, 0x5BE6, 0x8A71, 0x807D, 0x5F9E, 0x982D, 0x723E, 0x696D,
        0x611B, 0x5716, 0x96FB, 0x6A5F, 0x9AD4, 0x8A66, 0x5BEB, 0x8B80, 0x99AC, 0x9CE5, 0x9F8D, 0x8449, 0x842C, 0x8207};
    for (const uint16_t c : kTraditional) {
      if (c == cp) return true;
    }
    return false;
  }
};

}  // namespace cjk
