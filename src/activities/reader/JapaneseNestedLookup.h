#pragma once

#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "DefinitionTextRenderer.h"
#include "DictionaryDefinitionActivity.h"

// Lookups inside a Japanese dictionary definition (an example sentence's words), in the converted
// Japanese/Chinese dictionaries the word-lookup panels read.
namespace japanese_nested {

// The text a lookup at byte `offset` of a definition reads: from that character to the end of its
// line, as long as the longest conjugation a lookup can match. Empty when the character there is
// not CJK: those dictionaries are keyed by CJK headwords only.
std::string lookupTextAt(const std::string& text, size_t offset);

// DictionaryDefinitionActivity::NestedLookup: the longest entry at the start of `text`.
bool lookup(void* ctx, const char* text, std::vector<DictionaryDefinitionActivity::Entry>& out, StrId& failure);

// The lookup text under a long press at (x, y) on `text` drawn at `geometry`, or empty when no CJK
// character is there. Hold RenderLock: it measures through the shared font cache.
std::string wordAt(const GfxRenderer& renderer, const DefinitionText::WrapGeometry& geometry, const std::string& text,
                   int x, int y);

// The first panel of a chain for `word` (from wordAt or lookupTextAt), or null with the reason in
// `failure`. Call without RenderLock: it reads the dictionary from SD.
std::unique_ptr<DictionaryDefinitionActivity> open(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   const std::string& word, StrId& failure);

}  // namespace japanese_nested
