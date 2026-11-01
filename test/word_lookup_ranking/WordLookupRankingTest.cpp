// Which entry word lookup shows first, and how far a polite volitional matches.

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

class WordLookupRanking : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / ("cp_word_lookup_ranking_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    constexpr uint8_t OTHER = DictIndexRecord::POS_OTHER;
    buildDictionary(root_, {{"中", 255, OTHER, "【じゅう】\n\n[suffix] \n• through"},
                            {"中", 255, OTHER, "【なか】\n\n[noun] \n• inside"},
                            {"中", 227, OTHER, "【うち】\n\n[noun] \n• within"},
                            {"日", 255, OTHER, "【にち】\n\n[noun] [abbr.] \n• Sunday"},
                            {"日", 255, OTHER, "【ひ】\n\n[noun] \n• day"},
                            {"清める", 128, DictIndexRecord::POS_V1, "【きよめる】\n\n[1-dan] \n• to purify"}});
    testRoot() = root_.string();
    DictIndex::releaseCaches();
  }
  void TearDown() override {
    DictIndex::releaseCaches();
    std::filesystem::remove_all(root_);
  }
  std::filesystem::path root_;
};

TEST_F(WordLookupRanking, AffixGoesBehindAnEqualPriorityNoun) {
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup("中から", 0, r));
  EXPECT_EQ(r.entry.definition.rfind("【なか】", 0), 0u) << r.entry.definition;
}

TEST_F(WordLookupRanking, AbbreviationGoesBehindAnEqualPriorityNoun) {
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup("日から", 0, r));
  EXPECT_EQ(r.entry.definition.rfind("【ひ】", 0), 0u) << r.entry.definition;
}

TEST_F(WordLookupRanking, HigherPriorityStillWinsOverANonAffix) {
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup("中", 0, r));
  EXPECT_LT(r.entry.definition.find("【じゅう】"), r.entry.definition.find("【うち】"));
}

TEST_F(WordLookupRanking, PoliteVolitionalMatchesTheWholeVerb) {
  const std::string text = "清めましょう";
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup(text, 0, r));
  EXPECT_EQ(r.entry.headword, "清める");
  EXPECT_EQ(r.matchLength, text.size());
}

// lookupAll() over a word longer than the old six-character window, and the prefix answer that
// lets it stop early.
class WordLookupAll : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / ("cp_word_lookup_all_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    constexpr uint8_t OTHER = DictIndexRecord::POS_OTHER;
    buildDictionary(root_, {{"中", 200, OTHER, "zhong"},
                            {"中华", 150, OTHER, "zhonghua"},
                            {"中华人民共和国", 170, OTHER, "PRC"},
                            {"人民", 190, OTHER, "renmin"},
                            {"成立", 180, OTHER, "chengli"}});
    testRoot() = root_.string();
    DictIndex::releaseCaches();
  }
  void TearDown() override {
    DictIndex::releaseCaches();
    std::filesystem::remove_all(root_);
  }
  std::filesystem::path root_;
};

TEST_F(WordLookupAll, FindsASevenCharacterWordPastPrefixMisses) {
  uint8_t priority[WordLookup::MAX_WINDOW_CHARS] = {};
  const uint8_t found = WordLookup::lookupAll("中华人民共和国成立", 0, priority);
  EXPECT_EQ(found, 0b1000011);  // 中, 中华, 中华人民共和国
  EXPECT_EQ(priority[6], 170);
}

TEST_F(WordLookupAll, AMissReportsWhetherALongerWordStartsWithIt) {
  DictEntry e;
  EXPECT_FALSE(DictIndex::lookupExact("中华人民", e, DictIndex::DICT_ALL, false));
  EXPECT_TRUE(DictIndex::longerHeadwordMayExist());
  EXPECT_FALSE(DictIndex::lookupExact("中国", e, DictIndex::DICT_ALL, false));
  EXPECT_FALSE(DictIndex::longerHeadwordMayExist());
  // Past the last headword in sort order.
  EXPECT_FALSE(DictIndex::lookupExact("龙", e, DictIndex::DICT_ALL, false));
  EXPECT_FALSE(DictIndex::longerHeadwordMayExist());
}

TEST_F(WordLookupAll, StopsAtAPrefixNoHeadwordStartsWith) {
  uint8_t priority[WordLookup::MAX_WINDOW_CHARS] = {};
  EXPECT_EQ(WordLookup::lookupAll("人民共和国", 0, priority), 0b10);
  EXPECT_FALSE(DictIndex::longerHeadwordMayExist());  // stopped at 人民共
}

}  // namespace
