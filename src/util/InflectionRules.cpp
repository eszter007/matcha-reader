#include "InflectionRules.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace InflectionRules {
namespace {

constexpr uint8_t constLen(const char* s) {
  uint8_t n = 0;
  while (s[n] != '\0') n++;
  return n;
}

// One suffix rewrite: drop `from` off the end of the surface form and append
// `to`. `fromLen` is filled at compile time so the probe loop never calls
// strlen; the whole table lives in flash as constexpr .rodata.
struct Rule {
  const char* from;
  const char* to;
  uint8_t fromLen;
  constexpr Rule(const char* f, const char* t) : from(f), to(t), fromLen(constLen(f)) {}
};

// French suffix rewrites, grouped by what they undo. Rules are applied
// longest-suffix-first regardless of the order here (see addRuleVariants), so
// entries are grouped for reading, not for precedence.
//
// Suffixes are matched over UTF-8 bytes. That is safe without decoding: a
// continuation byte is 0x80-0xBF, so neither an ASCII letter nor a lead byte
// (0xC3, 0xC5) can ever be the tail of a longer sequence, and every match
// therefore lands on a codepoint boundary.
//
// Coverage is the three regular conjugations, the productive stem-alternating
// families built on them (-eindre/-aindre/-oindre, -aître, -uire), adjective
// -> adverb (-ment), and productive noun/adjective morphology. Suppletive
// verbs (être, avoir, aller, faire, pouvoir, vouloir, savoir, prendre, voir,
// dire) are not rule-reachable — "est", "ont", "fut", "vais", "peut" share no
// stem with their lemma — and belong in a .syn, same as the irregular passé
// simple of -aître verbs ("connus", "naquit").
// clang-format off
constexpr Rule kFrenchRules[] = {
    // ── Noun and adjective plurals ──────────────────────────────────
    {"aux", "al"},      // journaux → journal
    {"x", ""},          // bijoux → bijou, cheveux → cheveu
    {"s", ""},          // livres → livre

    // ── Feminine forms ──────────────────────────────────────────────
    {"e", ""},          // grande → grand
    {"es", ""},         // grandes → grand
    {"elle", "eau"},    // nouvelle → nouveau
    {"elles", "eau"},
    {"euse", "eux"},    // heureuse → heureux
    {"euses", "eux"},
    {"euse", "eur"},    // chanteuse → chanteur
    {"euses", "eur"},
    {"trice", "teur"},  // actrice → acteur
    {"trices", "teur"},
    {"ère", "er"},      // première → premier, boulangère → boulanger
    {"ères", "er"},
    {"ve", "f"},        // neuve → neuf
    {"ves", "f"},
    {"lle", "l"},       // cruelle → cruel
    {"lles", "l"},
    {"nne", "n"},       // bonne → bon, parisienne → parisien
    {"nnes", "n"},
    {"sse", "s"},       // grosse → gros
    {"sses", "s"},
    {"tte", "t"},       // nette → net, muette → muet
    {"ttes", "t"},
    {"che", "c"},       // blanche → blanc
    {"ches", "c"},
    {"gue", "g"},       // longue → long
    {"gues", "g"},

    // ── First conjugation (-er): present, subjunctive, imperative ───
    {"e", "er"},        // parle → parler
    {"es", "er"},
    {"ons", "er"},
    {"ez", "er"},
    {"ent", "er"},      // parlent → parler
    // Imperfect and present subjunctive
    {"ais", "er"},
    {"ait", "er"},
    {"aient", "er"},    // parlaient → parler
    {"ions", "er"},
    {"iez", "er"},
    // Future and conditional
    {"erai", "er"},
    {"eras", "er"},
    {"era", "er"},
    {"erons", "er"},
    {"erez", "er"},
    {"eront", "er"},
    {"erais", "er"},
    {"erait", "er"},
    {"erions", "er"},
    {"eriez", "er"},
    {"eraient", "er"},
    // Passé simple and imperfect subjunctive
    {"ai", "er"},
    {"as", "er"},
    {"a", "er"},
    {"âmes", "er"},
    {"âtes", "er"},
    {"èrent", "er"},    // parlèrent → parler
    {"ât", "er"},
    {"asse", "er"},
    {"asses", "er"},
    {"assent", "er"},
    {"assions", "er"},
    {"assiez", "er"},
    // Participles
    {"é", "er"},        // parlé → parler
    {"ée", "er"},
    {"és", "er"},
    {"ées", "er"},
    {"ant", "er"},      // parlant → parler

    // ── First conjugation, stem-changing ────────────────────────────
    // -ger and -cer keep the soft consonant before a/o (mangeons, plaçons).
    {"geons", "ger"},
    {"geais", "ger"},
    {"geait", "ger"},
    {"geaient", "ger"},
    {"geant", "ger"},
    {"gea", "ger"},
    {"geâmes", "ger"},
    {"geâtes", "ger"},
    {"çons", "cer"},
    {"çais", "cer"},
    {"çait", "cer"},
    {"çaient", "cer"},
    {"çant", "cer"},
    {"ça", "cer"},
    // Doubled consonant or grave accent before a mute ending.
    {"elle", "eler"},   // appelle → appeler
    {"elles", "eler"},
    {"ellent", "eler"},
    {"ette", "eter"},   // jette → jeter
    {"ettes", "eter"},
    {"ettent", "eter"},
    {"ète", "eter"},    // achète → acheter
    {"ètes", "eter"},
    {"ètent", "eter"},
    {"ète", "éter"},    // complète → compléter
    {"ètes", "éter"},
    {"ètent", "éter"},
    {"ère", "érer"},    // espère → espérer
    {"ères", "érer"},
    {"èrent", "érer"},
    {"ève", "ever"},    // lève → lever
    {"èves", "ever"},
    {"èvent", "ever"},
    {"oie", "oyer"},    // envoie → envoyer
    {"oies", "oyer"},
    {"oient", "oyer"},

    // ── Second conjugation (-ir, finir type) ────────────────────────
    {"is", "ir"},       // finis → finir
    {"it", "ir"},
    {"issons", "ir"},
    {"issez", "ir"},
    {"issent", "ir"},
    {"issais", "ir"},
    {"issait", "ir"},
    {"issaient", "ir"},
    {"issions", "ir"},
    {"issiez", "ir"},
    {"issant", "ir"},
    {"irai", "ir"},
    {"iras", "ir"},
    {"ira", "ir"},
    {"irons", "ir"},
    {"irez", "ir"},
    {"iront", "ir"},
    {"irais", "ir"},
    {"irait", "ir"},
    {"irions", "ir"},
    {"iriez", "ir"},
    {"iraient", "ir"},
    {"irent", "ir"},
    {"îmes", "ir"},
    {"îtes", "ir"},
    {"i", "ir"},        // fini → finir
    {"ie", "ir"},
    {"ies", "ir"},

    // ── Third conjugation (-re, vendre type) ────────────────────────
    {"ds", "dre"},      // vends → vendre
    {"d", "dre"},       // vend → vendre
    {"ons", "re"},
    {"ez", "re"},
    {"ent", "re"},      // vendent → vendre
    {"ais", "re"},
    {"ait", "re"},
    {"aient", "re"},
    {"ions", "re"},
    {"iez", "re"},
    {"rai", "re"},
    {"ras", "re"},
    {"ra", "re"},
    {"rons", "re"},
    {"rez", "re"},
    {"ront", "re"},
    {"rais", "re"},
    {"rait", "re"},
    {"raient", "re"},
    {"irent", "re"},
    {"îmes", "re"},
    {"îtes", "re"},
    {"u", "re"},        // vendu → vendre
    {"ue", "re"},
    {"us", "re"},
    {"ues", "re"},
    {"ant", "re"},      // vendant → vendre

    // ── Third conjugation, stem-changing (-eindre/-aindre/-oindre) ──
    // craindre, peindre, joindre, éteindre, atteindre, plaindre and their
    // compounds swap the infinitive's "eind/aind/oind" for "eign/aign/oign"
    // everywhere except the singular present, the past participle, and the
    // future/conditional (whose stem is the plain infinitive minus "e", so
    // the generic "-re" rules above already resolve them: éteindra → éteindre).
    {"eins", "eindre"},      // peins → peindre
    {"eint", "eindre"},      // peint → peindre (present il, and the participle)
    {"eignons", "eindre"},
    {"eignez", "eindre"},
    {"eignent", "eindre"},   // peignent → peindre
    {"eignais", "eindre"},
    {"eignait", "eindre"},
    {"eignions", "eindre"},
    {"eigniez", "eindre"},
    {"eignaient", "eindre"},
    {"eignis", "eindre"},
    {"eignit", "eindre"},    // éteignit → éteindre
    {"eignîmes", "eindre"},
    {"eignîtes", "eindre"},
    {"eignirent", "eindre"},
    {"eigne", "eindre"},
    {"eignes", "eindre"},
    {"eignant", "eindre"},

    {"ains", "aindre"},      // crains → craindre
    {"aint", "aindre"},
    {"aignons", "aindre"},
    {"aignez", "aindre"},
    {"aignent", "aindre"},
    {"aignais", "aindre"},
    {"aignait", "aindre"},
    {"aignions", "aindre"},
    {"aigniez", "aindre"},
    {"aignaient", "aindre"},
    {"aignis", "aindre"},
    {"aignit", "aindre"},
    {"aignîmes", "aindre"},
    {"aignîtes", "aindre"},
    {"aignirent", "aindre"},
    {"aigne", "aindre"},
    {"aignes", "aindre"},
    {"aignant", "aindre"},

    {"oins", "oindre"},      // joins → joindre
    {"oint", "oindre"},
    {"oignons", "oindre"},
    {"oignez", "oindre"},
    {"oignent", "oindre"},
    {"oignais", "oindre"},
    {"oignait", "oindre"},
    {"oignions", "oindre"},
    {"oigniez", "oindre"},
    {"oignaient", "oindre"},
    {"oignis", "oindre"},
    {"oignit", "oindre"},
    {"oignîmes", "oindre"},
    {"oignîtes", "oindre"},
    {"oignirent", "oindre"},
    {"oigne", "oindre"},
    {"oignes", "oindre"},
    {"oignant", "oindre"},

    // ── -aître verbs (connaître, paraître, naître, and compounds) ───
    // Passé simple is genuinely suppletive per verb ("connus", "naquit"
    // share no stem with "connaître"/"naître") and belongs in a .syn like
    // the verbs in the file header comment, not a rule here. Present
    // singular ("connais"/"parais") is deliberately not covered either:
    // "ais" is a 3-byte suffix that the generic {"ais","er"} rule (First
    // conjugation, above) already claims, and addRuleVariants() emits
    // same-length candidates in table order, so a later "ais"->"aître"
    // entry here would always lose to it for a common collision like
    // "parais" -> wrongly "parer" instead of "paraître".
    {"aît", "aître"},        // connaît → connaître
    {"aissons", "aître"},
    {"aissez", "aître"},
    {"aissent", "aître"},
    {"aissais", "aître"},
    {"aissait", "aître"},
    {"aissions", "aître"},
    {"aissiez", "aître"},
    {"aissaient", "aître"},
    {"aissant", "aître"},

    // ── -uire verbs (conduire, construire, cuire, produire, traduire) ─
    // Future/conditional already resolve through the generic "-re" rules
    // above, same reasoning as -eindre/-aindre/-oindre.
    // The bare "uit" suffix is deliberately not covered: it is too broad and
    // wrongly claims common non-verb words like "nuit" (night) and "bruit"
    // (noise), whose "wrong" reading ("nuire", "bruire") also happens to be a
    // real, rarer verb — the same table-order collision class as the -aître
    // "ais" case above. Narrowed to the "-duire"/"-truire" subfamilies plus
    // "cuire" itself, which still covers every verb this PR targets.
    {"uis", "uire"},         // conduis → conduire
    {"duit", "duire"},       // conduit → conduire (present il, and the participle)
    {"truit", "truire"},     // construit → construire
    {"cuit", "cuire"},       // cuit → cuire
    {"uisons", "uire"},
    {"uisez", "uire"},
    {"uisent", "uire"},
    {"uisais", "uire"},
    {"uisait", "uire"},
    {"uisions", "uire"},
    {"uisiez", "uire"},
    {"uisaient", "uire"},
    {"uisis", "uire"},
    {"uisit", "uire"},       // conduisit → conduire
    {"uisîmes", "uire"},
    {"uisîtes", "uire"},
    {"uisirent", "uire"},
    {"uisant", "uire"},

    // ── Adjective → adverb (-ment) ───────────────────────────────────
    // Only the consonant-stem pattern ("lent" -> feminine "lente" -> "lentement").
    // The vowel-stem pattern ("rapide" -> "rapidement") would need a bare
    // "ment" suffix, which at the same probe length shadows the third-person
    // plural of every -mer verb ("ils aiment" -> wrongly "ai", not "aimer").
    {"ement", ""},           // lentement → lent
};
// clang-format on

// Longest `from` in kFrenchRules, in bytes ("aissaient", "eignaient" and
// siblings, "eignîmes" and siblings, at 9). The probe loop counts down from
// here, so it only has to be an upper bound; a rule longer than this would
// simply never fire.
constexpr uint8_t FRENCH_MAX_SUFFIX = 9;

// Clitics that elide before a vowel. The layout engine treats the apostrophe as
// a word character (ParsedText.cpp isWordCharacter), so the page token for
// "l'eau" is one word and the lookup key keeps the clitic.
constexpr const char* kFrenchElisions[] = {"l", "d",  "j",     "n",      "m",      "t",     "s",
                                           "c", "qu", "jusqu", "lorsqu", "puisqu", "quoiqu"};

bool endsWith(const std::string& word, const char* suffix, size_t suffixLen) {
  // A strictly longer word, so a rewrite never produces an empty candidate.
  return word.size() > suffixLen && word.compare(word.size() - suffixLen, suffixLen, suffix) == 0;
}

// Lowercase the accented capitals French uses. Dictionary::cleanWord() folds
// ASCII only, so a sentence-initial "École" or "Être" keeps its capital and can
// never match a lowercase headword under the index's ASCII-only comparator.
// Folding a copy and probing it as a candidate — rather than folding the key in
// place — keeps a dictionary whose headword really is capitalised reachable by
// the exact match that runs first.
// False when nothing changed.
bool foldAccentedCase(const std::string& in, std::string& out) {
  out = in;
  bool changed = false;
  for (size_t i = 0; i + 1 < out.size(); i++) {
    const auto lead = static_cast<unsigned char>(out[i]);
    const auto next = static_cast<unsigned char>(out[i + 1]);
    // À-Þ (C3 80-C3 9E) lowercase by +0x20, skipping × (C3 97), which is maths.
    if (lead == 0xC3 && next >= 0x80 && next <= 0x9E && next != 0x97) {
      out[i + 1] = static_cast<char>(next + 0x20);
      changed = true;
    } else if (lead == 0xC5 && next == 0x92) {  // Œ → œ
      out[i + 1] = static_cast<char>(0x93);
      changed = true;
    } else if (lead == 0xC5 && next == 0xB8) {  // Ÿ → ÿ (C3 BF, not C5 B9)
      out[i] = static_cast<char>(0xC3);
      out[i + 1] = static_cast<char>(0xBF);
      changed = true;
    }
  }
  return changed;
}

// Strip a leading elided clitic: "l'eau" → "eau", "qu'il" → "il". False when
// there is no apostrophe, the prefix is not a clitic ("aujourd'hui"), or
// nothing would be left.
bool stripElision(const std::string& word, std::string& out) {
  size_t apostrophe = std::string::npos;
  size_t afterApostrophe = 0;
  for (size_t i = 0; i < word.size(); i++) {
    if (word[i] == '\'') {
      apostrophe = i;
      afterApostrophe = i + 1;
      break;
    }
    // U+2019 right single quotation mark, the typographic apostrophe EPUBs use.
    if (i + 2 < word.size() && static_cast<unsigned char>(word[i]) == 0xE2 &&
        static_cast<unsigned char>(word[i + 1]) == 0x80 && static_cast<unsigned char>(word[i + 2]) == 0x99) {
      apostrophe = i;
      afterApostrophe = i + 3;
      break;
    }
  }
  if (apostrophe == std::string::npos || apostrophe == 0 || afterApostrophe >= word.size()) return false;

  for (const char* clitic : kFrenchElisions) {
    const size_t len = strlen(clitic);
    if (apostrophe == len && word.compare(0, len, clitic) == 0) {
      out = word.substr(afterApostrophe);
      return true;
    }
  }
  return false;
}

// Append `candidate` unless it is the surface form, a duplicate, or the cap is
// already reached.
void addVariant(const std::string& surface, std::string candidate, std::vector<std::string>& out) {
  if (out.size() >= MAX_VARIANTS || candidate.empty() || candidate == surface) return;
  if (std::find(out.begin(), out.end(), candidate) != out.end()) return;
  out.push_back(std::move(candidate));
}

// Today's English set, kept for Generic so no non-French dictionary changes
// behaviour: possessives, plurals and verb endings, including the two
// doubled-consonant cases a plain suffix table cannot express.
void addEnglishVariants(const std::string& surface, const std::string& word, std::vector<std::string>& out) {
  const size_t n = word.size();
  const auto add = [&](std::string v) { addVariant(surface, std::move(v), out); };
  const auto ends = [&word, n](const char* suffix) {
    const size_t len = strlen(suffix);
    return n > len && word.compare(n - len, len, suffix) == 0;
  };

  if (ends("'s")) add(word.substr(0, n - 2));
  if (ends("\xE2\x80\x99s")) add(word.substr(0, n - 4));  // U+2019 apostrophe
  if (ends("ies")) add(word.substr(0, n - 3) + "y");      // stories -> story
  if (ends("es")) add(word.substr(0, n - 2));             // boxes -> box
  if (ends("s")) add(word.substr(0, n - 1));              // dogs -> dog
  if (ends("ed")) {
    add(word.substr(0, n - 2));                                            // walked -> walk
    add(word.substr(0, n - 1));                                            // loved -> love
    if (n >= 4 && word[n - 3] == word[n - 4]) add(word.substr(0, n - 3));  // stopped -> stop
  }
  if (ends("ing")) {
    add(word.substr(0, n - 3));                                            // walking -> walk
    add(word.substr(0, n - 3) + "e");                                      // making -> make
    if (n >= 5 && word[n - 4] == word[n - 5]) add(word.substr(0, n - 4));  // running -> run
  }
}

// Apply a rule table longest-suffix-first: a longer match is the more specific
// analysis, and the caller stops at the first candidate the index actually
// contains, so ordering decides which lemma wins for an ambiguous form
// ("belles" → "beau" before "bel"). The outer loop costs at most
// FRENCH_MAX_SUFFIX passes over a flash-resident table — nothing next to the
// SD probe each surviving candidate turns into.
void addRuleVariants(const Rule* rules, size_t ruleCount, uint8_t maxSuffix, const std::string& surface,
                     const std::string& word, std::vector<std::string>& out) {
  for (int len = maxSuffix; len >= 1; len--) {
    for (size_t i = 0; i < ruleCount && out.size() < MAX_VARIANTS; i++) {
      const Rule& rule = rules[i];
      if (rule.fromLen != len || !endsWith(word, rule.from, rule.fromLen)) continue;
      addVariant(surface, word.substr(0, word.size() - rule.fromLen) + rule.to, out);
    }
  }
}

}  // namespace

Language languageFromCode(const char* code) {
  if (!code || code[0] == '\0' || code[1] == '\0') return Language::Generic;
  // Compare the primary subtag only: "fr", "fr-FR" and "fr_CA" are one language.
  const char first = static_cast<char>(std::tolower(static_cast<unsigned char>(code[0])));
  const char second = static_cast<char>(std::tolower(static_cast<unsigned char>(code[1])));
  const char third = code[2];
  if (third != '\0' && third != '-' && third != '_' && third != '/') return Language::Generic;
  if (first == 'f' && second == 'r') return Language::French;
  return Language::Generic;
}

void variants(const Language language, const std::string& word, std::vector<std::string>& out) {
  out.clear();
  if (word.empty()) return;
  out.reserve(MAX_VARIANTS);

  // Each normalisation both emits a candidate and becomes the base the next
  // step works from, so "L'aimèrent" reaches "aimer" in one pass:
  // fold → "l'aimèrent", elide → "aimèrent", rewrite → "aimer".
  std::string base = word;
  std::string folded;
  if (foldAccentedCase(base, folded)) {
    addVariant(word, folded, out);
    base = folded;
  }
  if (language == Language::French) {
    std::string elided;
    if (stripElision(base, elided)) {
      addVariant(word, elided, out);
      base = elided;
    }
  }

  switch (language) {
    case Language::French:
      addRuleVariants(kFrenchRules, sizeof(kFrenchRules) / sizeof(kFrenchRules[0]), FRENCH_MAX_SUFFIX, word, base, out);
      break;
    case Language::Generic:
    default:
      addEnglishVariants(word, base, out);
      break;
  }
}

}  // namespace InflectionRules
