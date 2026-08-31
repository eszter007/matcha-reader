#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Lemma candidates to retry after a dictionary lookup misses on the surface
// form. Pure string work — no HAL, no SD — so the rule sets are unit-testable
// on the host (test/inflection_rules).
namespace InflectionRules {

// Every candidate costs one index probe: a sampled bisect over .qidx plus a
// linear scan of up to SAMPLE_INTERVAL .idx entries, each headword read a byte
// at a time under storageMutex. The rule sets are therefore capped rather than
// exhaustive — coverage past this point belongs in a .syn on the SD card, which
// resolves in a single probe and costs no flash.
inline constexpr size_t MAX_VARIANTS = 12;

enum class Language : uint8_t {
  Generic,  // unknown language: the English suffix set, applied to everything
  French,
};

// Map an EPUB language tag or dictionary folder prefix ("fr", "fr-FR", "FR") to
// a rule set. Anything unrecognised is Generic.
Language languageFromCode(const char* code);

// Fill `out` with lemma candidates for `word` in probe order: non-ASCII case
// fold, French elision, then suffix rewrites longest-suffix-first. `word` is a
// cleaned lookup key (Dictionary::cleanWord). Clears `out`, never emits the
// input or a duplicate, and stops at MAX_VARIANTS.
void variants(Language language, const std::string& word, std::vector<std::string>& out);

}  // namespace InflectionRules
