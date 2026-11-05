// お/ご + stem + する and continuative stems before a comma resolve to the verb only where the
// text says so.

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

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

class WordLookupVerbStems : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / ("cp_word_lookup_verb_stems_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    constexpr uint8_t OTHER = DictIndexRecord::POS_OTHER;
    constexpr uint8_t V5 = DictIndexRecord::POS_V5;
    constexpr uint8_t V1 = DictIndexRecord::POS_V1;
    constexpr uint8_t VS = DictIndexRecord::POS_VS | DictIndexRecord::POS_OTHER;
    buildDictionary(root_, {{"お出し", 255, OTHER, "dashi"},
                            {"出す", 255, V5, "to take out"},
                            {"お茶", 255, OTHER, "tea"},
                            {"お願い", 255, OTHER, "【おねがい】\n\n[noun] [suru] \n• request"},
                            {"願う", 255, V5, "to wish"},
                            {"お喋り", 255, VS, "chatter"},
                            {"喋る", 255, V5, "to talk"},
                            {"通り", 255, OTHER, "as, in accordance with"},
                            {"通る", 255, V5, "to pass"},
                            {"茶", 255, OTHER, "tea plant"},
                            {"説明", 255, VS, "explanation"},
                            {"乗り", 255, OTHER, "ride"},
                            {"乗る", 255, V5, "to ride"},
                            {"祭り", 255, OTHER, "festival"},
                            {"祭る", 255, V5, "to deify"},
                            {"考え", 255, OTHER, "thought"},
                            {"考える", 255, V1, "to think"},
                            {"愛し", 255, OTHER, "beloved"},
                            {"愛す", 255, V5, "to love (godan)"},
                            {"愛する", 255, DictIndexRecord::POS_VS, "to love"},
                            {"高い", 255, DictIndexRecord::POS_ADJ_I, "high"}});
    testRoot() = root_.string();
    DictIndex::releaseCaches();
  }
  void TearDown() override {
    DictIndex::releaseCaches();
    std::filesystem::remove_all(root_);
  }
  // Looks up `text` at the byte offset of `at` (the first occurrence) and returns the headword
  // and the matched surface.
  static std::pair<std::string, std::string> lookupAt(const std::string& text, const std::string& at) {
    WordLookupResult r;
    const size_t offset = text.find(at);
    if (offset == std::string::npos || !WordLookup::lookup(text, offset, r)) return {"<none>", ""};
    return {r.entry.headword, text.substr(offset, r.matchLength)};
  }
  std::filesystem::path root_;
};

// --- お/ご + stem + する

TEST_F(WordLookupVerbStems, HumbleOStemSuruIsTheVerb) {
  EXPECT_EQ(lookupAt("お出しして", "お出し"), std::make_pair(std::string("出す"), std::string("お出しして")));
  EXPECT_EQ(lookupAt("お出しします", "お出し").first, "出す");
  EXPECT_EQ(lookupAt("お出しした", "お出し").first, "出す");
}

TEST_F(WordLookupVerbStems, ARealONounWithSuruStaysTheNoun) {
  EXPECT_EQ(lookupAt("お茶しよう", "お茶").first, "お茶");
  EXPECT_EQ(lookupAt("お茶する", "お茶").first, "お茶");
}

TEST_F(WordLookupVerbStems, AnONounThatIsASuruNounStaysTheNoun) {
  EXPECT_EQ(lookupAt("お願いします", "お願い").first, "お願い");
  EXPECT_EQ(lookupAt("お喋りします", "お喋り").first, "お喋り");  // POS_VS, no [suru] tag
}

TEST_F(WordLookupVerbStems, HonorificGoSuruNounIsTheNoun) {
  EXPECT_EQ(lookupAt("ご説明します", "ご説明"), std::make_pair(std::string("説明"), std::string("ご説明します")));
}

TEST_F(WordLookupVerbStems, ONounWithoutSuruStaysTheNoun) {
  EXPECT_EQ(lookupAt("お出しを取る", "お出し").first, "お出し");
}

// --- continuative stem before a comma

TEST_F(WordLookupVerbStems, StemAfterAnArgumentBeforeACommaIsTheVerb) {
  EXPECT_EQ(lookupAt("バスに乗り、駅へ", "乗り").first, "乗る");
  EXPECT_EQ(lookupAt("馬を乗り、", "乗り").first, "乗る");
  EXPECT_EQ(lookupAt("馬で乗り，", "乗り").first, "乗る");
}

TEST_F(WordLookupVerbStems, PrecedingCharacterCanBePassedSeparately) {
  // The lookup screen hands over text starting at the word; the particle before it comes apart.
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup("乗り、駅へ", 0, r, true, U'に'));
  EXPECT_EQ(r.entry.headword, "乗る");
  ASSERT_TRUE(WordLookup::lookup("乗り、駅へ", 0, r, true, U'の'));
  EXPECT_EQ(r.entry.headword, "乗り");
  ASSERT_TRUE(WordLookup::lookup("乗り、駅へ", 0, r, true, 0));
  EXPECT_EQ(r.entry.headword, "乗り");
}

TEST_F(WordLookupVerbStems, SingleKanjiSuruVerbBeatsItsGodanVariant) {
  EXPECT_EQ(lookupAt("島を愛し、", "愛し").first, "愛する");
}

TEST_F(WordLookupVerbStems, NounInAListStaysTheNoun) {
  EXPECT_EQ(lookupAt("夏の祭り、花火", "祭り").first, "祭り");
  EXPECT_EQ(lookupAt("祭り、花火", "祭り").first, "祭り");
  EXPECT_EQ(lookupAt("花火、祭り、", "祭り").first, "祭り");
}

TEST_F(WordLookupVerbStems, DooriAsSuffixStaysTheNoun) {
  EXPECT_EQ(lookupAt("今まで通り、", "通り").first, "通り");
  EXPECT_EQ(lookupAt("いつも通り、", "通り").first, "通り");
}

TEST_F(WordLookupVerbStems, WithoutACommaTheNounStays) {
  EXPECT_EQ(lookupAt("バスに乗りが", "乗り").first, "乗り");
  EXPECT_EQ(lookupAt("バスに乗り。", "乗り").first, "乗り");
}

TEST_F(WordLookupVerbStems, IchidanStemsAreLeftAlone) {
  // 考え does not end in an i-row kana: a noun (thought) far more often than a clause link.
  EXPECT_EQ(lookupAt("それを考え、", "考え").first, "考え");
}

TEST_F(WordLookupVerbStems, AdjectivesAreNotStems) { EXPECT_EQ(lookupAt("山が高い、", "高い").first, "高い"); }

}  // namespace
