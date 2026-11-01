// Conjugations that need their own deinflection rules: kanji 来, ございました, ず, てる, そう.

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Deinflector.h"
#include "HalStorage.h"
#include "WordLookup.h"

namespace {

struct Rec {
  const char* headword;
  uint8_t priority;
  uint8_t posFlags;
  const char* definition;
};

void writeU32(std::string& out, uint32_t v) {
  for (int b = 0; b < 4; b++) out.push_back(static_cast<char>((v >> (8 * b)) & 0xFF));
}

// Records keep their given order within a headword, as the converter writes them.
void buildDictionary(const std::filesystem::path& dir, std::vector<Rec> recs) {
  std::stable_sort(recs.begin(), recs.end(),
                   [](const Rec& a, const Rec& b) { return std::strcmp(a.headword, b.headword) < 0; });
  std::string idx, dat;
  for (const Rec& r : recs) {
    char hw[DictIndexRecord::HEADWORD_SIZE] = {};
    std::memcpy(hw, r.headword, std::strlen(r.headword));
    idx.append(hw, sizeof(hw));
    writeU32(idx, static_cast<uint32_t>(dat.size()));
    const size_t len = std::strlen(r.definition);
    idx.push_back(static_cast<char>(len & 0xFF));
    idx.push_back(static_cast<char>((len >> 8) & 0xFF));
    idx.push_back(static_cast<char>(r.priority));
    idx.push_back(static_cast<char>(r.posFlags));
    dat += r.definition;
  }
  std::filesystem::create_directories(dir / "dictionaries" / "jp");
  const auto write = [&dir](const char* name, const std::string& bytes) {
    std::FILE* f = std::fopen((dir / "dictionaries" / "jp" / name).string().c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), f), bytes.size());
    std::fclose(f);
  };
  write("vocab.idx", idx);
  write("vocab.dat", dat);
}

class WordLookupDeinflection : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / ("cp_word_lookup_deinflection_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    constexpr uint8_t OTHER = DictIndexRecord::POS_OTHER;
    constexpr uint8_t V5 = DictIndexRecord::POS_V5;
    constexpr uint8_t V1 = DictIndexRecord::POS_V1;
    buildDictionary(root_, {{"来", 255, OTHER, "next (noun)"},
                            {"来る", 255, DictIndexRecord::POS_VK, "to come"},
                            {"出来る", 255, V1, "to be able"},
                            {"ござる", 255, V5, "to be (archaic)"},
                            {"ございます", 255, OTHER, "to be (polite)"},
                            {"いる", 255, V1, "to be"},
                            {"います", 255, OTHER, "to be (honorific)"},
                            {"笑う", 255, V5, "to laugh"},
                            {"食べる", 255, V1, "to eat"},
                            {"読む", 255, V5, "to read"},
                            {"できる", 255, V1, "to be able"},
                            {"でく", 255, V5, "rare godan"},
                            {"高い", 255, DictIndexRecord::POS_ADJ_I, "high"},
                            {"勉強", 255, DictIndexRecord::POS_VS | OTHER, "study"},
                            {"まず", 255, OTHER, "first of all"},
                            {"必ず", 255, OTHER, "certainly"},
                            {"かわいそう", 255, OTHER, "pitiful"},
                            {"ありがとうございます", 255, OTHER, "thank you"},
                            {"分かつ", 255, V5, "【わかつ】\n• to divide"},
                            {"分かる", 255, V5, "【わかる】\n• to understand\n• to comprehend\n• to grasp"},
                            {"入る", 255, V5, "to enter"},
                            {"見える", 255, V1, "to be visible"},
                            {"小さい", 255, DictIndexRecord::POS_ADJ_I, "small"},
                            {"帰る", 255, V5, "to return"},
                            {"て", 255, OTHER, "particle"}});
    testRoot() = root_.string();
    DictIndex::releaseCaches();
  }
  void TearDown() override {
    DictIndex::releaseCaches();
    std::filesystem::remove_all(root_);
  }
  // Headword and matched surface at the start of `text`.
  static std::pair<std::string, std::string> lookup(const std::string& text) {
    WordLookupResult r;
    if (!WordLookup::lookup(text, 0, r)) return {"<none>", ""};
    return {r.entry.headword, text.substr(0, r.matchLength)};
  }
  std::filesystem::path root_;
};

std::pair<std::string, std::string> both(const char* headword, const char* surface) { return {headword, surface}; }

TEST_F(WordLookupDeinflection, KanjiKuruForms) {
  EXPECT_EQ(lookup("来なかった。"), both("来る", "来なかった"));
  EXPECT_EQ(lookup("来ない。"), both("来る", "来ない"));
  EXPECT_EQ(lookup("来ます。"), both("来る", "来ます"));
  EXPECT_EQ(lookup("出来ない。"), both("出来る", "出来ない"));
  EXPECT_EQ(lookup("来てる。"), both("来る", "来てる"));
  EXPECT_EQ(lookup("来てた。"), both("来る", "来てた"));
  EXPECT_EQ(lookup("来そう。"), both("来る", "来そう"));
}

TEST_F(WordLookupDeinflection, GozaimashitaIsGozaimasuButOtherPolitePastsAreTheVerb) {
  EXPECT_EQ(lookup("ございました。"), both("ございます", "ございました"));
  EXPECT_EQ(lookup("いました。"), both("いる", "いました"));
}

TEST_F(WordLookupDeinflection, ClassicalNegativeZu) {
  EXPECT_EQ(lookup("笑わず、"), both("笑う", "笑わず"));
  EXPECT_EQ(lookup("食べず、"), both("食べる", "食べず"));
}

TEST_F(WordLookupDeinflection, ColloquialTeru) {
  EXPECT_EQ(lookup("食べてる。"), both("食べる", "食べてる"));
  EXPECT_EQ(lookup("読んでる。"), both("読む", "読んでる"));
  EXPECT_EQ(lookup("食べてた。"), both("食べる", "食べてた"));
}

TEST_F(WordLookupDeinflection, AppearanceSou) {
  EXPECT_EQ(lookup("食べそう。"), both("食べる", "食べそう"));
  EXPECT_EQ(lookup("できそう。"), both("できる", "できそう"));
  EXPECT_EQ(lookup("高そう。"), both("高い", "高そう"));
}

TEST_F(WordLookupDeinflection, SameTextFromTwoRulesKeepsBothClasses) {
  const auto c = Deinflector::deinflect("来ない");
  const auto has = [&c](WordCondition w) {
    return std::any_of(c.begin(), c.end(),
                       [w](const DeinflectionCandidate& d) { return d.text == "来る" && d.condition == w; });
  };
  EXPECT_TRUE(has(WordCondition::V1));
  EXPECT_TRUE(has(WordCondition::VK));
}

TEST_F(WordLookupDeinflection, WordsEndingLikeASuffixStayThemselves) {
  EXPECT_EQ(lookup("まず、"), both("まず", "まず"));
  EXPECT_EQ(lookup("必ず来る"), both("必ず", "必ず"));
  EXPECT_EQ(lookup("かわいそうな"), both("かわいそう", "かわいそう"));
}

TEST_F(WordLookupDeinflection, ConjugationPastTheWindowWhenTheTextRunsOnInHiragana) {
  EXPECT_EQ(lookup("食べさせられなかった。"), both("食べる", "食べさせられなかった"));
  EXPECT_EQ(lookup("ありがとうございます。"), both("ありがとうございます", "ありがとうございます"));
}

TEST_F(WordLookupDeinflection, WindowStaysAtEightWithoutAHiraganaTail) {
  // The text runs on, but in kanji: the normal window decides.
  EXPECT_EQ(lookup("食べさせられ勉強勉強").second, "食べさせられ");
}

TEST_F(WordLookupDeinflection, LongSuruConjugationFindsItsNoun) {
  EXPECT_EQ(lookup("勉強させられなかった。"), both("勉強", "勉強させられなかった"));
}

TEST_F(WordLookupDeinflection, LongPassStopsAtANul) {
  std::string text = "来";
  text.push_back('\0');
  text += "ああああああああ";
  WordLookupResult r;
  if (WordLookup::lookup(text, 0, r)) {
    EXPECT_LE(r.matchLength, std::string("来").size());
  }
}

TEST_F(WordLookupDeinflection, EveryHeadwordTheTextCanBeMostCommonFirst) {
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup("分かって。", 0, r));
  EXPECT_EQ(r.entry.headword, "分かる");
  const std::string alternative = std::string(kAlternativeSeparator) + "分かつ\n【わかつ】";
  EXPECT_NE(r.entry.definition.find(alternative), std::string::npos) << r.entry.definition;
  EXPECT_LT(r.entry.definition.find("understand"), r.entry.definition.find("divide"));
}

TEST_F(WordLookupDeinflection, TheScanTakesTheFirstHitWithoutAlternatives) {
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup("分かって。", 0, r, /*needDefinition=*/false));
  EXPECT_EQ(r.entry.definition.find(kAlternativeSeparator), std::string::npos);
  EXPECT_EQ(r.matchLength, std::string("分かって").size());
}

TEST_F(WordLookupDeinflection, AuxiliariesReachTheVerbThroughTheTeForm) {
  EXPECT_EQ(lookup("帰っていった。"), both("帰る", "帰っていった"));
  EXPECT_EQ(lookup("勉強してくれた。"), both("勉強", "勉強してくれた"));
}

TEST_F(WordLookupDeinflection, AdjectivePlusNaru) {
  EXPECT_EQ(lookup("小さくなった。"), both("小さい", "小さくなった"));
  EXPECT_EQ(lookup("見えなくなった。"), both("見える", "見えなくなった"));
}

TEST_F(WordLookupDeinflection, UnrelatedRulesDoNotChain) {
  // よかった is a word of its own: no chain of rules may fold it into 入る.
  EXPECT_NE(lookup("入っていってよかった。").second, "入っていってよかった");
}

TEST_F(WordLookupDeinflection, TheBareTeFormIsNeverAWordOfItsOwn) {
  for (const auto& c : Deinflector::deinflect("帰っていった")) {
    if (c.text == "帰って") {
      EXPECT_EQ(c.condition, WordCondition::STEM);
    }
  }
}

}  // namespace
