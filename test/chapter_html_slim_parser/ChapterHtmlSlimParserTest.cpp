#include <Epub.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ChapterHtmlSlimParser.h and its own includes' STL dependencies, explicit here so the
// macro below never rewrites an as-yet-unincluded header's own `template <class T>` into
// invalid `template <struct T>` -- relying on transitive include order from gtest/Page.h
// would be fragile.
#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

namespace {

// A hardcoded "/tmp" isn't portable (Windows runners, sandboxes without a writable /tmp) --
// mirrors the css_parser test's use of std::filesystem::temp_directory_path().
std::string cssCacheDir() {
  const auto dir = std::filesystem::temp_directory_path() / "chapter-html-slim-parser-test";
  std::filesystem::create_directories(dir);
  return dir.string();
}

class ChapterHtmlSlimParserTest : public ::testing::TestWithParam<const char*> {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{cssCacheDir()};
  ChapterHtmlSlimParser parser{nullptr,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               0,
                               static_cast<uint16_t>(renderer.getScreenWidth()),
                               static_cast<uint16_t>(renderer.getScreenHeight()),
                               false,
                               false,
                               false,
                               {},
                               true,
                               "",
                               "",
                               0,
                               {},
                               nullptr,
                               &cssParser};

  void SetUp() override { parser.currentTextBlock = std::make_unique<ParsedText>(false); }
};

TEST_P(ChapterHtmlSlimParserTest, KeepsCssVerticalAlignAndInternalLinkMetadata) {
  const char* verticalAlign = GetParam();
  const char* expectedHref = "#note-target";
  const XML_Char* attributes[] = {"href", expectedHref, "style", verticalAlign, nullptr};

  ChapterHtmlSlimParser::startElement(&parser, "a", attributes);
  const uint8_t linkId = parser.currentFootnoteLinkId;
  ASSERT_NE(linkId, 0u);
  ChapterHtmlSlimParser::characterData(&parser, "1", 1);
  ChapterHtmlSlimParser::endElement(&parser, "a");

  ASSERT_EQ(parser.currentTextBlock->size(), 1u);
  const auto style = parser.currentTextBlock->getWordStyleAt(0);
  const auto expectedStyle =
      std::string(verticalAlign).find("super") != std::string::npos ? EpdFontFamily::SUP : EpdFontFamily::SUB;
  EXPECT_NE(static_cast<uint8_t>(style) & static_cast<uint8_t>(expectedStyle), 0u);

  ASSERT_EQ(parser.pendingFootnotes.size(), 1u);
  const FootnoteEntry& footnote = parser.pendingFootnotes.front().second;
  EXPECT_EQ(footnote.href, expectedHref);
  ASSERT_EQ(parser.currentTextBlock->wordLinkIds.size(), 1u);
  EXPECT_EQ(parser.currentTextBlock->wordLinkIds.front(), linkId);
  EXPECT_TRUE(parser.currentTextBlock->linkTargetMatches(linkId, expectedHref));
}

INSTANTIATE_TEST_SUITE_P(CssVerticalAlign, ChapterHtmlSlimParserTest,
                         ::testing::Values("vertical-align: super", "vertical-align: sub"));

class ChapterHtmlSlimParserFrenchInversionTest : public ::testing::Test {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{cssCacheDir()};
  std::shared_ptr<Epub> epub = std::make_shared<Epub>();
  std::unique_ptr<ChapterHtmlSlimParser> parser;

  void makeParser(const char* language) {
    epub->language = language;
    parser = std::make_unique<ChapterHtmlSlimParser>(
        epub, filepath, renderer, 0, 1.0f, false, 0, static_cast<uint16_t>(renderer.getScreenWidth()),
        static_cast<uint16_t>(renderer.getScreenHeight()), false, false, false,
        std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>{}, true, "", "", 0,
        std::vector<std::string>{}, std::function<void()>{}, &cssParser);
    parser->currentTextBlock = std::make_unique<ParsedText>(false);
  }

  // Feeds `text` and forces a flush, as if it were followed by whitespace.
  void feedWord(const char* text) {
    ChapterHtmlSlimParser::characterData(parser.get(), text, static_cast<int>(strlen(text)));
    parser->flushPartWordBuffer();
  }
};

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsVerbAndPronoun) {
  makeParser("fr");
  feedWord("songeai-je");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "songeai");
  EXPECT_EQ(parser->currentTextBlock->words[1], "-");
  EXPECT_EQ(parser->currentTextBlock->words[2], "je");
  // The connector and pronoun stay glued to the verb: no rendered gap, matching the source.
  EXPECT_TRUE(parser->currentTextBlock->wordContinues[1]);
  EXPECT_TRUE(parser->currentTextBlock->wordContinues[2]);
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsAroundEuphonicT) {
  makeParser("fr");
  feedWord("pense-t-il");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "pense");
  EXPECT_EQ(parser->currentTextBlock->words[1], "-t-");
  EXPECT_EQ(parser->currentTextBlock->words[2], "il");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsLexicalizedCompoundsWhole) {
  makeParser("fr");
  feedWord("rendez-vous");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "rendez-vous");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsOrdinaryCompoundsWhole) {
  makeParser("fr");
  feedWord("grand-mère");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "grand-mère");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, DoesNotSplitInNonFrenchBooks) {
  makeParser("en");
  feedWord("songeai-je");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "songeai-je");
}

}  // namespace
