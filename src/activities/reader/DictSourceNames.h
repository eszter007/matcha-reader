#pragma once

#include <DictIndex.h>
#include <I18n.h>

#include <cstdint>
#include <cstring>

// Footer labels for the three converted-dictionary slots. The converter's title file wins; the
// Japanese folder keeps the names its users know; any other language gets a generic word.
namespace dictsource {

inline const char* name(const uint8_t dict) {
  const char* title = DictIndex::slotTitle(dict);
  if (title && title[0] != '\0') return title;
  const bool jp = std::strcmp(DictIndex::languageFolder(), "jp") == 0;
  if (dict == DictIndex::DICT_GRAMMAR) return tr(STR_DICT_KIND_GRAMMAR);
  if (dict == DictIndex::DICT_NAMES) return jp ? "JMnedict" : tr(STR_DICT_KIND_NAME);
  return jp ? "JMdict" : tr(STR_DICTIONARY);
}

inline StrId kind(const uint8_t dict) {
  if (dict == DictIndex::DICT_GRAMMAR) return StrId::STR_DICT_KIND_GRAMMAR;
  if (dict == DictIndex::DICT_NAMES) return StrId::STR_DICT_KIND_NAME;
  return StrId::STR_DICT_KIND_VOCAB;
}

}  // namespace dictsource
