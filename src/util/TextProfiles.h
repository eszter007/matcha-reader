#pragma once

#include <I18n.h>

#include <string_view>

#include "CrossPointSettings.h"
#include "util/CjkScript.h"

namespace textprofile {

using Id = CrossPointSettings::TextProfileId;

// The profile a book reads with: its CJK script, or Korean from a ko/kor tag.
inline Id forBook(const CjkScript script, const std::string_view language) {
  switch (script) {
    case CjkScript::Japanese:
      return Id::Japanese;
    case CjkScript::TraditionalChinese:
      return Id::TraditionalChinese;
    case CjkScript::SimplifiedChinese:
      return Id::SimplifiedChinese;
    case CjkScript::None:
      break;
  }
  const std::string_view primary = language.substr(0, language.find_first_of("-_"));
  return cjk::subtagIs(primary, "ko") || cjk::subtagIs(primary, "kor") ? Id::Korean : Id::Latin;
}

// The script Text Settings previews and filters rows for while editing a profile outside a book.
inline CjkScript script(const Id id) {
  switch (id) {
    case Id::Japanese:
      return CjkScript::Japanese;
    case Id::TraditionalChinese:
      return CjkScript::TraditionalChinese;
    case Id::SimplifiedChinese:
      return CjkScript::SimplifiedChinese;
    default:
      return CjkScript::None;
  }
}

inline const char* title(const Id id) {
  switch (id) {
    case Id::Japanese:
      return tr(STR_TEXT_SETTINGS_JA);
    case Id::TraditionalChinese:
      return tr(STR_TEXT_SETTINGS_ZHT);
    case Id::SimplifiedChinese:
      return tr(STR_TEXT_SETTINGS_ZHS);
    case Id::Korean:
      return tr(STR_TEXT_SETTINGS_KO);
    default:
      return tr(STR_TEXT_SETTINGS_LATIN);
  }
}

}  // namespace textprofile
