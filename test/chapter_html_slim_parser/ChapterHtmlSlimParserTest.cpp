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

// Recorded by the TextBlock test double in ParserLinkStubs.cpp: this binary links a stub
// constructor (the real one flattens into an arena whose render path needs a full renderer),
// so the per-word data a line was built from is read back from there. Global scope on
// purpose -- an extern inside the anonymous namespace would name a different symbol.
extern std::vector<std::vector<std::string>> stubLineWords;
extern std::vector<std::vector<int16_t>> stubLineXPos;

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

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsAroundEuphonicTWhenUppercased) {
  makeParser("fr");
  // Simulates the buffer after a CSS text-transform: uppercase run has already applied --
  // the euphonic "-t-" check must fold case, not just match lowercase 't'.
  feedWord("PENSE-T-IL");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "PENSE");
  EXPECT_EQ(parser->currentTextBlock->words[1], "-T-");
  EXPECT_EQ(parser->currentTextBlock->words[2], "IL");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsWithTrailingPunctuation) {
  makeParser("fr");
  // The tokenizer only splits on whitespace, so punctuation right after the inversion (a
  // comma before a dialogue tag, a question mark) stays glued to the buffered word.
  feedWord("songeai-je,");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "songeai");
  EXPECT_EQ(parser->currentTextBlock->words[1], "-");
  EXPECT_EQ(parser->currentTextBlock->words[2], "je,");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsAroundEuphonicTWithTrailingPunctuation) {
  makeParser("fr");
  feedWord("pense-t-il?");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "pense");
  EXPECT_EQ(parser->currentTextBlock->words[1], "-t-");
  EXPECT_EQ(parser->currentTextBlock->words[2], "il?");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsLexicalizedCompoundsWhole) {
  makeParser("fr");
  feedWord("rendez-vous");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "rendez-vous");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsLexicalizedCompoundsWholeWithTrailingPunctuation) {
  makeParser("fr");
  feedWord("rendez-vous.");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "rendez-vous.");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsSecondLexicalizedCompoundWhole) {
  makeParser("fr");
  // Would otherwise match the euphonic "-t-on" pattern (verb "dira" + pronoun "on").
  feedWord("qu'en-dira-t-on");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->words[0], "qu'en-dira-t-on");
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

// Drop caps, end to end: a `::first-letter` font-size has to reach the layout, take the letter
// out of the flow, indent the lines beside the enlarged glyph and release the ones below it.
//
// Against the stub renderer (stubs/GfxRenderer.h): 16px lines, an 8x8 glyph ink box with
// top bearing 8, ascender 12, 4px spaces and an 8px advance per character.
class DropCapTest : public ::testing::Test {
 protected:
  std::string filepath = "unused.xhtml";
  GfxRenderer renderer;
  CssParser cssParser{cssCacheDir()};
  std::unique_ptr<ChapterHtmlSlimParser> parser;
  std::vector<std::shared_ptr<TextBlock>> lines;

  static constexpr int LINE_HEIGHT = 16;
  static constexpr int GLYPH_INK = 8;
  static constexpr int SPACE_WIDTH = 4;

  void makeParser(const std::string& css) {
    ASSERT_TRUE(loadCss(css));
    parser = std::make_unique<ChapterHtmlSlimParser>(
        nullptr, filepath, renderer, 0, 1.0f, false, 0, static_cast<uint16_t>(renderer.getScreenWidth()),
        static_cast<uint16_t>(renderer.getScreenHeight()), false, false, false,
        std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>{}, true, "", "", 0,
        std::vector<std::string>{}, std::function<void()>{}, &cssParser);
    parser->currentTextBlock = std::make_unique<ParsedText>(false);
    // The root entry beginParse() would have pushed: block elements read the enclosing style
    // off the top of this stack, and these tests drive startElement without a full parse.
    parser->blockStyleStack.push_back(BlockStyle{});
    stubLineWords.clear();
    stubLineXPos.clear();
  }

  bool loadCss(const std::string& css) {
    const auto path = std::filesystem::path(cssCacheDir()) / "dropcap.css";
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f == nullptr) return false;
    std::fwrite(css.data(), 1, css.size(), f);
    std::fclose(f);
    HalFile file;
    if (!file.open(path.string().c_str(), "rb")) return false;
    return cssParser.loadFromStream(file);
  }

  void openParagraph(const char* classAttr = nullptr) {
    if (classAttr != nullptr) {
      const XML_Char* attributes[] = {"class", classAttr, nullptr};
      ChapterHtmlSlimParser::startElement(parser.get(), "p", attributes);
    } else {
      const XML_Char* attributes[] = {nullptr};
      ChapterHtmlSlimParser::startElement(parser.get(), "p", attributes);
    }
  }

  void feedWord(const char* text) {
    ChapterHtmlSlimParser::characterData(parser.get(), text, static_cast<int>(strlen(text)));
    parser->flushPartWordBuffer();
  }

  // Enough words that the drop cap's lines fill and the paragraph runs past them.
  void feedParagraph(const size_t wordCount) {
    feedWord("Le");
    for (size_t i = 1; i < wordCount; ++i) feedWord("syndicat");
  }

  void layout() {
    parser->currentTextBlock->layoutAndExtractLines(
        renderer, 0, static_cast<uint16_t>(renderer.getScreenWidth()),
        [this](const std::shared_ptr<TextBlock>& line, uint32_t) { lines.push_back(line); });
  }
};

TEST_F(DropCapTest, WrapsTheOpeningLinesAroundAnEnlargedFirstLetter) {
  makeParser("p::first-letter { font-size: 300%; }\n");
  openParagraph();
  ASSERT_EQ(parser->currentTextBlock->getBlockStyle().dropCapLines, 3u);

  feedParagraph(60);
  layout();
  ASSERT_GT(lines.size(), 4u);

  // 300% of a 16px line is three lines tall; an 8px-tall glyph magnified by whole pixels to
  // fill 48px is 6x, and the column it needs is that plus one space.
  const auto& cap = lines[0]->getDropCap();
  ASSERT_TRUE(cap.present());
  EXPECT_EQ(cap.cp, static_cast<uint32_t>('L'));
  EXPECT_EQ(cap.scale, (LINE_HEIGHT * 3) / GLYPH_INK);
  EXPECT_EQ(cap.inkLeft, 0);
  EXPECT_EQ(cap.inkTop, 4) << "the enlarged ink top should meet the line's own cap height";

  const int expectedIndent = GLYPH_INK * cap.scale + SPACE_WIDTH;
  ASSERT_GE(stubLineXPos.size(), 4u);
  for (size_t i = 0; i < 3; ++i) {
    ASSERT_FALSE(stubLineXPos[i].empty());
    EXPECT_EQ(stubLineXPos[i][0], expectedIndent) << "line " << i << " should clear the drop cap column";
  }
  // The fourth line has passed the enlarged letter and returns to the full column -- with no
  // first-line indent, which the drop cap's own opening line already stood in for.
  ASSERT_FALSE(stubLineXPos[3].empty());
  EXPECT_EQ(stubLineXPos[3][0], 0);

  // Only the first line draws it.
  for (size_t i = 1; i < lines.size(); ++i) {
    EXPECT_FALSE(lines[i]->getDropCap().present()) << "line " << i << " redraws the drop cap";
  }
}

TEST_F(DropCapTest, TheLetterLeavesTheTextFlow) {
  makeParser("p::first-letter { font-size: 300%; }\n");
  openParagraph();
  feedParagraph(20);
  layout();

  ASSERT_FALSE(stubLineWords.empty());
  ASSERT_FALSE(stubLineWords[0].empty());
  // "Le" opened the paragraph; the L became the drop cap, so the flow starts at "e".
  EXPECT_EQ(stubLineWords[0][0], "e");
}

TEST_F(DropCapTest, IgnoresARuleThatOnlyMildlyEnlargesTheLetter) {
  // Under 2x there is no room beside the letter to wrap into, so the paragraph stays ordinary
  // and the letter keeps its place in the text.
  makeParser("p::first-letter { font-size: 130%; }\n");
  openParagraph();
  ASSERT_EQ(parser->currentTextBlock->getBlockStyle().dropCapLines, 0u);

  feedParagraph(20);
  layout();
  ASSERT_FALSE(lines.empty());
  EXPECT_FALSE(lines[0]->getDropCap().present());
  ASSERT_FALSE(stubLineWords.empty());
  ASSERT_FALSE(stubLineWords[0].empty());
  EXPECT_EQ(stubLineWords[0][0], "Le");
}

TEST_F(DropCapTest, LeavesAParagraphOpeningWithPunctuationAlone) {
  // `::first-letter` is applied by class across whole books; a paragraph that happens to open
  // with a quote must not blow that mark up to three lines tall.
  makeParser("p::first-letter { font-size: 300%; }\n");
  openParagraph();
  feedWord("\"Le");
  for (int i = 0; i < 20; ++i) feedWord("syndicat");
  layout();

  ASSERT_FALSE(lines.empty());
  EXPECT_FALSE(lines[0]->getDropCap().present());
  ASSERT_FALSE(stubLineXPos.empty());
  ASSERT_FALSE(stubLineXPos[0].empty());
  // No column was reserved, so the line keeps the paragraph's ordinary first-line indent.
  EXPECT_EQ(stubLineXPos[0][0], SPACE_WIDTH * 3);
}

}  // namespace
