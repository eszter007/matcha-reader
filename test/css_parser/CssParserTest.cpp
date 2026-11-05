// Covers the flat-pool rule store: lookup, merge semantics, style deduplication, compound
// selectors, and the cache round-trip that has to survive the storage change byte for byte.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

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

TEST_F(CssParserTest, FontInheritIsKeptThroughTheCascade) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser,
                      "p { font-weight:bold; font-style:italic; }"
                      ".last { font-weight:bold; font-weight:inherit; font-style:italic; font-style:INHERIT; }"
                      ".normal { font-weight:inherit; font-weight:normal; font-style:inherit; font-style:normal; }"));
  const CssStyle last = parser.resolveStyle("p", "last");
  EXPECT_EQ(last.fontWeight, CssFontWeight::Inherit);
  EXPECT_EQ(last.fontStyle, CssFontStyle::Inherit);
  const CssStyle normal = parser.resolveStyle("p", "normal");
  EXPECT_EQ(normal.fontWeight, CssFontWeight::Normal);
  EXPECT_EQ(normal.fontStyle, CssFontStyle::Normal);
  const CssStyle inlineStyle = CssParser::parseInlineStyle("font-weight:bold;font-weight:inherit;font-style:inherit");
  EXPECT_EQ(inlineStyle.fontWeight, CssFontWeight::Inherit);
  EXPECT_EQ(inlineStyle.fontStyle, CssFontStyle::Inherit);
}

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

// Writes the Matcha E-Book shape: a text stylesheet the chapters link, and an image-page one whose
// `body { text-align: center }` they do not. `copy.css` is an identical copy of `text.css`.
void writeTwoStylesheetCache(CssParser& writer, const std::function<bool(const std::string&)>& load) {
  const std::vector<std::string> sources{"OPS/css/text.css", "OPS/css/image.css", "OPS/css/copy.css"};
  const uint8_t aliases[] = {0, 1, 0};
  ASSERT_TRUE(writer.beginCacheAppend(&sources, aliases));
  writer.setSource(0);
  ASSERT_TRUE(load("p { font-weight: bold; }\n"));
  ASSERT_TRUE(writer.appendRulesToCache());
  writer.clear();
  writer.setSource(1);
  ASSERT_TRUE(load("body { text-align: center; }\n"));
  ASSERT_TRUE(writer.appendRulesToCache());
  writer.clear();
  ASSERT_TRUE(writer.endCacheAppend(/*discard=*/false));
}

TEST_F(CssParserTest, ChapterAppliesOnlyTheStylesheetsItLinks) {
  {
    CssParser writer(cachePath());
    writeTwoStylesheetCache(writer, [&](const std::string& css) { return loadCss(writer, css); });
  }
  CssParser reader(cachePath());
  ASSERT_TRUE(reader.validateCache());
  const auto filter = reader.stylesheetFilterFor({"OPS/css/text.css"});
  ASSERT_TRUE(reader.loadFromCache(nullptr, &filter));
  EXPECT_TRUE(reader.resolveStyle("p", "").defined.fontWeight);
  EXPECT_FALSE(reader.resolveStyle("body", "").defined.textAlign) << "an unlinked stylesheet leaked into the chapter";

  std::vector<CssParser::VerticalBlockRule> vertical;
  reader.collectVerticalStyles(vertical, 256, &filter);
  EXPECT_TRUE(vertical.empty()) << "the vertical engine centred a chapter from an unlinked stylesheet";
}

TEST_F(CssParserTest, LinkingAnIdenticalCopyAppliesTheOriginalsRules) {
  {
    CssParser writer(cachePath());
    writeTwoStylesheetCache(writer, [&](const std::string& css) { return loadCss(writer, css); });
  }
  CssParser reader(cachePath());
  const auto filter = reader.stylesheetFilterFor({"OPS/css/copy.css"});
  ASSERT_TRUE(reader.loadFromCache(nullptr, &filter));
  EXPECT_TRUE(reader.resolveStyle("p", "").defined.fontWeight);
  EXPECT_FALSE(reader.resolveStyle("body", "").defined.textAlign);
}

// The EBPAJ shape: chapters link only book-style.css, whose rules are all @imported -- one level
// through a "../" path, the next from inside the imported sheet.
TEST_F(CssParserTest, LinkedStylesheetBringsInWhatItImports) {
  {
    CssParser writer(cachePath());
    const std::vector<std::string> sources{"item/style/book-style.css", "item/style/style-standard.css",
                                           "item/style/style-advance.css", "item/style/image.css"};
    const uint8_t aliases[] = {0, 1, 2, 3};
    ASSERT_TRUE(writer.beginCacheAppend(&sources, aliases));
    const char* sheets[] = {
        "@charset \"UTF-8\";\n@import url(\"../style/style-standard.css\");\n",
        "@import \"style-advance.css\";\n.k-solid { border-style: solid solid solid solid; }\n",
        ".start-1em { margin-top: 1em; }\n",
        "body { text-align: center; }\n",
    };
    for (size_t i = 0; i < 4; ++i) {
      writer.setSource(i);
      ASSERT_TRUE(loadCss(writer, sheets[i]));
      ASSERT_TRUE(writer.appendRulesToCache());
      writer.clear();
    }
    ASSERT_TRUE(writer.endCacheAppend(/*discard=*/false));
  }
  CssParser reader(cachePath());
  ASSERT_TRUE(reader.validateCache());
  const auto filter = reader.stylesheetFilterFor({"item/style/book-style.css"});
  ASSERT_TRUE(reader.loadFromCache(nullptr, &filter));
  EXPECT_TRUE(reader.resolveStyle("div", "k-solid").hasBorder()) << "an @imported stylesheet was filtered out";
  EXPECT_TRUE(reader.resolveStyle("div", "start-1em").defined.marginTop) << "a nested @import was not followed";
  EXPECT_FALSE(reader.resolveStyle("body", "").defined.textAlign) << "an unlinked stylesheet leaked into the chapter";

  std::vector<CssParser::VerticalBlockRule> vertical;
  reader.collectVerticalStyles(vertical, 256, &filter);
  EXPECT_EQ(vertical.size(), 2u) << "the vertical engine must see the imported box and indent rules";
}

TEST_F(CssParserTest, ImportPathIsPercentDecoded) {
  {
    CssParser writer(cachePath());
    const std::vector<std::string> sources{"OPS/book.css", "OPS/my sheet.css"};
    const uint8_t aliases[] = {0, 1};
    ASSERT_TRUE(writer.beginCacheAppend(&sources, aliases));
    writer.setSource(0);
    ASSERT_TRUE(loadCss(writer, "@import \"my%20sheet.css\";\n"));
    ASSERT_TRUE(writer.appendRulesToCache());
    writer.clear();
    writer.setSource(1);
    ASSERT_TRUE(loadCss(writer, "p { font-weight: bold; }\n"));
    ASSERT_TRUE(writer.appendRulesToCache());
    writer.clear();
    ASSERT_TRUE(writer.endCacheAppend(/*discard=*/false));
  }
  CssParser reader(cachePath());
  const auto filter = reader.stylesheetFilterFor({"OPS/book.css"});
  ASSERT_TRUE(reader.loadFromCache(nullptr, &filter));
  EXPECT_TRUE(reader.resolveStyle("p", "").defined.fontWeight);
}

TEST_F(CssParserTest, ChapterWithNoKnownLinkKeepsEveryStylesheet) {
  {
    CssParser writer(cachePath());
    writeTwoStylesheetCache(writer, [&](const std::string& css) { return loadCss(writer, css); });
  }
  CssParser reader(cachePath());
  for (const std::vector<std::string>& links :
       {std::vector<std::string>{}, std::vector<std::string>{"OPS/css/missing.css"}}) {
    const auto filter = reader.stylesheetFilterFor(links);
    ASSERT_TRUE(reader.loadFromCache(nullptr, &filter));
    EXPECT_TRUE(reader.resolveStyle("p", "").defined.fontWeight);
    EXPECT_TRUE(reader.resolveStyle("body", "").defined.textAlign);
  }
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

// ::first-letter is the one pseudo-element the selector parser accepts. It has to be reachable
// through its own lookup and INVISIBLE to the ordinary element cascade -- a drop cap's 300%
// must never become the paragraph's own font-size.
TEST_F(CssParserTest, FirstLetterRuleIsSeparateFromTheElementStyle) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, "p { text-align: justify; }\np::first-letter { font-size: 300%; }\n"));

  const CssStyle block = parser.resolveStyle("p", "");
  EXPECT_TRUE(block.defined.textAlign);
  EXPECT_FALSE(block.defined.fontSize) << "the pseudo-element rule leaked into the block cascade";

  CssLength size;
  ASSERT_TRUE(parser.resolveFirstLetterFontSize("p", "", nullptr, size));
  EXPECT_EQ(size.unit, CssUnit::Percent);
  EXPECT_FLOAT_EQ(size.value, 300.0f);

  CssLength other;
  EXPECT_FALSE(parser.resolveFirstLetterFontSize("div", "", nullptr, other));
}

TEST_F(CssParserTest, FirstLetterMatchesClassAndCompoundSelectors) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser,
                      ".opener::first-letter { font-size: 3em; }\n"
                      ".chapter p::first-letter { font-size: 4em; }\n"));

  CssLength size;
  ASSERT_TRUE(parser.resolveFirstLetterFontSize("p", "opener", nullptr, size));
  EXPECT_FLOAT_EQ(size.value, 3.0f);

  CssElementPath path;
  path.push("div");
  path.setTopClasses("chapter");
  path.push("p");
  CssLength descendant;
  ASSERT_TRUE(parser.resolveFirstLetterFontSize("p", "", &path, descendant));
  EXPECT_FLOAT_EQ(descendant.value, 4.0f);
}

// CSS 2.1 spelled pseudo-elements with one colon and EPUB toolchains still emit that form, so
// both spellings have to reach the same stored rule.
TEST_F(CssParserTest, FirstLetterAcceptsTheCss2OneColonSpelling) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser,
                      "p:first-letter { font-size: 300%; }\n"
                      "p.opener:first-letter { font-size: 4em; }\n"));

  const CssStyle block = parser.resolveStyle("p", "");
  EXPECT_FALSE(block.defined.fontSize) << "the pseudo-element rule leaked into the block cascade";

  CssLength size;
  ASSERT_TRUE(parser.resolveFirstLetterFontSize("p", "", nullptr, size));
  EXPECT_FLOAT_EQ(size.value, 300.0f);

  CssLength scoped;
  ASSERT_TRUE(parser.resolveFirstLetterFontSize("p", "opener", nullptr, scoped));
  EXPECT_FLOAT_EQ(scoped.value, 4.0f);
}

// Both spellings normalize to ONE key, so a stylesheet using both (they routinely coexist in the
// same file) cascades them against each other rather than stashing two unrelated rules.
TEST_F(CssParserTest, BothFirstLetterSpellingsShareOneStoredKey) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser,
                      "p::first-letter { font-size: 300%; }\n"
                      "p:first-letter { font-size: 4em; }\n"));
  EXPECT_EQ(parser.ruleCount(), 1u);

  CssLength size;
  ASSERT_TRUE(parser.resolveFirstLetterFontSize("p", "", nullptr, size));
  EXPECT_EQ(size.unit, CssUnit::Em) << "the later rule should win, as it would in a browser";
  EXPECT_FLOAT_EQ(size.value, 4.0f);
}

// Everything else with a colon stays rejected, and a mid-selector spelling is not a suffix.
// The bare forms need the universal selector this engine does not implement; the one-colon
// strip must not turn `::first-letter` into a compound with a stray leading ':'.
TEST_F(CssParserTest, OtherPseudoSelectorsAreStillRejected) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser,
                      "p:hover { font-size: 300%; }\n"
                      "p::before { font-size: 300%; }\n"
                      "p:first-line { font-size: 300%; }\n"
                      "p::first-letter span { font-size: 300%; }\n"
                      "p:first-letter span { font-size: 300%; }\n"
                      "::first-letter { font-size: 300%; }\n"
                      ":first-letter { font-size: 300%; }\n"));
  EXPECT_EQ(parser.ruleCount(), 0u);

  CssLength size;
  EXPECT_FALSE(parser.resolveFirstLetterFontSize("p", "", nullptr, size));
}

// A class-based first-letter rule has to survive the chapter-usage filter loadFromCache applies:
// the pseudo hangs off the compound with no separator, so the class name is only recoverable
// once the suffix is stripped.
TEST_F(CssParserTest, FirstLetterRuleSurvivesTheFilteredCacheRoundTrip) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer, "p.opener::first-letter { font-size: 300%; }\n"));
    ASSERT_TRUE(writer.saveToCache());
  }

  CssParser reader(cachePath());
  const std::vector<std::string> usedClasses = {"opener"};
  ASSERT_TRUE(reader.loadFromCache(&usedClasses));

  CssLength size;
  ASSERT_TRUE(reader.resolveFirstLetterFontSize("p", "opener", nullptr, size));
  EXPECT_FLOAT_EQ(size.value, 300.0f);
}

// The EBPAJ writing-mode scope ("h|"/"v|") rebuilds a subject-only selector to build its key.
// That rebuild must carry the pseudo-element: without it the rule is stored under the PLAIN
// scoped key, where resolveStyle's own "h|" twin lookup finds it -- so a drop cap's 300% would
// be applied to the whole paragraph instead of its first letter.
TEST_F(CssParserTest, ScopedFirstLetterRuleDoesNotLeakIntoTheElementCascade) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, ".hltr p::first-letter { font-size: 300%; }\n"));

  const CssStyle block = parser.resolveStyle("p", "");
  EXPECT_FALSE(block.defined.fontSize) << "the drop cap size leaked onto the paragraph itself";

  CssLength size;
  ASSERT_TRUE(parser.resolveFirstLetterFontSize("p", "", nullptr, size));
  EXPECT_FLOAT_EQ(size.value, 300.0f);
}

TEST_F(CssParserTest, PreservedWhitespaceSurvivesCacheAndNormalOverridesIt) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer,
                        ".code { white-space: pre-wrap; }\n.plain { white-space: normal; }\n"
                        ".wrap { white-space: break-spaces; }\n.odd { white-space: inherit; }\n"));
    ASSERT_TRUE(writer.saveToCache());
  }
  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  const CssStyle code = reader.resolveStyle("div", "code");
  EXPECT_TRUE(code.hasWhiteSpace());
  EXPECT_TRUE(code.preserveWhitespace());
  EXPECT_TRUE(reader.resolveStyle("div", "wrap").preserveWhitespace());
  const CssStyle plain = reader.resolveStyle("div", "plain");
  EXPECT_TRUE(plain.hasWhiteSpace());
  EXPECT_FALSE(plain.preserveWhitespace());
  // An unsupported value leaves the property unset, so an inherited value keeps applying.
  EXPECT_FALSE(reader.resolveStyle("div", "odd").hasWhiteSpace());
}

TEST_F(CssParserTest, EqualSpecificityUsesSourceOrderAndSurvivesCache) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer,
                        ".zeta { text-align: right; }\n.alpha { text-align: center; }\n"
                        ".late { font-weight: bold; }\n.early { font-weight: normal; }\n"));
    // Later rule wins the tie, whatever the selectors' alphabetical order.
    EXPECT_EQ(writer.resolveStyle("p", "alpha zeta").textAlign, CssTextAlign::Center);
    EXPECT_EQ(writer.resolveStyle("p", "late early").fontWeight, CssFontWeight::Normal);
    ASSERT_TRUE(writer.saveToCache());
  }
  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  EXPECT_EQ(reader.resolveStyle("p", "alpha zeta").textAlign, CssTextAlign::Center);
  EXPECT_EQ(reader.resolveStyle("p", "zeta alpha").textAlign, CssTextAlign::Center);
  EXPECT_EQ(reader.resolveStyle("p", "late early").fontWeight, CssFontWeight::Normal);
}

TEST_F(CssParserTest, HigherSpecificityStillBeatsLaterSource) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser, "p.note { text-align: right; }\n.note { text-align: center; }\n"));
  EXPECT_EQ(parser.resolveStyle("p", "note").textAlign, CssTextAlign::Right);
}

TEST_F(CssParserTest, LightBackgroundIsShadedAndSurvivesCache) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer,
                        ".note { background-color: #ddd; }\n.page { background-color: #fff; }\n"
                        ".dark { color: #fff; background-color: #333; }\n"));
    EXPECT_EQ(writer.resolveStyle("div", "note").inkMode, CssInkMode::Shaded);
    EXPECT_EQ(writer.resolveStyle("div", "page").inkMode, CssInkMode::Normal);
    EXPECT_EQ(writer.resolveStyle("div", "dark").inkMode, CssInkMode::Inverted);
    ASSERT_TRUE(writer.saveToCache());
  }
  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  EXPECT_EQ(reader.resolveStyle("div", "note").inkMode, CssInkMode::Shaded);
}

TEST_F(CssParserTest, InitialLetterSizesTheInitialUnlessFontSizeIsSet) {
  CssParser parser(cachePath());
  ASSERT_TRUE(loadCss(parser,
                      ".dc { initial-letter: 3; }\n.both { initial-letter: 3; font-size: 2em; }\n"
                      ".one { initial-letter: 1; }\n"));
  const CssStyle dc = parser.resolveStyle("span", "dc");
  ASSERT_TRUE(dc.hasFontSize());
  EXPECT_FLOAT_EQ(dc.fontSize.value, 3.0f);
  EXPECT_EQ(dc.fontSize.unit, CssUnit::Em);
  EXPECT_FLOAT_EQ(parser.resolveStyle("span", "both").fontSize.value, 2.0f);
  EXPECT_FALSE(parser.resolveStyle("span", "one").hasFontSize());
}

}  // namespace

TEST_F(CssParserTest, AutoMarginsAndEndAlignReachTheVerticalEngine) {
  {
    CssParser writer(cachePath());
    ASSERT_TRUE(loadCss(writer,
                        ".vrtl .block-align-center { margin: auto 0; }\n"
                        ".vrtl .block-align-end { margin: auto 0 0 0; }\n"
                        ".align-end { text-align: right; }\n"
                        ".wrapper { margin: auto; }\n"));
    ASSERT_TRUE(writer.saveToCache());
  }
  CssParser reader(cachePath());
  ASSERT_TRUE(reader.loadFromCache());
  // A horizontal layout still reads an auto margin as 0.
  const CssStyle centered = reader.resolveStyle("div", "block-align-center");
  EXPECT_FLOAT_EQ(centered.marginTop.toPixels(16.0f), 0.0f);

  std::vector<CssParser::VerticalBlockRule> vertical;
  reader.collectVerticalStyles(vertical);
  const auto styleOf = [&](const char* cls) {
    const uint32_t hash = CssParser::blockSelectorHash(cls, strlen(cls));
    for (const auto& rule : vertical) {
      if (rule.classHash == hash) return rule.style;
    }
    return CssParser::VerticalBlockStyle{};
  };
  EXPECT_EQ(styleOf("block-align-center").blockAlign, CssParser::VERTICAL_BLOCK_CENTER);
  EXPECT_EQ(styleOf("block-align-end").blockAlign, CssParser::VERTICAL_BLOCK_END);
  EXPECT_TRUE(styleOf("align-end").alignEnd);
  EXPECT_FALSE(styleOf("align-end").alignCenter);
  EXPECT_EQ(styleOf("wrapper").blockAlign, 0) << "a plain auto margin centres a wrapper horizontally, not a block";
}
