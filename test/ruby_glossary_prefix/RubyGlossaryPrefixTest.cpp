// The book's furigana glossary lets word lookup select a name the dictionary only knows in parts
// (緋倉美津子: the dictionary has 緋 and 美津子, the book annotates the whole name).

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "HalStorage.h"
#include "RubyGlossary.h"

namespace {

class RubyGlossaryPrefix : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / ("cp_ruby_glossary_prefix_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    book_ = (root_ / "book").string();
    std::filesystem::create_directories(book_);
    RubyGlossary::merge(
        book_, {{"緋", "ひ"}, {"緋倉正彦", "ひくらまさひこ"}, {"緋倉美津子", "ひくらみつこ"}, {"美津", "みつ"}});
  }
  void TearDown() override { std::filesystem::remove_all(root_); }
  std::filesystem::path root_;
  std::string book_;
};

TEST_F(RubyGlossaryPrefix, FindsTheLongestAnnotatedNameAtTheStart) {
  std::string base, readings;
  ASSERT_TRUE(RubyGlossary::longestPrefix(book_, "緋倉美津子さんは", base, readings));
  EXPECT_EQ(base, "緋倉美津子");
  EXPECT_EQ(readings, "ひくらみつこ");
}

TEST_F(RubyGlossaryPrefix, IgnoresSingleCharacterBases) {
  std::string base, readings;
  EXPECT_FALSE(RubyGlossary::longestPrefix(book_, "緋色", base, readings));
  EXPECT_TRUE(base.empty());
}

TEST_F(RubyGlossaryPrefix, JoinsDistinctReadingsUpToACap) {
  std::vector<std::pair<std::string, std::string>> pairs;
  for (int i = 0; i < 200; i++) pairs.emplace_back("東京", "とうきょう" + std::to_string(i));
  pairs.emplace_back("東京", "とうきょう0");
  RubyGlossary::merge(book_, pairs);
  std::string base, readings;
  ASSERT_TRUE(RubyGlossary::longestPrefix(book_, "東京駅", base, readings));
  EXPECT_EQ(base, "東京");
  EXPECT_LE(readings.size(), 96u);
  EXPECT_EQ(readings.find("とうきょう0"), 0u);
  EXPECT_EQ(readings.find("とうきょう0・", 1), std::string::npos);
}

TEST_F(RubyGlossaryPrefix, SpanKeysMatchTheMultiCharacterBases) {
  std::vector<RubyGlossary::SpanKey> keys;
  EXPECT_EQ(RubyGlossary::loadSpanKeys(book_, keys), 4);
  ASSERT_EQ(keys.size(), 3u);
  const std::string name = "緋倉美津子";
  const uint32_t h = RubyGlossary::hashText(name.data(), name.size());
  bool found = false;
  for (const auto& k : keys) found |= k.hash == h && k.chars == 5;
  EXPECT_TRUE(found);
}

TEST_F(RubyGlossaryPrefix, NoGlossaryMeansNoKeys) {
  std::vector<RubyGlossary::SpanKey> keys;
  EXPECT_EQ(RubyGlossary::loadSpanKeys((root_ / "missing").string(), keys), 0);
  EXPECT_TRUE(keys.empty());
}

}  // namespace
