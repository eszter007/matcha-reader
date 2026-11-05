// JMdict files a suru verb under its noun (嘲笑, not 嘲笑する). WordLookup must reach the noun from
// a conjugated suru verb, or a shorter window wins with a wrong reading (嘲笑さ -> 嘲笑う).

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
  uint8_t posFlags;
  const char* definition;
};

void writeU32(std::string& out, uint32_t v) {
  for (int b = 0; b < 4; b++) out.push_back(static_cast<char>((v >> (8 * b)) & 0xFF));
}

// Records must be in memcmp order of their headwords.
void buildDictionary(const std::filesystem::path& dir, std::vector<Rec> recs) {
  std::sort(recs.begin(), recs.end(),
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
    idx.push_back(static_cast<char>(128));
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

class WordLookupSuru : public ::testing::TestWithParam<uint8_t> {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
            ("cp_word_lookup_suru_" + std::to_string(::getpid()) + "_" + std::to_string(GetParam()));
    std::filesystem::remove_all(root_);
    // GetParam(): the suru noun's flags -- POS_VS from current converters, POS_OTHER from older ones.
    buildDictionary(root_, {{"嘲笑", GetParam(), "chousho"},
                            {"嘲笑う", DictIndexRecord::POS_V5, "azawarau"},
                            {"に", DictIndexRecord::POS_OTHER, "particle"},
                            {"対", DictIndexRecord::POS_OTHER, "tsui"},
                            {"対する", DictIndexRecord::POS_VS, "tai suru"}});
    testRoot() = root_.string();
    DictIndex::releaseCaches();
  }
  void TearDown() override {
    DictIndex::releaseCaches();
    std::filesystem::remove_all(root_);
  }
  std::filesystem::path root_;
};

TEST_P(WordLookupSuru, ConjugatedSuruVerbResolvesToItsNoun) {
  const std::string text = "嘲笑されている";
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup(text, 0, r));
  EXPECT_EQ(r.entry.headword, "嘲笑");
  EXPECT_EQ(r.matchLength, text.size());
}

TEST_P(WordLookupSuru, GodanVerbStillResolves) {
  const std::string text = "嘲笑った";
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup(text, 0, r));
  EXPECT_EQ(r.entry.headword, "嘲笑う");
  EXPECT_EQ(r.matchLength, text.size());
}

TEST_P(WordLookupSuru, SuruVerbHeadwordWinsOverItsNoun) {
  const std::string text = "対して";
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup(text, 0, r));
  EXPECT_EQ(r.entry.headword, "対する");
  EXPECT_EQ(r.matchLength, text.size());
}

TEST_P(WordLookupSuru, KanaStemIsNotTakenAsASuruNoun) {
  WordLookupResult r;
  ASSERT_TRUE(WordLookup::lookup("にする", 0, r));
  EXPECT_EQ(r.matchLength, std::strlen("に"));
}

INSTANTIATE_TEST_SUITE_P(PosFlags, WordLookupSuru,
                         ::testing::Values(DictIndexRecord::POS_VS, DictIndexRecord::POS_OTHER));

}  // namespace
