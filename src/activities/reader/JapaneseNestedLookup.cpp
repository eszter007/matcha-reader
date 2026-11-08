#include "JapaneseNestedLookup.h"

#include <DictIndex.h>
#include <Utf8.h>
#include <WordLookup.h>

#include "DefinitionTextRenderer.h"
#include "DictSourceNames.h"
#include "WordSelectionScan.h"

namespace japanese_nested {

std::string lookupTextAt(const std::string& text, const size_t offset) {
  if (offset >= text.size()) return {};
  const auto* start = reinterpret_cast<const unsigned char*>(text.c_str() + offset);
  const auto* next = start;
  if (!utf8IsCjkCodepoint(utf8NextCodepoint(&next))) return {};
  // CJK characters are 3-4 bytes; a lookup never reads past MAX_CONJUGATION_CHARS of them.
  static constexpr size_t kMaxBytes = WordLookup::MAX_CONJUGATION_CHARS * 4;
  size_t end = text.find('\n', offset);
  if (end == std::string::npos) end = text.size();
  end = std::min(end, offset + kMaxBytes);
  while (end > offset && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) end--;
  return text.substr(offset, end - offset);
}

bool lookup(void* /*ctx*/, const char* text, std::vector<DictionaryDefinitionActivity::Entry>& out, StrId& failure) {
  const std::string source(text);
  WordLookupResult result;
  if (!WordLookup::lookup(source, 0, result)) {
    failure = StrId::STR_NO_MATCH;
    return false;
  }
  WordSelectionScan::stripTrailingParticle(source, result);
  DictionaryDefinitionActivity::Entry entry;
  entry.headword = std::move(result.entry.headword);
  entry.definition = std::move(result.entry.definition);
  DefinitionText::EntryMetadata metadata;
  DefinitionText::extractEntryMetadata(entry.definition, entry.headword, metadata);
  const uint8_t dict = result.entry.sourceDict;
  DefinitionText::formatEntryBody(entry.definition, dict == DictIndex::DICT_GRAMMAR ? entry.headword : std::string(),
                                  metadata.grammar);
  entry.reading = std::move(metadata.reading);
  entry.grammar = std::move(metadata.grammar);
  entry.dictName = metadata.source.empty() ? std::string(dictsource::name(dict)) : std::move(metadata.source);
  entry.definitionText = true;
  out.push_back(std::move(entry));
  return true;
}

std::string wordAt(const GfxRenderer& renderer, const DefinitionText::WrapGeometry& geometry, const std::string& text,
                   const int x, const int y) {
  return lookupTextAt(text, DefinitionText::hitTest(renderer, geometry, text, x, y));
}

}  // namespace japanese_nested
