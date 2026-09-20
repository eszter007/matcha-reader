#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "InflectionRules.h"

namespace {

using InflectionRules::Language;

std::vector<std::string> french(const std::string& word) {
  std::vector<std::string> out;
  InflectionRules::variants(Language::French, word, out);
  return out;
}

std::vector<std::string> generic(const std::string& word) {
  std::vector<std::string> out;
  InflectionRules::variants(Language::Generic, word, out);
  return out;
}

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

// Index of a candidate, or -1. Order is probe order: the lookup stops at the
// first candidate the dictionary actually contains, so relative position
// decides which lemma wins for an ambiguous surface form.
int indexOf(const std::vector<std::string>& haystack, const std::string& needle) {
  const auto it = std::find(haystack.begin(), haystack.end(), needle);
  return it == haystack.end() ? -1 : static_cast<int>(it - haystack.begin());
}

}  // namespace

TEST(LanguageFromCode, RecognisesFrenchTagsAndFolders) {
  EXPECT_EQ(InflectionRules::languageFromCode("fr"), Language::French);
  EXPECT_EQ(InflectionRules::languageFromCode("fr-FR"), Language::French);
  EXPECT_EQ(InflectionRules::languageFromCode("FR_ca"), Language::French);
  EXPECT_EQ(InflectionRules::languageFromCode("fr/wiktionnaire"), Language::French);
}

TEST(LanguageFromCode, RejectsEverythingElse) {
  EXPECT_EQ(InflectionRules::languageFromCode("en"), Language::Generic);
  EXPECT_EQ(InflectionRules::languageFromCode("fra"), Language::Generic);  // not a primary subtag
  EXPECT_EQ(InflectionRules::languageFromCode("french"), Language::Generic);
  EXPECT_EQ(InflectionRules::languageFromCode("f"), Language::Generic);
  EXPECT_EQ(InflectionRules::languageFromCode(""), Language::Generic);
  EXPECT_EQ(InflectionRules::languageFromCode(nullptr), Language::Generic);
}

TEST(French, RegularErVerbs) {
  EXPECT_TRUE(contains(french("parle"), "parler"));
  EXPECT_TRUE(contains(french("parlons"), "parler"));
  EXPECT_TRUE(contains(french("parlez"), "parler"));
  EXPECT_TRUE(contains(french("parlent"), "parler"));
  EXPECT_TRUE(contains(french("parlait"), "parler"));
  EXPECT_TRUE(contains(french("parlaient"), "parler"));
  EXPECT_TRUE(contains(french("parlerait"), "parler"));
  EXPECT_TRUE(contains(french("parleraient"), "parler"));
  EXPECT_TRUE(contains(french("parlèrent"), "parler"));
  EXPECT_TRUE(contains(french("parlé"), "parler"));
  EXPECT_TRUE(contains(french("parlées"), "parler"));
  EXPECT_TRUE(contains(french("parlant"), "parler"));
}

TEST(French, ErVerbsWithStemChanges) {
  EXPECT_TRUE(contains(french("mangeons"), "manger"));
  EXPECT_TRUE(contains(french("mangeait"), "manger"));
  EXPECT_TRUE(contains(french("mangeaient"), "manger"));
  EXPECT_TRUE(contains(french("commençons"), "commencer"));
  EXPECT_TRUE(contains(french("commençait"), "commencer"));
  EXPECT_TRUE(contains(french("appelle"), "appeler"));
  EXPECT_TRUE(contains(french("jettent"), "jeter"));
  EXPECT_TRUE(contains(french("achète"), "acheter"));
  EXPECT_TRUE(contains(french("complète"), "compléter"));
  EXPECT_TRUE(contains(french("espèrent"), "espérer"));
  EXPECT_TRUE(contains(french("lève"), "lever"));
  EXPECT_TRUE(contains(french("envoient"), "envoyer"));
}

TEST(French, SecondAndThirdConjugation) {
  EXPECT_TRUE(contains(french("finissent"), "finir"));
  EXPECT_TRUE(contains(french("finissait"), "finir"));
  EXPECT_TRUE(contains(french("finirait"), "finir"));
  EXPECT_TRUE(contains(french("fini"), "finir"));
  EXPECT_TRUE(contains(french("choisissons"), "choisir"));
  EXPECT_TRUE(contains(french("vend"), "vendre"));
  EXPECT_TRUE(contains(french("vends"), "vendre"));
  EXPECT_TRUE(contains(french("vendent"), "vendre"));
  EXPECT_TRUE(contains(french("vendu"), "vendre"));
  EXPECT_TRUE(contains(french("attendait"), "attendre"));
}

TEST(French, EindreAindreOindreStemAlternation) {
  // Passé simple: the case that motivated this rule set (s'éteindre).
  EXPECT_TRUE(contains(french("éteignit"), "éteindre"));
  // Present singular / past participle use "eins"/"eint", not "eign".
  EXPECT_TRUE(contains(french("peins"), "peindre"));
  EXPECT_TRUE(contains(french("peint"), "peindre"));
  // Present plural, imperfect and present participle use "eign"/"aign"/"oign".
  EXPECT_TRUE(contains(french("atteignent"), "atteindre"));
  EXPECT_TRUE(contains(french("craignait"), "craindre"));
  EXPECT_TRUE(contains(french("craignant"), "craindre"));
  EXPECT_TRUE(contains(french("rejoignirent"), "rejoindre"));
  // Future/conditional already resolve through the generic "-re" rules.
  EXPECT_TRUE(contains(french("éteindra"), "éteindre"));
}

TEST(French, AitreVerbs) {
  EXPECT_TRUE(contains(french("connaît"), "connaître"));
  EXPECT_TRUE(contains(french("connaissons"), "connaître"));
  EXPECT_TRUE(contains(french("paraissait"), "paraître"));
  EXPECT_TRUE(contains(french("naissant"), "naître"));
}

TEST(French, AitreDoesNotClaimTheAmbiguousPresentSingular) {
  // "ais" (present singular: "connais", "parais") is not rule-reachable: it is
  // the same 3-byte suffix the generic First-conjugation {"ais","er"} rule
  // already claims, and same-length candidates are emitted in table order, so
  // a lookup would stop at "parer" (a real, common headword) before ever
  // trying "paraître" if both were candidates. Leaving "ais" uncovered avoids
  // that wrong lemma winning.
  EXPECT_FALSE(contains(french("parais"), "paraître"));
}

TEST(French, UireVerbs) {
  EXPECT_TRUE(contains(french("conduit"), "conduire"));
  EXPECT_TRUE(contains(french("conduisons"), "conduire"));
  EXPECT_TRUE(contains(french("construisait"), "construire"));
  EXPECT_TRUE(contains(french("conduisit"), "conduire"));
  EXPECT_TRUE(contains(french("construit"), "construire"));
  // "cuit" alone has no room for a stem (the rule needs a strictly longer
  // word — see endsWith()), so it is reachable only through a compound.
  EXPECT_TRUE(contains(french("recuit"), "recuire"));
  // Future/conditional already resolve through the generic "-re" rules.
  EXPECT_TRUE(contains(french("conduira"), "conduire"));
}

TEST(French, UireDoesNotClaimCommonNonVerbWords) {
  // The bare "uit" suffix is not covered: "nuit" (night) and "bruit" (noise)
  // are common nouns whose "wrong" verb reading ("nuire", "bruire") is also
  // a real, rarer headword — the lookup would stop there before ever trying
  // the noun. Narrowed to "-duire"/"-truire"/"cuire" instead (see UireVerbs).
  EXPECT_FALSE(contains(french("nuit"), "nuire"));
  EXPECT_FALSE(contains(french("bruit"), "bruire"));
}

TEST(French, MentAdverbs) {
  EXPECT_TRUE(contains(french("lentement"), "lent"));
  EXPECT_TRUE(contains(french("grandement"), "grand"));
  // The bare "ment" pattern (rapide -> rapidement) is deliberately not
  // covered: it would shadow "ils aiment" -> "aimer" at the same probe
  // length (see the comment on the "ement" rule).
  EXPECT_TRUE(contains(french("aiment"), "aimer"));
  EXPECT_FALSE(contains(french("aiment"), "ai"));
}

TEST(French, NounAndAdjectiveMorphology) {
  EXPECT_TRUE(contains(french("livres"), "livre"));
  EXPECT_TRUE(contains(french("journaux"), "journal"));
  EXPECT_TRUE(contains(french("bijoux"), "bijou"));
  EXPECT_TRUE(contains(french("grande"), "grand"));
  EXPECT_TRUE(contains(french("grandes"), "grand"));
  EXPECT_TRUE(contains(french("nouvelle"), "nouveau"));
  EXPECT_TRUE(contains(french("heureuse"), "heureux"));
  EXPECT_TRUE(contains(french("chanteuse"), "chanteur"));
  EXPECT_TRUE(contains(french("actrice"), "acteur"));
  EXPECT_TRUE(contains(french("première"), "premier"));
  EXPECT_TRUE(contains(french("neuve"), "neuf"));
  EXPECT_TRUE(contains(french("bonnes"), "bon"));
  EXPECT_TRUE(contains(french("grosse"), "gros"));
  EXPECT_TRUE(contains(french("blanche"), "blanc"));
  EXPECT_TRUE(contains(french("longue"), "long"));
}

TEST(French, ElisionStripsTheClitic) {
  EXPECT_TRUE(contains(french("l'eau"), "eau"));
  EXPECT_TRUE(contains(french("d'abord"), "abord"));
  EXPECT_TRUE(contains(french("qu'il"), "il"));
  EXPECT_TRUE(contains(french("jusqu'ici"), "ici"));
  // U+2019, the typographic apostrophe EPUB text actually uses.
  EXPECT_TRUE(contains(french("l\xE2\x80\x99homme"), "homme"));
}

TEST(French, ElisionLeavesRealApostrophesAlone) {
  // "aujourd" is not a clitic, and the whole word is the headword.
  EXPECT_FALSE(contains(french("aujourd'hui"), "hui"));
}

TEST(French, ElidedFormStillInflects) {
  // Elision feeds the rewrite pass, so one variants() call spans both steps.
  EXPECT_TRUE(contains(french("j'aimais"), "aimer"));
  EXPECT_TRUE(contains(french("l'appelle"), "appeler"));
}

TEST(French, FoldsAccentedCapitals) {
  // cleanWord() lowercases ASCII only, so a sentence-initial capital arrives
  // unfolded and would never match a lowercase headword.
  EXPECT_TRUE(contains(french("École"), "école"));
  EXPECT_TRUE(contains(french("Être"), "être"));
  EXPECT_TRUE(contains(french("Ça"), "ça"));
  EXPECT_TRUE(contains(french("Œuvre"), "œuvre"));
}

TEST(French, FoldingFeedsTheRewritePass) {
  // The rewrite pass runs on the folded stem, so one variants() call spans both.
  EXPECT_TRUE(contains(french("Élevait"), "élever"));
  EXPECT_TRUE(contains(french("Écoutent"), "écouter"));
}

TEST(French, LongerSuffixWinsTheProbeOrder) {
  // "belles" is both a feminine plural of "beau" and, by a shorter rule, "bel".
  // The longer, more specific analysis must be probed first.
  const auto out = french("belles");
  ASSERT_TRUE(contains(out, "beau"));
  ASSERT_TRUE(contains(out, "bel"));
  EXPECT_LT(indexOf(out, "beau"), indexOf(out, "bel"));
}

TEST(French, NeverEmitsTheSurfaceFormOrDuplicates) {
  for (const char* word : {"parle", "belles", "l'eau", "vendaient", "issu", "a", "es"}) {
    const auto out = french(word);
    EXPECT_FALSE(contains(out, word)) << word;
    auto sorted = out;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::unique(sorted.begin(), sorted.end()), sorted.end()) << word;
  }
}

TEST(French, RespectsTheProbeBudget) {
  for (const char* word : {"belles", "appelles", "mangeaient", "parleraient", "finissaient"}) {
    EXPECT_LE(french(word).size(), InflectionRules::MAX_VARIANTS) << word;
  }
}

TEST(French, ShortWordsProduceNoEmptyCandidates) {
  // endsWith needs a strictly longer word, so a one-letter remainder is the
  // shortest a rule can leave.
  for (const char* word : {"", "a", "es", "le", "de"}) {
    for (const auto& candidate : french(word)) EXPECT_FALSE(candidate.empty()) << word;
  }
}

TEST(Generic, KeepsTheEnglishBehaviour) {
  EXPECT_TRUE(contains(generic("dogs"), "dog"));
  EXPECT_TRUE(contains(generic("boxes"), "box"));
  EXPECT_TRUE(contains(generic("stories"), "story"));
  EXPECT_TRUE(contains(generic("walked"), "walk"));
  EXPECT_TRUE(contains(generic("loved"), "love"));
  EXPECT_TRUE(contains(generic("stopped"), "stop"));
  EXPECT_TRUE(contains(generic("walking"), "walk"));
  EXPECT_TRUE(contains(generic("making"), "make"));
  EXPECT_TRUE(contains(generic("running"), "run"));
  EXPECT_TRUE(contains(generic("dog's"), "dog"));
}

TEST(Generic, DoesNotApplyFrenchRules) {
  EXPECT_FALSE(contains(generic("parlons"), "parler"));
  EXPECT_FALSE(contains(generic("l'eau"), "eau"));
}

TEST(Generic, StillFoldsAccentedCapitals) {
  // Language-independent: the index comparator is ASCII-only for every
  // dictionary, not just French ones.
  EXPECT_TRUE(contains(generic("Über"), "über"));
}
