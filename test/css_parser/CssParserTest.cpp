// Covers the flat-pool rule store: lookup, merge semantics, style deduplication, compound
// selectors, and the cache round-trip that has to survive the storage change byte for byte.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "CssParser.h"

namespace {

class CssParserTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("cssparser-" + std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->line()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string cachePath() const { return dir_.string(); }

  // Feeds CSS through the real streaming parser rather than poking the store directly.
  bool loadCss(CssParser& parser, const std::string& css) const {
    const std::string path = (dir_ / "in.css").string();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(css.data(), 1, css.size(), f);
    std::fclose(f);
    HalFile file;
    if (!file.open(path.c_str(), "rb")) return false;
    return parser.loadFromStream(file);
  }

  std::filesystem::path dir_;
};

TEST_F(CssParserTest, ResolvesTagAndClassRules) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, "p { text-align: center; }\n.lead { font-weight: bold; }\n"));

  const CssStyle tag = parser.resolveStyle("p", "");
  EXPECT_TRUE(tag.defined.textAlign);
  EXPECT_EQ(tag.textAlign, CssTextAlign::Center);

  const CssStyle cls = parser.resolveStyle("span", "lead");
  EXPECT_TRUE(cls.defined.fontWeight);
}

TEST_F(CssParserTest, SelectorMatchingIsCaseInsensitive) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, "P.Lead { text-align: right; }\n"));

  const CssStyle style = parser.resolveStyle("p", "lead");
  EXPECT_TRUE(style.defined.textAlign);
  EXPECT_EQ(style.textAlign, CssTextAlign::Right);
}

TEST_F(CssParserTest, RepeatedSelectorMergesRatherThanDuplicating) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, "p { text-align: center; }\np { font-weight: bold; }\n"));

  EXPECT_EQ(parser.ruleCount(), 1u);
  const CssStyle style = parser.resolveStyle("p", "");
  EXPECT_TRUE(style.defined.textAlign);
  EXPECT_TRUE(style.defined.fontWeight);
}

// A merge onto a body that other entries share must not rewrite it underneath them.
TEST_F(CssParserTest, MergingASharedStyleBodyLeavesTheOtherHolderIntact) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, "a { text-align: center; }\nb { text-align: center; }\nb { font-weight: bold; }\n"));

  const CssStyle a = parser.resolveStyle("a", "");
  EXPECT_TRUE(a.defined.textAlign);
  EXPECT_FALSE(a.defined.fontWeight) << "the shared body was mutated through b's entry";

  const CssStyle b = parser.resolveStyle("b", "");
  EXPECT_TRUE(b.defined.fontWeight);
}

// Identical declaration blocks across many selectors should intern to one body.
TEST_F(CssParserTest, IdenticalBodiesAreDeduplicated) {
  std::string css;
  for (int i = 0; i < 40; ++i) css += ".u" + std::to_string(i) + " { text-align: center; }\n";
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, css));

  EXPECT_EQ(parser.ruleCount(), 40u);
  for (int i = 0; i < 40; ++i) {
    const CssStyle style = parser.resolveStyle("p", "u" + std::to_string(i));
    EXPECT_TRUE(style.defined.textAlign) << "class u" << i;
  }
}

TEST_F(CssParserTest, CacheRoundTripPreservesRules) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer, "p { text-align: center; margin-top: 2em; }\n.lead { font-weight: bold; }\n"));
    ASSERT_TRUE(writer.saveToCache());
  }

  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  EXPECT_EQ(reader.ruleCount(), 2u);

  const CssStyle style = reader.resolveStyle("p", "");
  EXPECT_TRUE(style.defined.textAlign);
  EXPECT_EQ(style.textAlign, CssTextAlign::Center);
  EXPECT_TRUE(style.defined.marginTop);
  EXPECT_FLOAT_EQ(style.marginTop.value, 2.0f);
}

TEST_F(CssParserTest, IncrementalAppendRoundTripsAndMergesAcrossFlushes) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(writer.beginCacheAppend());
    ASSERT_TRUE(loadCss(writer, "p { text-align: center; }\n"));
    ASSERT_TRUE(writer.appendRulesToCache());
    writer.clear();
    // Same selector in a later file: the load must merge it onto the earlier record.
    ASSERT_TRUE(loadCss(writer, "p { font-weight: bold; }\n"));
    ASSERT_TRUE(writer.appendRulesToCache());
    ASSERT_TRUE(writer.endCacheAppend(/*discard=*/false));
  }

  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  const CssStyle style = reader.resolveStyle("p", "");
  EXPECT_TRUE(style.defined.textAlign);
  EXPECT_TRUE(style.defined.fontWeight);
}

// The reason the append writes to a temp file at all.
TEST_F(CssParserTest, DiscardedAppendLeavesThePreviousCacheIntact) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer, "p { text-align: center; }\n"));
    ASSERT_TRUE(writer.saveToCache());
  }
  ASSERT_TRUE(CssParser(cachePath()).hasCache());

  {
    CssParser retry(cachePath());
    ASSERT_TRUE(retry.beginCacheAppend());
    ASSERT_TRUE(loadCss(retry, ".other { font-weight: bold; }\n"));
    ASSERT_TRUE(retry.appendRulesToCache());
    EXPECT_FALSE(retry.endCacheAppend(/*discard=*/true));
  }

  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache()) << "the good cache was destroyed by a discarded re-parse";
  const CssStyle style = reader.resolveStyle("p", "");
  EXPECT_TRUE(style.defined.textAlign);
}

TEST_F(CssParserTest, StaleCacheVersionIsRejected) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer, "p { text-align: center; }\n"));
    ASSERT_TRUE(writer.saveToCache());
  }

  const std::string path = (dir_ / "css_rules.cache").string();
  std::FILE* f = std::fopen(path.c_str(), "r+b");
  ASSERT_NE(f, nullptr);
  const uint8_t bogus = 0;
  std::fwrite(&bogus, 1, 1, f);
  std::fclose(f);

  CssParser reader(cachePath());
  EXPECT_FALSE(reader.validateCache());
}

// Descendant and child selectors are this fork's addition on top of the tag/class rules; the
// flat store has to keep resolving them through the ancestor walk.
TEST_F(CssParserTest, ResolvesDescendantAndChildSelectors) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, ".callout p { text-align: center; }\nblockquote > p { font-weight: bold; }\n"));

  {  // <div class="callout"><p>  -- descendant matches
    CssElementPath path;
    path.push("div");
    path.setTopClasses("callout");
    path.push("p");
    const CssStyle style = parser.resolveStyle("p", "", &path);
    EXPECT_TRUE(style.defined.textAlign);
    EXPECT_EQ(style.textAlign, CssTextAlign::Center);
    EXPECT_FALSE(style.defined.fontWeight);
  }

  {  // <blockquote><p>  -- child matches
    CssElementPath path;
    path.push("blockquote");
    path.push("p");
    const CssStyle style = parser.resolveStyle("p", "", &path);
    EXPECT_TRUE(style.defined.fontWeight);
  }

  {  // a bare <p> elsewhere picks up neither
    CssElementPath path;
    path.push("section");
    path.push("p");
    const CssStyle style = parser.resolveStyle("p", "", &path);
    EXPECT_FALSE(style.defined.textAlign);
    EXPECT_FALSE(style.defined.fontWeight);
  }
}

// Compound rules must survive the cache round-trip, keyed the same way on the way back in.
TEST_F(CssParserTest, CompoundSelectorsSurviveTheCacheRoundTrip) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer, ".callout p { text-align: center; }\n"));
    ASSERT_TRUE(writer.saveToCache());
  }

  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  CssElementPath path;
  path.push("div");
  path.setTopClasses("callout");
  path.push("p");
  const CssStyle style = reader.resolveStyle("p", "", &path);
  EXPECT_TRUE(style.defined.textAlign);
}

}  // namespace
