#include "Deinflector.h"

#include <algorithm>
#include <cstring>

namespace {

struct Rule {
  const char* from;
  const char* to;
  WordCondition condIn;
  WordCondition condOut;
};

// Japanese deinflection rules — linguistic facts covering common
// verb/adjective conjugation patterns.  Organized by conjugation type.
// Rules are tried against the END of the input string; if "from" matches
// the suffix, it's replaced by "to" and the candidate gets condOut.
//
// condIn gates which prior condition allows this rule to fire (DICT
// means "accept from raw surface form — no prior rule needed").
// condOut is what the output candidate is tagged as for further chaining
// or for final dictionary lookup.

// clang-format off
static constexpr Rule kRules[] = {
    // ございます in the past. First, so its unclassed candidate is the one kept: した→す reaches the
    // same text as a godan verb, which ございます is not. Other verbs need no ました rule.
    {"\xe3\x81\x94\xe3\x81\x96\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x94\xe3\x81\x96\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99", WordCondition::DICT, WordCondition::DICT},  // ございました→ございます

    // ── Ichidan (v1) verbs: dictionary form ends in -る ──────────
    // Negative
    {"\xe3\x81\xaa\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ない→る
    // Past
    {"\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // た→る
    // Te-form
    {"\xe3\x81\xa6", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // て→る
    // Polite
    {"\xe3\x81\xbe\xe3\x81\x99", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ます→る
    {"\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ません→る
    // Polite volitional: rewritten to ます so the ます rules above and below finish it.
    {"\xe3\x81\xbe\xe3\x81\x97\xe3\x82\x87\xe3\x81\x86", "\xe3\x81\xbe\xe3\x81\x99", WordCondition::DICT, WordCondition::DICT},  // ましょう→ます
    // Passive/potential
    {"\xe3\x82\x89\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // られる→る
    // Causative
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // させる→る
    // Volitional
    {"\xe3\x82\x88\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // よう→る
    // Conditional
    {"\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // れば→る
    // Desire
    {"\xe3\x81\x9f\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // たい→る
    // Suru progressive/compound て-forms. MUST precede the generic ichidan ている→る rule:
    // rule order is candidate order, and the ichidan rule turns している into しる -- 知る, a real
    // headword that then shadows する in the lookup's first-match-wins candidate walk. Mirrors the
    // godan している→す block below, which is equally shadowed without these.
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // している→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // していた→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // していない→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // しています→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xa6", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // していて→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // してきた→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // してくる→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x8f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // していく→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // していった→する
    // Progressive て+いる and compound auxiliaries
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ている→る
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ていた→る
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xaa\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ていない→る
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ています→る
    {"\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てきた→る (came to do)
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てくる→る
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x8f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ていく→る (going to do)
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ていった→る
    {"\xe3\x81\xa6\xe3\x81\x8a\xe3\x81\x8f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ておく→る (do in advance)
    {"\xe3\x81\xa6\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てしまう→る
    {"\xe3\x81\xa6\xe3\x81\x97\xe3\x81\xbe\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てしまった→る
    // Colloquial PAST contractions of てしまった / でしまった: strip the しまった part, leaving the
    // bare te-form (keeping euphonic っ/ん/い) which the te-form rules below reduce to every verb
    // class -- たまっちゃった -> たまって -> たまる, 死んじゃった -> 死んで -> 死ぬ. Only the PAST
    // (った) forms: the present ちゃう/じゃう were tried and removed -- a godan renyoukei い→う rule
    // turns ～ちゃい into ～ちゃう, so ちゃう→て mis-ate the い of the common ～ちゃいけない ("must
    // not", where ちゃ = ては, NOT ちゃう). Greedy segmentation can't resolve that ちゃ ambiguity;
    // the past forms don't end in い so they don't hit it.
    {"\xe3\x81\xa1\xe3\x82\x83\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::DICT},  // ちゃった→て
    {"\xe3\x81\x98\xe3\x82\x83\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::DICT},  // じゃった→で
    {"\xe3\x81\xa6\xe3\x81\xbf\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てみる→る (try doing)
    {"\xe3\x81\xa6\xe3\x81\xbf\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てみた→る
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てある→る (state result)
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てあった→る
    {"\xe3\x81\xa6\xe3\x82\x82\xe3\x82\x89\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てもらう→る (receive favor)
    {"\xe3\x81\xa6\xe3\x82\x82\xe3\x82\x89\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てもらった→る
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x81\x92\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てあげる→る (give favor)
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てくれる→る (receive favor)
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てくれた→る
    {"\xe3\x81\xa6\xe3\x81\xbb\xe3\x81\x97\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てほしい→る (want someone to do)

    // Godan て-form compound auxiliaries (って+aux for godan verbs)
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // っている→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // っていた→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // ってきた→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // ってくる→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // っている→つ
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // っていた→つ
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ってきた→つ
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // っている→る (godan る)
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // っていた→る
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // ってきた→る
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x84\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んでいる→む
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x84\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んでいた→む
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んできた→む
    {"\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いている→く
    {"\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いていた→く
    {"\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いてきた→く
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // している→す
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // していた→す
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // してきた→す

    // ── Godan (v5) verbs: dictionary form ends in う-row kana ────
    // Past/te-form: consonant-stem euphonic changes
    // K-column: く→いた/いて
    {"\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いた→く
    {"\xe3\x81\x84\xe3\x81\xa6", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いて→く
    // G-column: ぐ→いだ/いで
    {"\xe3\x81\x84\xe3\x81\xa0", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // いだ→ぐ
    {"\xe3\x81\x84\xe3\x81\xa7", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // いで→ぐ
    // S-column: す→した/して
    {"\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // した→す
    {"\xe3\x81\x97\xe3\x81\xa6", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // して→す
    // T-column: つ→った/って
    {"\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // った→つ
    {"\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // って→つ
    // N-column: ぬ→んだ/んで
    {"\xe3\x82\x93\xe3\x81\xa0", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // んだ→ぬ
    {"\xe3\x82\x93\xe3\x81\xa7", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // んで→ぬ
    // B-column: ぶ→んだ/んで
    {"\xe3\x82\x93\xe3\x81\xa0", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // んだ→ぶ
    {"\xe3\x82\x93\xe3\x81\xa7", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // んで→ぶ
    // M-column: む→んだ/んで
    {"\xe3\x82\x93\xe3\x81\xa0", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んだ→む
    {"\xe3\x82\x93\xe3\x81\xa7", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んで→む
    // R-column: る→った/って (godan る, not ichidan)
    {"\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // った→る
    {"\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // って→る
    // U-column: う→った/って
    {"\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // った→う
    {"\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // って→う

    // Godan negative: replace あ-row ending + ない
    {"\xe3\x81\x8b\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かない→く
    {"\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がない→ぐ
    {"\xe3\x81\x95\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // さない→す
    {"\xe3\x81\x9f\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たない→つ
    {"\xe3\x81\xaa\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なない→ぬ
    {"\xe3\x81\xb0\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばない→ぶ
    {"\xe3\x81\xbe\xe3\x81\xaa\xe3\x81\x84", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まない→む
    {"\xe3\x82\x89\xe3\x81\xaa\xe3\x81\x84", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らない→る
    {"\xe3\x82\x8f\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わない→う

    // Godan polite -ます: replace い-row + ます
    {"\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // きます→く
    {"\xe3\x81\x8e\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎます→ぐ
    {"\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // します→す
    {"\xe3\x81\xa1\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ちます→つ
    {"\xe3\x81\xab\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // にます→ぬ
    {"\xe3\x81\xb3\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // びます→ぶ
    {"\xe3\x81\xbf\xe3\x81\xbe\xe3\x81\x99", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // みます→む
    {"\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // ります→る
    {"\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // います→う

    // Godan polite negative -ません: replace い-row + ません → う-row
    // (ありません→ある). Without these, the ichidan ません→る rule wrongly
    // produces ありる.
    {"\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // きません→く
    {"\xe3\x81\x8e\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎません→ぐ
    {"\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // しません→す
    {"\xe3\x81\xa1\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ちません→つ
    {"\xe3\x81\xab\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // にません→ぬ
    {"\xe3\x81\xb3\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // びません→ぶ
    {"\xe3\x81\xbf\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // みません→む
    {"\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // りません→る
    {"\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // いません→う

    // Godan polite negative PAST -ませんでした: replace い-row + ませんでした → う-row
    // (ありませんでした→ある). Must precede the shorter ません rules in coverage.
    {"\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // きませんでした→く
    {"\xe3\x81\x8e\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎませんでした→ぐ
    {"\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // しませんでした→す
    {"\xe3\x81\xa1\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ちませんでした→つ
    {"\xe3\x81\xab\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // にませんでした→ぬ
    {"\xe3\x81\xb3\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // びませんでした→ぶ
    {"\xe3\x81\xbf\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // みませんでした→む
    {"\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // りませんでした→る
    {"\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // いませんでした→う

    // Ichidan polite negative past: ませんでした→る (食べませんでした→食べる)
    {"\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ませんでした→る

    // Negative conditional なければ, per verb class. Without direct rules this only resolves by
    // CHAINING through ければ→い (adj-i conditional) -- しなければ→しない→(い→う)→しなう, and 撓う
    // is a real godan verb, so the pre-する candidate wins the lookup (device report:
    // しなければいけません showed 撓う). Direct rules are generated first, so the right verb wins
    // the candidate order. Suru/kuru first (longest/most specific), then godan rows, then the
    // generic ichidan fallback.
    {"\xe3\x81\x97\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // しなければ→する
    {"\xe3\x81\x93\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // こなければ→くる
    {"\xe3\x81\x8b\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かなければ→く
    {"\xe3\x81\x8c\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がなければ→ぐ
    {"\xe3\x81\x95\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // さなければ→す
    {"\xe3\x81\x9f\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たなければ→つ
    {"\xe3\x81\xaa\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // ななければ→ぬ
    {"\xe3\x81\xb0\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばなければ→ぶ
    {"\xe3\x81\xbe\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まなければ→む
    {"\xe3\x82\x89\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らなければ→る
    {"\xe3\x82\x8f\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わなければ→う
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // くなければ→い (高くなければ→高い)
    {"\xe3\x81\xaa\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // なければ→る (食べなければ→食べる)

    // Godan masu-stem (連用形): bare い-row ending (used for compound verbs and nominal forms)
    {"\xe3\x81\x8d", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // き→く
    {"\xe3\x81\x8e", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎ→ぐ
    {"\xe3\x81\x97", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // し→す
    {"\xe3\x81\xa1", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ち→つ
    {"\xe3\x81\xab", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // に→ぬ
    {"\xe3\x81\xb3", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // び→ぶ
    {"\xe3\x81\xbf", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // み→む
    {"\xe3\x82\x8a", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // り→る
    {"\xe3\x81\x84", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // い→う

    // Godan volitional: replace お-row + う → dictionary う-row ending
    {"\xe3\x81\x93\xe3\x81\x86", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // こう→く
    {"\xe3\x81\x94\xe3\x81\x86", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ごう→ぐ
    {"\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // そう→す
    {"\xe3\x81\xa8\xe3\x81\x86", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // とう→つ
    {"\xe3\x81\xae\xe3\x81\x86", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // のう→ぬ
    {"\xe3\x81\xbc\xe3\x81\x86", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ぼう→ぶ
    {"\xe3\x82\x82\xe3\x81\x86", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // もう→む
    {"\xe3\x82\x8d\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // ろう→る
    {"\xe3\x81\x8a\xe3\x81\x86", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // おう→う

    // Godan passive: replace あ-row + れる
    {"\xe3\x81\x8b\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かれる→く
    {"\xe3\x81\x8c\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がれる→ぐ
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // される→す
    {"\xe3\x81\x9f\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たれる→つ
    {"\xe3\x81\xaa\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なれる→ぬ
    {"\xe3\x81\xb0\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばれる→ぶ
    {"\xe3\x81\xbe\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まれる→む
    {"\xe3\x82\x89\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // られる→る
    {"\xe3\x82\x8f\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // われる→う

    // Godan passive masu-stem: replace あ-row + れ (continuative before comma, conjunctive)
    {"\xe3\x81\x8b\xe3\x82\x8c", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かれ→く
    {"\xe3\x81\x8c\xe3\x82\x8c", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がれ→ぐ
    {"\xe3\x81\x95\xe3\x82\x8c", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // され→す
    {"\xe3\x81\x9f\xe3\x82\x8c", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たれ→つ
    {"\xe3\x81\xaa\xe3\x82\x8c", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なれ→ぬ
    {"\xe3\x81\xb0\xe3\x82\x8c", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばれ→ぶ
    {"\xe3\x81\xbe\xe3\x82\x8c", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まれ→む
    {"\xe3\x82\x89\xe3\x82\x8c", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // られ→る
    {"\xe3\x82\x8f\xe3\x82\x8c", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // われ→う

    // Godan causative masu-stem: replace あ-row + せ
    {"\xe3\x81\x8b\xe3\x81\x9b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かせ→く
    {"\xe3\x81\x8c\xe3\x81\x9b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がせ→ぐ
    {"\xe3\x81\x95\xe3\x81\x9b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // させ→す
    {"\xe3\x81\x9f\xe3\x81\x9b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たせ→つ
    {"\xe3\x81\xaa\xe3\x81\x9b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なせ→ぬ
    {"\xe3\x81\xb0\xe3\x81\x9b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばせ→ぶ
    {"\xe3\x81\xbe\xe3\x81\x9b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // ませ→む
    {"\xe3\x82\x89\xe3\x81\x9b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らせ→る
    {"\xe3\x82\x8f\xe3\x81\x9b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わせ→う

    // Ichidan passive/potential and causative masu-stem
    {"\xe3\x82\x89\xe3\x82\x8c", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // られ→る
    {"\xe3\x81\x95\xe3\x81\x9b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // させ→る

    // Suru passive/causative masu-stem
    {"\xe3\x81\x95\xe3\x82\x8c", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // され→する
    {"\xe3\x81\x95\xe3\x81\x9b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させ→する

    // Ichidan masu-stem (連用形): stem appears before commas, auxiliaries.
    // Add る to try the dictionary form. These え-row endings are common
    // ichidan stems: め→める, べ→べる, け→ける, etc.
    {"\xe3\x82\x81", "\xe3\x82\x81\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // め→める
    {"\xe3\x81\xb9", "\xe3\x81\xb9\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // べ→べる
    {"\xe3\x81\x91", "\xe3\x81\x91\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // け→ける
    {"\xe3\x81\x9b", "\xe3\x81\x9b\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // せ→せる
    {"\xe3\x81\xa6", "\xe3\x81\xa6\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // て→てる
    {"\xe3\x81\xad", "\xe3\x81\xad\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ね→ねる
    {"\xe3\x81\xb8", "\xe3\x81\xb8\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // へ→へる
    {"\xe3\x82\x8c", "\xe3\x82\x8c\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // れ→れる
    {"\xe3\x81\x88", "\xe3\x81\x88\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // え→える
    {"\xe3\x81\x92", "\xe3\x81\x92\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // げ→げる
    {"\xe3\x81\xa7", "\xe3\x81\xa7\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // で→でる
    {"\xe3\x81\xbe", "\xe3\x81\xbe\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ま→まる (not ideal but catches some)

    // Godan passive past: replace あ-row + れた
    {"\xe3\x81\x8b\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かれた→く
    {"\xe3\x81\x8c\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がれた→ぐ
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // された→す
    {"\xe3\x81\x9f\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たれた→つ
    {"\xe3\x81\xaa\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なれた→ぬ
    {"\xe3\x81\xb0\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばれた→ぶ
    {"\xe3\x81\xbe\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まれた→む
    {"\xe3\x82\x89\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // られた→る
    {"\xe3\x82\x8f\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // われた→う
    // Godan passive te-form: replace あ-row + れて
    {"\xe3\x81\x8b\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かれて→く
    {"\xe3\x81\x8c\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がれて→ぐ
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // されて→す
    {"\xe3\x81\x9f\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たれて→つ
    {"\xe3\x81\xaa\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なれて→ぬ
    {"\xe3\x81\xb0\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばれて→ぶ
    {"\xe3\x81\xbe\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まれて→む
    {"\xe3\x82\x89\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // られて→る
    {"\xe3\x82\x8f\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // われて→う

    // Godan causative: replace あ-row + せる
    {"\xe3\x81\x8b\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かせる→く
    {"\xe3\x81\x8c\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がせる→ぐ
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // させる→す
    {"\xe3\x81\x9f\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たせる→つ
    {"\xe3\x81\xaa\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なせる→ぬ
    {"\xe3\x81\xb0\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばせる→ぶ
    {"\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // ませる→む
    {"\xe3\x82\x89\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らせる→る
    {"\xe3\x82\x8f\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わせる→う

    // Godan causative past: replace あ-row + せた
    {"\xe3\x81\x8b\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かせた→く
    {"\xe3\x81\x8c\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がせた→ぐ
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // させた→す
    {"\xe3\x81\x9f\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たせた→つ
    {"\xe3\x82\x89\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らせた→る
    {"\xe3\x82\x8f\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わせた→う

    // Godan potential: replace え-row + る
    {"\xe3\x81\x91\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // ける→く
    {"\xe3\x81\x92\xe3\x82\x8b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // げる→ぐ
    {"\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // せる→す
    {"\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // てる→つ
    {"\xe3\x81\xad\xe3\x82\x8b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // ねる→ぬ
    {"\xe3\x81\xb9\xe3\x82\x8b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // べる→ぶ
    {"\xe3\x82\x81\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // める→む
    {"\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // れる→る
    {"\xe3\x81\x88\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // える→う

    // Classical negative -ず (笑わず, 食べず, せず, こず).
    {"\xe3\x81\x8b\xe3\x81\x9a", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // かず→く
    {"\xe3\x81\x8c\xe3\x81\x9a", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // がず→ぐ
    {"\xe3\x81\x95\xe3\x81\x9a", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // さず→す
    {"\xe3\x81\x9f\xe3\x81\x9a", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // たず→つ
    {"\xe3\x81\xaa\xe3\x81\x9a", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // なず→ぬ
    {"\xe3\x81\xb0\xe3\x81\x9a", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // ばず→ぶ
    {"\xe3\x81\xbe\xe3\x81\x9a", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // まず→む
    {"\xe3\x82\x89\xe3\x81\x9a", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // らず→る
    {"\xe3\x82\x8f\xe3\x81\x9a", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // わず→う
    {"\xe3\x81\x9a", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // ず→る
    {"\xe3\x81\x9b\xe3\x81\x9a", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // せず→する
    {"\xe3\x81\x93\xe3\x81\x9a", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // こず→くる
    // Colloquial -てる/-てた for -ている/-ていた (食べてる, 読んでる, してた).
    {"\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てる→る
    {"\xe3\x81\xa6\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // てた→る
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // ってる→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ってる→つ
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // ってる→る
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x82\x8b", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んでる→む
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x82\x8b", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // んでる→ぬ
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x82\x8b", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // んでる→ぶ
    {"\xe3\x81\x84\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いてる→く
    {"\xe3\x81\x84\xe3\x81\xa7\xe3\x82\x8b", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // いでる→ぐ
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // してる→す
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x9f", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // ってた→う
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x9f", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ってた→つ
    {"\xe3\x81\xa3\xe3\x81\xa6\xe3\x81\x9f", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // ってた→る
    {"\xe3\x82\x93\xe3\x81\xa7\xe3\x81\x9f", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // んでた→む
    {"\xe3\x81\x84\xe3\x81\xa6\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // いてた→く
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x9f", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // してた→す
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // してる→する
    {"\xe3\x81\x97\xe3\x81\xa6\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // してた→する
    // Appearance -そう on the stem (食べそう, 降りそう, 高そう, 勉強しそう). Ichidan first: できそう is
    // できる, not the rare godan reading the い-row stem also allows.
    {"\xe3\x81\x9d\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V1},  // そう→る
    {"\xe3\x81\x8d\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // きそう→く
    {"\xe3\x81\x8e\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\x90", WordCondition::DICT, WordCondition::V5},  // ぎそう→ぐ
    {"\xe3\x81\x97\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\x99", WordCondition::DICT, WordCondition::V5},  // しそう→す
    {"\xe3\x81\xa1\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\xa4", WordCondition::DICT, WordCondition::V5},  // ちそう→つ
    {"\xe3\x81\xab\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\xac", WordCondition::DICT, WordCondition::V5},  // にそう→ぬ
    {"\xe3\x81\xb3\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\xb6", WordCondition::DICT, WordCondition::V5},  // びそう→ぶ
    {"\xe3\x81\xbf\xe3\x81\x9d\xe3\x81\x86", "\xe3\x82\x80", WordCondition::DICT, WordCondition::V5},  // みそう→む
    {"\xe3\x82\x8a\xe3\x81\x9d\xe3\x81\x86", "\xe3\x82\x8b", WordCondition::DICT, WordCondition::V5},  // りそう→る
    {"\xe3\x81\x84\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\x86", WordCondition::DICT, WordCondition::V5},  // いそう→う
    {"\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // そう→い
    {"\xe3\x81\x97\xe3\x81\x9d\xe3\x81\x86", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // しそう→する

    // て-form + auxiliary back to the bare て/で form, unclassed, so the て rows above finish it for
    // every verb class (してくれた → して → する, 帰っていった → 帰って → 帰る).
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ている→て
    {"\xe3\x81\xa7\xe3\x81\x84\xe3\x82\x8b", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でいる→で
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ていた→て
    {"\xe3\x81\xa7\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でいた→で
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ていない→て
    {"\xe3\x81\xa7\xe3\x81\x84\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でいない→で
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ています→て
    {"\xe3\x81\xa7\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でいます→で
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ていました→て
    {"\xe3\x81\xa7\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でいました→で
    {"\xe3\x81\xa6\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てきた→て
    {"\xe3\x81\xa7\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // できた→で
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8b", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てくる→て
    {"\xe3\x81\xa7\xe3\x81\x8f\xe3\x82\x8b", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でくる→で
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x8f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ていく→て
    {"\xe3\x81\xa7\xe3\x81\x84\xe3\x81\x8f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でいく→で
    {"\xe3\x81\xa6\xe3\x81\x84\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ていった→て
    {"\xe3\x81\xa7\xe3\x81\x84\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でいった→で
    {"\xe3\x81\xa6\xe3\x81\x8a\xe3\x81\x8f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ておく→て
    {"\xe3\x81\xa7\xe3\x81\x8a\xe3\x81\x8f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でおく→で
    {"\xe3\x81\xa6\xe3\x81\x8a\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // ておいた→て
    {"\xe3\x81\xa7\xe3\x81\x8a\xe3\x81\x84\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でおいた→で
    {"\xe3\x81\xa6\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x86", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てしまう→て
    {"\xe3\x81\xa7\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x86", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でしまう→で
    {"\xe3\x81\xa6\xe3\x81\x97\xe3\x81\xbe\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てしまった→て
    {"\xe3\x81\xa7\xe3\x81\x97\xe3\x81\xbe\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でしまった→で
    {"\xe3\x81\xa6\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てしまいました→て
    {"\xe3\x81\xa7\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x84\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でしまいました→で
    {"\xe3\x81\xa6\xe3\x81\xbf\xe3\x82\x8b", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てみる→て
    {"\xe3\x81\xa7\xe3\x81\xbf\xe3\x82\x8b", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でみる→で
    {"\xe3\x81\xa6\xe3\x81\xbf\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てみた→て
    {"\xe3\x81\xa7\xe3\x81\xbf\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でみた→で
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x82\x8b", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てある→て
    {"\xe3\x81\xa7\xe3\x81\x82\xe3\x82\x8b", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // である→で
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てあった→て
    {"\xe3\x81\xa7\xe3\x81\x82\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // であった→で
    {"\xe3\x81\xa6\xe3\x82\x82\xe3\x82\x89\xe3\x81\x86", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てもらう→て
    {"\xe3\x81\xa7\xe3\x82\x82\xe3\x82\x89\xe3\x81\x86", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でもらう→で
    {"\xe3\x81\xa6\xe3\x82\x82\xe3\x82\x89\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てもらった→て
    {"\xe3\x81\xa7\xe3\x82\x82\xe3\x82\x89\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でもらった→で
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x81\x92\xe3\x82\x8b", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てあげる→て
    {"\xe3\x81\xa7\xe3\x81\x82\xe3\x81\x92\xe3\x82\x8b", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // であげる→で
    {"\xe3\x81\xa6\xe3\x81\x82\xe3\x81\x92\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てあげた→て
    {"\xe3\x81\xa7\xe3\x81\x82\xe3\x81\x92\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // であげた→で
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てくれる→て
    {"\xe3\x81\xa7\xe3\x81\x8f\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でくれる→で
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てくれた→て
    {"\xe3\x81\xa7\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でくれた→で
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てくれない→て
    {"\xe3\x81\xa7\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でくれない→で
    {"\xe3\x81\xa6\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てくれなかった→て
    {"\xe3\x81\xa7\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でくれなかった→で
    {"\xe3\x81\xa6\xe3\x81\xbb\xe3\x81\x97\xe3\x81\x84", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てほしい→て
    {"\xe3\x81\xa7\xe3\x81\xbb\xe3\x81\x97\xe3\x81\x84", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でほしい→で
    {"\xe3\x81\xa6\xe3\x82\x8b", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てる→て
    {"\xe3\x81\xa7\xe3\x82\x8b", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でる→で
    {"\xe3\x81\xa6\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てた→て
    {"\xe3\x81\xa7\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でた→で
    {"\xe3\x81\xa6\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てない→て
    {"\xe3\x81\xa7\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でない→で
    {"\xe3\x81\xa6\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa6", WordCondition::DICT, WordCondition::STEM},  // てなかった→て
    {"\xe3\x81\xa7\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\xa7", WordCondition::DICT, WordCondition::STEM},  // でなかった→で
    // Adjective + なる: back to the adverbial く, which く→い finishes (小さくなった, 見えなくなった).
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x82\x8b", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::STEM},  // くなる→く
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::STEM},  // くなった→く
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::STEM},  // くなって→く
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::STEM},  // くなります→く
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x82\x8a\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::STEM},  // くなりました→く
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x82\x89\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::STEM},  // くならない→く
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x82\x89\xe3\x81\xaa\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::STEM},  // くならなかった→く

    // ── I-adjective (adj-i): dictionary form ends in -い ─────────
    {"\xe3\x81\x8f\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // くない→い (negative)
    {"\xe3\x81\x8b\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // かった→い (past)
    {"\xe3\x81\x8f\xe3\x81\xa6", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // くて→い (te-form)
    {"\xe3\x81\x91\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // ければ→い (conditional)
    {"\xe3\x81\x8f", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // く→い (adverbial)
    {"\xe3\x81\x95", "\xe3\x81\x84", WordCondition::DICT, WordCondition::ADJ_I},  // さ→い (nominal)

    // ── Suru (する) irregular verb ───────────────────────────────
    {"\xe3\x81\x97\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // した→する
    {"\xe3\x81\x97\xe3\x81\xa6", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // して→する
    {"\xe3\x81\x97\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // しない→する
    {"\xe3\x81\x97\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // します→する
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // される→する
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // された→する
    {"\xe3\x81\x95\xe3\x82\x8c\xe3\x81\xa6", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // されて→する
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させる→する
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x81\x9f", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させた→する
    {"\xe3\x81\x95\xe3\x81\x9b\xe3\x81\xa6", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // させて→する
    {"\xe3\x81\xa7\xe3\x81\x8d\xe3\x82\x8b", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // できる→する
    {"\xe3\x81\x97\xe3\x82\x88\xe3\x81\x86", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // しよう→する
    {"\xe3\x81\x99\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x99\xe3\x82\x8b", WordCondition::DICT, WordCondition::VS},  // すれば→する

    // ── Kuru (来る/くる) irregular verb ──────────────────────────
    {"\xe3\x81\x8d\xe3\x81\x9f", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // きた→くる
    {"\xe3\x81\x8d\xe3\x81\xa6", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // きて→くる
    {"\xe3\x81\x93\xe3\x81\xaa\xe3\x81\x84", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // こない→くる
    {"\xe3\x81\x8d\xe3\x81\xbe\xe3\x81\x99", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // きます→くる
    {"\xe3\x81\x93\xe3\x82\x89\xe3\x82\x8c\xe3\x82\x8b", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // こられる→くる
    {"\xe3\x81\x93\xe3\x82\x88\xe3\x81\x86", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // こよう→くる
    {"\xe3\x81\x8f\xe3\x82\x8c\xe3\x81\xb0", "\xe3\x81\x8f\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // くれば→くる
    // The same in kanji: 来 keeps its spelling, so these do not reach 来る through the kana rows.
    {"\xe6\x9d\xa5\xe3\x81\xaa\xe3\x81\x84", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来ない→来る
    {"\xe6\x9d\xa5\xe3\x81\xbe\xe3\x81\x99", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来ます→来る
    {"\xe6\x9d\xa5\xe3\x81\xa6", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来て→来る
    {"\xe6\x9d\xa5\xe3\x81\x9f", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来た→来る
    {"\xe6\x9d\xa5\xe3\x82\x8c\xe3\x81\xb0", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来れば→来る
    {"\xe6\x9d\xa5\xe3\x82\x88\xe3\x81\x86", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来よう→来る
    {"\xe6\x9d\xa5\xe3\x82\x89\xe3\x82\x8c\xe3\x82\x8b", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来られる→来る
    {"\xe6\x9d\xa5\xe3\x81\x9a", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来ず→来る
    {"\xe6\x9d\xa5\xe3\x81\xa6\xe3\x82\x8b", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来てる→来る
    {"\xe6\x9d\xa5\xe3\x81\xa6\xe3\x81\x9f", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来てた→来る
    {"\xe6\x9d\xa5\xe3\x81\xa6\xe3\x81\x84\xe3\x82\x8b", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来ている→来る
    {"\xe6\x9d\xa5\xe3\x81\xa6\xe3\x81\x84\xe3\x81\x9f", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来ていた→来る
    {"\xe6\x9d\xa5\xe3\x81\x9d\xe3\x81\x86", "\xe6\x9d\xa5\xe3\x82\x8b", WordCondition::DICT, WordCondition::VK},  // 来そう→来る

    // ── Iku (行く) irregular te/ta forms ─────────────────────────
    {"\xe3\x81\xa3\xe3\x81\x9f", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // った→く (行った→行く, duplicates godan る but also matches く)
    {"\xe3\x81\xa3\xe3\x81\xa6", "\xe3\x81\x8f", WordCondition::DICT, WordCondition::V5},  // って→く (行って→行く)
};
// clang-format on

static constexpr size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

constexpr size_t cstrLen(const char* s) {
  size_t n = 0;
  while (s[n] != '\0') n++;
  return n;
}

constexpr bool endsWithC(const char* s, const size_t len, const char* suffix) {
  const size_t n = cstrLen(suffix);
  if (len < n) return false;
  for (size_t i = 0; i < n; i++) {
    if (s[len - n + i] != suffix[i]) return false;
  }
  return true;
}

// The class a rule's own ending conjugates as, which decides what an earlier rule's result may
// hand it. A conjugated form is itself a word of some class: 食べさせられない ends in the
// adjective-like ない, 食べさせられる in the ichidan られる. A rule whose ending is such an
// auxiliary takes a candidate of that class; any other rule (た, て, ます, ば, a stem) only takes
// the surface text or an unclassed result. Without this, unrelated rules chained into words
// that were never there: 入っていってよかった ran on through よう and てる to 入る.
// DICT: an ending nothing classed may continue into.
constexpr WordCondition formClassOf(const char* from) {
  const size_t len = cstrLen(from);
  constexpr size_t kKana = 3;                       // UTF-8 bytes per kana
  if (len < 2 * kKana) return WordCondition::DICT;  // a single kana is a stem or a final form
  constexpr const char* kAdjective[] = {"\xe3\x81\xaa\xe3\x81\x84",
                                        "\xe3\x81\x9f\xe3\x81\x84",
                                        "\xe3\x81\xbb\xe3\x81\x97\xe3\x81\x84",  // ない たい ほしい
                                        "\xe3\x82\x89\xe3\x81\x97\xe3\x81\x84",
                                        "\xe3\x81\xab\xe3\x81\x8f\xe3\x81\x84",
                                        "\xe3\x82\x84\xe3\x81\x99\xe3\x81\x84"};  // らしい にくい やすい
  for (const char* a : kAdjective) {
    if (endsWithC(from, len, a)) return WordCondition::ADJ_I;
  }
  if (endsWithC(from, len, "\xe3\x81\x99\xe3\x82\x8b")) return WordCondition::VS;  // する
  if (endsWithC(from, len, "\xe3\x81\x8f\xe3\x82\x8b")) return WordCondition::VK;  // くる
  // る: ichidan after an い- or え-row kana (られる, ている), else godan (なる, ある).
  if (endsWithC(from, len, "\xe3\x82\x8b")) {
    constexpr const char* kIchidanBefore[] = {
        "\xe3\x81\x84", "\xe3\x81\x8d", "\xe3\x81\x8e", "\xe3\x81\x97", "\xe3\x81\x98", "\xe3\x81\xa1", "\xe3\x81\xa2",
        "\xe3\x81\xab", "\xe3\x81\xb2", "\xe3\x81\xb3", "\xe3\x81\xb4", "\xe3\x81\xbf", "\xe3\x82\x8a", "\xe3\x81\x88",
        "\xe3\x81\x91", "\xe3\x81\x92", "\xe3\x81\x9b", "\xe3\x81\x9c", "\xe3\x81\xa6", "\xe3\x81\xa7", "\xe3\x81\xad",
        "\xe3\x81\xb8", "\xe3\x81\xb9", "\xe3\x81\xba", "\xe3\x82\x81", "\xe3\x82\x8c"};
    for (const char* k : kIchidanBefore) {
      if (endsWithC(from, len - kKana, k)) return WordCondition::V1;
    }
    return WordCondition::V5;
  }
  constexpr const char* kGodanFinal[] = {"\xe3\x81\x86", "\xe3\x81\x8f", "\xe3\x81\x90", "\xe3\x81\x99", "\xe3\x81\xa4",
                                         "\xe3\x81\xac", "\xe3\x81\xb6", "\xe3\x82\x80"};  // うくぐすつぬぶむ
  for (const char* g : kGodanFinal) {
    if (endsWithC(from, len, g)) return WordCondition::V5;
  }
  return WordCondition::DICT;
}

struct FormClasses {
  WordCondition of[kRuleCount];
};

constexpr FormClasses buildFormClasses() {
  FormClasses t{};
  for (size_t r = 0; r < kRuleCount; r++) t.of[r] = formClassOf(kRules[r].from);
  return t;
}

// Each rule's form class, worked out at compile time (flash, no RAM, no first-call setup).
constexpr FormClasses kFormClassTable = buildFormClasses();
constexpr const WordCondition* kFormClass = kFormClassTable.of;

bool endsWith(const std::string& str, const char* suffix, size_t suffixLen) {
  if (str.size() < suffixLen) return false;
  return std::memcmp(str.data() + str.size() - suffixLen, suffix, suffixLen) == 0;
}

}  // namespace

std::vector<DeinflectionCandidate> Deinflector::deinflect(const std::string& surface) {
  std::vector<DeinflectionCandidate> results;
  if (surface.empty()) return results;

  // The surface form itself is always a candidate (it may be a dictionary form).
  results.push_back({surface, WordCondition::DICT});

  // Apply rules iteratively — each new candidate can be further deinflected
  // (enabling rule chaining like causative-passive-past). Limit depth to
  // prevent runaway on pathological input.
  constexpr size_t kMaxCandidates = 64;

  for (size_t i = 0; i < results.size() && results.size() < kMaxCandidates; i++) {
    // Copy by value — push_back below can reallocate the vector,
    // invalidating any reference into results[].
    const std::string text = results[i].text;
    const WordCondition cond = results[i].condition;

    for (size_t r = 0; r < kRuleCount; r++) {
      const Rule& rule = kRules[r];

      // Condition check: DICT matches any input; otherwise must match exactly.
      if (rule.condIn != WordCondition::DICT && rule.condIn != cond) continue;
      // A classed result only continues into a rule whose ending conjugates as that class.
      if (cond != WordCondition::DICT && cond != WordCondition::STEM && kFormClass[r] != cond) continue;

      const size_t fromLen = std::strlen(rule.from);
      if (!endsWith(text, rule.from, fromLen)) continue;

      if (text.size() < fromLen) continue;
      // A bare stem needs a word before its て/で/く: できた is できる, not で + きた (でる).
      if (rule.condOut == WordCondition::STEM && text.size() <= fromLen) continue;

      std::string newText = text.substr(0, text.size() - fromLen) + rule.to;

      // Avoid duplicates. The class counts: the same text from two rules (付ける as a godan
      // passive and as an ichidan verb) is checked against each class's entries.
      bool found = false;
      for (const auto& existing : results) {
        if (existing.text == newText && existing.condition == rule.condOut) {
          found = true;
          break;
        }
      }
      if (!found) {
        results.push_back({std::move(newText), rule.condOut});
      }
    }
  }

  return results;
}
