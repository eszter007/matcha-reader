#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Word class conditions for deinflection rule chaining.
// A rule applies only when the input word's condition matches condIn,
// and the output candidate gets condOut for further chaining.
enum class WordCondition : uint8_t {
  V1 = 0,     // ichidan verb (食べる)
  V5 = 1,     // godan verb (書く)
  VS = 2,     // suru verb (する)
  VK = 3,     // kuru verb (来る)
  ADJ_I = 4,  // i-adjective (美しい)
  DICT = 5,   // any dictionary form (terminal)
};

struct DeinflectionCandidate {
  std::string text;
  WordCondition condition;
};

// Given a surface-form string, returns plausible dictionary-form candidates
// by applying Japanese conjugation rules (verb/adjective suffix stripping).
// Rules are linguistic facts reimplemented independently; see Yomitan's
// deinflection engine for the reference behavior specification.
class Deinflector {
 public:
  static std::vector<DeinflectionCandidate> deinflect(const std::string& surface);
};
