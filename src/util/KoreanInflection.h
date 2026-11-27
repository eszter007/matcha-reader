#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Dictionary-form candidates for a Korean word as it appears in text: particles and the copula
// come off nouns (학교에서 → 학교, 학생이에요 → 학생), and verb and adjective endings are undone
// to the 다 form (먹었습니다 → 먹다, 갔어요 → 가다, 도와줘 → 도와주다, 더워요 → 덥다). Covers the
// speech levels, tense, honorific 시, connective and adnominal endings, vowel contraction and the
// irregular stems (ㅂ ㄷ ㅅ ㅎ 르, ㄹ and 으 dropping). Pure string work; the dictionary decides
// which candidate is a word. Probe order: particles, copula, then predicate analyses.
namespace KoreanInflection {

inline constexpr size_t MAX_VARIANTS = 40;

// Clears `out`; never emits `word` itself or a duplicate; stops at MAX_VARIANTS.
void variants(const std::string& word, std::vector<std::string>& out);

}  // namespace KoreanInflection
