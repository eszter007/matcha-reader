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

// A codepoint every usable font for the script must carry, for coverage probes: あ for
// Japanese, 的 for Chinese (the most frequent character, present in every Han font).
inline uint32_t probeCodepoint(const CjkScript s) { return s == CjkScript::Japanese ? 0x3042 : 0x7684; }

}  // namespace cjk
