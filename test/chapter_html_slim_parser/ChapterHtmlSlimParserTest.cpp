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
  CssParser cssParser{caseDir()};
  std::unique_ptr<ChapterHtmlSlimParser> parser;
  std::vector<std::shared_ptr<TextBlock>> lines;

  static constexpr int LINE_HEIGHT = 16;
  static constexpr int GLYPH_INK = 8;
  static constexpr int SPACE_WIDTH = 4;

  // Its own directory per test: ctest runs these as CONCURRENT PROCESSES (`ctest -j` in
  // ci.yml), so a shared stylesheet path lets one case read the CSS another just wrote.
  std::string caseDir() const {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("matcha-dropcap-" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::create_directories(dir);
    return dir.string();
  }

  void makeParser(const std::string& css, const CssTextAlign alignment = CssTextAlign::Justify) {
    ASSERT_TRUE(loadCss(css));
    parser = std::make_unique<ChapterHtmlSlimParser>(
        nullptr, filepath, renderer, 0, 1.0f, false, static_cast<uint8_t>(alignment),
        static_cast<uint16_t>(renderer.getScreenWidth()), static_cast<uint16_t>(renderer.getScreenHeight()), false,
        false, false, std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t)>{}, true, "", "", 0,
        std::vector<std::string>{}, std::function<void()>{}, &cssParser);
    parser->currentTextBlock = std::make_unique<ParsedText>(false);
    // The root entry beginParse() would have pushed: block elements read the enclosing style
    // off the top of this stack, and these tests drive startElement without a full parse.
    parser->blockStyleStack.push_back(BlockStyle{});
    stubLineWords.clear();
    stubLineXPos.clear();
  }

  bool loadCss(const std::string& css) {
    const auto path = std::filesystem::path(caseDir()) / "dropcap.css";
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

  // `<span class="...">text</span>`, driven through the real element callbacks so the inline
  // drop cap claim and its release at the close tag both run.
  void feedSpan(const char* classAttr, const char* text) {
    const XML_Char* attributes[] = {"class", classAttr, nullptr};
    ChapterHtmlSlimParser::startElement(parser.get(), "span", attributes);
    ChapterHtmlSlimParser::characterData(parser.get(), text, static_cast<int>(strlen(text)));
    ChapterHtmlSlimParser::endElement(parser.get(), "span");
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

// The CSS 2.1 one-colon spelling is what a good many EPUB toolchains emit -- often beside the
// two-colon one in the same stylesheet -- so it has to reach the layout the same way.
TEST_F(DropCapTest, AcceptsTheCss2OneColonSpelling) {
  makeParser("p.opener:first-letter { font-size: 300%; }\n");
  openParagraph("opener");
  ASSERT_EQ(parser->currentTextBlock->getBlockStyle().dropCapLines, 3u);

  feedParagraph(20);
  layout();
  ASSERT_FALSE(lines.empty());
  const auto& cap = lines[0]->getDropCap();
  ASSERT_TRUE(cap.present());
  EXPECT_EQ(cap.cp, static_cast<uint32_t>('L'));
}

// Plenty of books never write the pseudo-element at all: the initial is marked up as an enlarged
// span opening the paragraph (`class="lettrine"` / `class="let"` in French trade EPUBs). Sizing
// that span through the font ladder cannot produce a drop cap -- 270% of an 18pt reader font
// snaps back to the largest resident size, i.e. no change -- so it has to reach the same
// magnified-glyph path the pseudo-element takes.
TEST_F(DropCapTest, ClaimsADropCapFromAnEnlargedOpeningSpan) {
  makeParser(".let { font-size: 270%; }\n");
  parser->insideBody = true;
  openParagraph();
  feedSpan("let", "L");
  ASSERT_EQ(parser->currentTextBlock->getBlockStyle().dropCapLines, 3u);

  for (int i = 0; i < 40; ++i) feedWord("syndicat");
  layout();

  ASSERT_FALSE(lines.empty());
  const auto& cap = lines[0]->getDropCap();
  ASSERT_TRUE(cap.present());
  EXPECT_EQ(cap.cp, static_cast<uint32_t>('L'));
}

// An enlarged span holding a WORD is big text, not an initial. The count is unknowable when the
// span opens, so the claim is provisional and has to be given back at the close tag.
TEST_F(DropCapTest, ReleasesAnEnlargedOpeningSpanThatHoldsMoreThanOneLetter) {
  makeParser(".let { font-size: 270%; }\n");
  parser->insideBody = true;
  openParagraph();
  feedSpan("let", "Le");
  EXPECT_EQ(parser->currentTextBlock->getBlockStyle().dropCapLines, 0u);

  for (int i = 0; i < 40; ++i) feedWord("syndicat");
  layout();

  ASSERT_FALSE(lines.empty());
  EXPECT_FALSE(lines[0]->getDropCap().present());
}

// A paragraph does not have to open with the initial: a French chapter opens dialogue with an em
// dash and a no-break space, so both are already tokenized when the lettrine span arrives.
TEST_F(DropCapTest, ClaimsADropCapAfterTheEmDashAFrenchChapterOpensWith) {
  makeParser(".let { font-size: 270%; }\n");
  parser->insideBody = true;
  openParagraph();
  feedWord("\xE2\x80\x94");  // U+2014 em dash
  feedSpan("let", "L");
  ASSERT_EQ(parser->currentTextBlock->getBlockStyle().dropCapLines, 3u);

  for (int i = 0; i < 40; ++i) feedWord("syndicat");
  layout();

  ASSERT_FALSE(lines.empty());
  const auto& cap = lines[0]->getDropCap();
  ASSERT_TRUE(cap.present());
  EXPECT_EQ(cap.cp, static_cast<uint32_t>('L'));
  // The dash leaves the flow WITH the initial and is drawn at body size beside it. Left in the
  // text it would land to the right of the letter, since the flow starts past the column.
  EXPECT_EQ(cap.prefixCp, 0x2014u);
  ASSERT_FALSE(stubLineWords.empty());
  ASSERT_FALSE(stubLineWords[0].empty());
  EXPECT_EQ(stubLineWords[0][0], "syndicat") << "the dash should no longer be in the text flow";
  // The column holds the mark, a gap, the magnified glyph and the gap before the text.
  const int markAdvance = GLYPH_INK + SPACE_WIDTH;  // stub: one glyph advance per character
  ASSERT_FALSE(stubLineXPos.empty());
  ASSERT_FALSE(stubLineXPos[0].empty());
  EXPECT_EQ(stubLineXPos[0][0], markAdvance + GLYPH_INK * cap.scale + SPACE_WIDTH);
  EXPECT_EQ(cap.inkLeft, markAdvance) << "the enlarged letter should start after the mark";
}

// Mid-sentence emphasis must not blow its first letter up four lines tall. The word count alone
// cannot tell the two apart when the span opens, so the claim is provisional: prepareDropCap
// rejects it once it can see a LETTER sitting ahead of the initial.
TEST_F(DropCapTest, IgnoresAnEnlargedSpanThatIsNotTheParagraphOpening) {
  makeParser(".let { font-size: 270%; }\n");
  parser->insideBody = true;
  openParagraph();
  feedWord("Le");
  feedSpan("let", "S");

  for (int i = 0; i < 40; ++i) feedWord("syndicat");
  layout();

  ASSERT_FALSE(lines.empty());
  EXPECT_FALSE(lines[0]->getDropCap().present());
  // ...and the letter stays where the author put it.
  ASSERT_GE(stubLineWords[0].size(), 2u);
  EXPECT_EQ(stubLineWords[0][0], "Le");
  EXPECT_EQ(stubLineWords[0][1], "S");
}

// The sub-2x gate is the same one the pseudo-element path applies.
TEST_F(DropCapTest, IgnoresAMildlyEnlargedOpeningSpan) {
  makeParser(".let { font-size: 130%; }\n");
  parser->insideBody = true;
  openParagraph();
  feedSpan("let", "L");
  EXPECT_EQ(parser->currentTextBlock->getBlockStyle().dropCapLines, 0u);
}

// A single-size reader font (an SD-card font) has no 12/14/16/18pt ladder to snap to, so
// cssBlockFontId can only answer "no change" and an inline font-size is silently lost. A book
// that sets its small caps as `<small>` over literal capitals then renders them at FULL size,
// i.e. as plain capitals. Scaling the glyph bitmap is the only way to honour it.
TEST_F(DropCapTest, ScalesAnInlineFontSizeTheLadderCannotServe) {
  makeParser("small { font-size: 77%; }\n");
  parser->insideBody = true;
  openParagraph();
  feedWord("Le");

  const XML_Char* none[] = {nullptr};
  ChapterHtmlSlimParser::startElement(parser.get(), "small", none);
  feedWord("ORSQUE");
  ChapterHtmlSlimParser::endElement(parser.get(), "small");

  ASSERT_GE(parser->currentTextBlock->wordFonts.size(), 2u);
  EXPECT_EQ(parser->currentTextBlock->wordFonts[0], 0) << "an unsized word must carry no tag";
  // 77% snaps to the nearest eighth: 3/4, which decimates a 1-bit glyph on a clean period
  // instead of beating against the stem spacing.
  EXPECT_EQ(parser->currentTextBlock->wordFonts[1], -192);
  EXPECT_EQ(parser->currentTextBlock->effectiveWordScale(1), 192);
  EXPECT_EQ(parser->currentTextBlock->effectiveWordFont(1, 7), 7) << "a scale tag is not a font id";

  // The measured width has to follow the drawn size, or the next word lands inside this one.
  layout();
  ASSERT_FALSE(stubLineWords.empty());
  ASSERT_GE(stubLineWords[0].size(), 2u);
  ASSERT_GE(stubLineXPos[0].size(), 2u);
  const int leWidth = stubLineXPos[0][1] - stubLineXPos[0][0] - SPACE_WIDTH;
  EXPECT_EQ(leWidth, 2 * GLYPH_INK) << "the unscaled word keeps its full advance";
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

TEST_F(DropCapTest, KeepsTheFaceOfTheWordTheLetterCameFrom) {
  // An italic chapter opening must not get a regular initial: the glyph is measured and drawn
  // with the first word's own face, or the reserved column does not match what lands in it.
  makeParser("p::first-letter { font-size: 300%; }\n");
  openParagraph();
  parser->currentTextBlock->addWord("Le", EpdFontFamily::BOLD_ITALIC);
  for (int i = 0; i < 20; ++i) parser->currentTextBlock->addWord("syndicat", EpdFontFamily::BOLD_ITALIC);
  layout();

  ASSERT_FALSE(lines.empty());
  const auto& cap = lines[0]->getDropCap();
  ASSERT_TRUE(cap.present());
  EXPECT_EQ(cap.style, static_cast<uint8_t>(EpdFontFamily::BOLD_ITALIC));
}

TEST_F(DropCapTest, DropsDecorationAndScriptBitsFromTheDropCapFace) {
  // An underlined or superscripted opening word must not carry that into a letter drawn three
  // lines tall and outside the text flow -- only the face bits survive.
  makeParser("p::first-letter { font-size: 300%; }\n");
  openParagraph();
  const auto decorated =
      static_cast<EpdFontFamily::Style>(EpdFontFamily::BOLD | EpdFontFamily::UNDERLINE | EpdFontFamily::SUP);
  parser->currentTextBlock->addWord("Le", decorated);
  for (int i = 0; i < 20; ++i) parser->currentTextBlock->addWord("syndicat", decorated);
  layout();

  ASSERT_FALSE(lines.empty());
  const auto& cap = lines[0]->getDropCap();
  ASSERT_TRUE(cap.present());
  EXPECT_EQ(cap.style, static_cast<uint8_t>(EpdFontFamily::BOLD));
}

// The reserved column is an obstruction, not a text-indent: every alignment has to clear it.
// extractLine's right/center paths derive x from effectivePageWidth, which is already reduced by
// the indent, so without adding it back the line slides left into the drop cap.
TEST_F(DropCapTest, RightAndCentreAlignedLinesStayClearOfTheColumn) {
  for (const auto alignment : {CssTextAlign::Right, CssTextAlign::Center}) {
    lines.clear();
    makeParser("p::first-letter { font-size: 300%; }\n", alignment);
    openParagraph();
    feedParagraph(60);
    layout();

    ASSERT_FALSE(lines.empty());
    const auto& cap = lines[0]->getDropCap();
    ASSERT_TRUE(cap.present());
    const int expectedIndent = GLYPH_INK * cap.scale + SPACE_WIDTH;
    ASSERT_GE(stubLineXPos.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
      ASSERT_FALSE(stubLineXPos[i].empty());
      EXPECT_GE(stubLineXPos[i][0], expectedIndent)
          << "alignment " << static_cast<int>(alignment) << ", line " << i << " overlaps the drop cap";
    }
  }
}

// `::first-letter` is applied across whole classes of paragraph, so the guard has to hold for the
// quote marks EPUBs actually open dialogue with -- not just the ASCII one.
TEST_F(DropCapTest, LeavesAParagraphOpeningWithANonAsciiQuoteAlone) {
  for (const char* opening : {"\xE2\x80\x9CLe", "\xC2\xABLe"}) {  // U+201C left double quote, U+00AB «
    lines.clear();
    makeParser("p::first-letter { font-size: 300%; }\n");
    openParagraph();
    feedWord(opening);
    for (int i = 0; i < 20; ++i) feedWord("syndicat");
    layout();

    ASSERT_FALSE(lines.empty());
    EXPECT_FALSE(lines[0]->getDropCap().present()) << "opening " << opening << " became a drop cap";
  }
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

TEST_F(ChapterHtmlSlimParserTest, ParagraphWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, HeaderWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "h1", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

TEST_F(ChapterHtmlSlimParserTest, SpanWithHiddenAttributeShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "Before ", 7);
  ChapterHtmlSlimParser::startElement(&parser, "span", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);
  ChapterHtmlSlimParser::endElement(&parser, "span");
  ChapterHtmlSlimParser::characterData(&parser, " After ", 7);

  ASSERT_EQ(parser.currentTextBlock->size(), 2);
  ASSERT_EQ(parser.currentTextBlock->words[0], "Before");
  ASSERT_EQ(parser.currentTextBlock->words[1], "After");
}

TEST_F(ChapterHtmlSlimParserTest, DivWithHiddenAttributeContentShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

}  // namespace
