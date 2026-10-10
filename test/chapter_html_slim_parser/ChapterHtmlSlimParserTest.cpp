#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/parsers/XhtmlDoctype.h>
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
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/activities/settings/TextSettingsPreview.h"
#include "src/clippings/ClippingText.h"
#include "src/util/ParagraphIndentMigration.h"

// ChapterHtmlSlimParser.h and its own includes' STL dependencies, explicit here so the
// macro below never rewrites an as-yet-unincluded header's own `template <class T>` into
// invalid `template <struct T>` -- relying on transitive include order from gtest/Page.h
// would be fragile.
#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

// Per-line word data, refilled from the real TextBlocks after each DropCapTest layout. Global
// scope on purpose -- an extern inside the anonymous namespace would name a different symbol.
extern std::vector<std::vector<std::string>> stubLineWords;
extern std::vector<std::vector<int16_t>> stubLineXPos;

namespace {

// Records one extracted line's words and x positions into the capture vectors.
void captureLine(const TextBlock& block) {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    words.emplace_back(block.wordText(i));
    xpos.push_back(block.wordXpos(i));
  }
  stubLineWords.push_back(std::move(words));
  stubLineXPos.push_back(std::move(xpos));
}

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

  void SetUp() override { parser.currentTextBlock = std::make_unique<ParsedText>(); }
};

TEST_F(ChapterHtmlSlimParserTest, RubySurvivesPartialParagraphExtraction) {
  ParsedText text;
  text.addWord("a", EpdFontFamily::REGULAR);
  text.addWord("b", EpdFontFamily::REGULAR);
  text.addWord("c", EpdFontFamily::REGULAR);
  text.setRubyForWordAt(2, "c");
  size_t lines = 0;
  text.layoutAndExtractLines(
      renderer, 0, 20,
      [&](std::unique_ptr<TextBlock> line, auto) {
        ++lines;
        EXPECT_TRUE(line->getRubyTexts().empty());
      },
      false);
  EXPECT_EQ(lines, 1u);
  const size_t retainedWords = text.size();
  ASSERT_GT(retainedWords, 0u);
  ASSERT_LT(retainedWords, 3u);
  text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) {
    ++lines;
    ASSERT_EQ(line->getRubyTexts().size(), retainedWords);
    EXPECT_EQ(line->getRubyTexts().back(), "c");
    for (size_t i = 0; i + 1 < retainedWords; ++i) EXPECT_TRUE(line->getRubyTexts()[i].empty());
  });
  EXPECT_EQ(lines, 2u);
}

TEST_F(ChapterHtmlSlimParserTest, UnequalTableCellsAndRubySurvivePageBreaks) {
  parser.viewportWidth = 240;
  parser.viewportHeight = 32;
  parser.tableRowCells.reserve(2);
  std::multiset<std::string> expected;
  for (int column = 0; column < 2; ++column) {
    auto cell = std::make_unique<ParsedText>();
    for (int index = 0; index < (column == 0 ? 30 : 3); ++index) {
      const auto word = std::string(column == 0 ? "left" : "right") + std::to_string(index);
      expected.insert(word);
      cell->addWord(word, EpdFontFamily::REGULAR);
    }
    if (column == 0) cell->setRubyGroupAt(0, 2, "reading");
    parser.tableRowCells.push_back(std::move(cell));
  }
  std::multiset<std::string> actual;
  unsigned pages = 0;
  unsigned rubyLines = 0;
  auto inspect = [&](std::unique_ptr<Page> page, auto, auto, auto) {
    ++pages;
    for (const auto& element : page->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = *line.getBlock();
      ASSERT_TRUE(block.valid());
      EXPECT_LE(element->yPos + 16 + block.getRubyShift(12), parser.viewportHeight);
      rubyLines += block.hasRuby();
      for (uint16_t word = 0; word < block.wordCount(); ++word) actual.insert(block.wordText(word));
    }
  };
  parser.completePageFn = inspect;
  parser.finishTableRow();
  ASSERT_NE(parser.currentPage, nullptr);
  inspect(std::move(parser.currentPage), 0, 0, 0);
  EXPECT_GT(pages, 2u);
  EXPECT_EQ(rubyLines, 1u);
  EXPECT_EQ(actual, expected);
  for (const auto& lines : parser.tableCellLines) EXPECT_TRUE(lines.empty());
}

TEST_F(ChapterHtmlSlimParserTest, PageImageDeserializeRejectsMissingImageBlock) {
  const auto path = std::filesystem::temp_directory_path() / "crosspoint-missing-image-cache.bin";
  {
    HalFile output;
    ASSERT_TRUE(output.open(path.c_str(), "wb"));
    const int16_t coordinates[] = {0, 0};
    output.write(coordinates, sizeof(coordinates));
  }
  HalFile input;
  ASSERT_TRUE(input.open(path.c_str(), "rb"));
  EXPECT_EQ(PageImage::deserialize(input), nullptr);
}

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
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "songeai");
  EXPECT_EQ(parser->currentTextBlock->wordAt(1), "-");
  EXPECT_EQ(parser->currentTextBlock->wordAt(2), "je");
  // The connector and pronoun stay glued to the verb: no rendered gap, matching the source.
  EXPECT_TRUE(parser->currentTextBlock->wordContinues[1]);
  EXPECT_TRUE(parser->currentTextBlock->wordContinues[2]);
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsAroundEuphonicT) {
  makeParser("fr");
  feedWord("pense-t-il");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "pense");
  EXPECT_EQ(parser->currentTextBlock->wordAt(1), "-t-");
  EXPECT_EQ(parser->currentTextBlock->wordAt(2), "il");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsAroundEuphonicTWhenUppercased) {
  makeParser("fr");
  // Simulates the buffer after a CSS text-transform: uppercase run has already applied --
  // the euphonic "-t-" check must fold case, not just match lowercase 't'.
  feedWord("PENSE-T-IL");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "PENSE");
  EXPECT_EQ(parser->currentTextBlock->wordAt(1), "-T-");
  EXPECT_EQ(parser->currentTextBlock->wordAt(2), "IL");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsWithTrailingPunctuation) {
  makeParser("fr");
  // The tokenizer only splits on whitespace, so punctuation right after the inversion (a
  // comma before a dialogue tag, a question mark) stays glued to the buffered word.
  feedWord("songeai-je,");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "songeai");
  EXPECT_EQ(parser->currentTextBlock->wordAt(1), "-");
  EXPECT_EQ(parser->currentTextBlock->wordAt(2), "je,");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, SplitsAroundEuphonicTWithTrailingPunctuation) {
  makeParser("fr");
  feedWord("pense-t-il?");

  ASSERT_EQ(parser->currentTextBlock->size(), 3u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "pense");
  EXPECT_EQ(parser->currentTextBlock->wordAt(1), "-t-");
  EXPECT_EQ(parser->currentTextBlock->wordAt(2), "il?");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsLexicalizedCompoundsWhole) {
  makeParser("fr");
  feedWord("rendez-vous");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "rendez-vous");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsLexicalizedCompoundsWholeWithTrailingPunctuation) {
  makeParser("fr");
  feedWord("rendez-vous.");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "rendez-vous.");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsSecondLexicalizedCompoundWhole) {
  makeParser("fr");
  // Would otherwise match the euphonic "-t-on" pattern (verb "dira" + pronoun "on").
  feedWord("qu'en-dira-t-on");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "qu'en-dira-t-on");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, KeepsOrdinaryCompoundsWhole) {
  makeParser("fr");
  feedWord("grand-mère");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "grand-mère");
}

TEST_F(ChapterHtmlSlimParserFrenchInversionTest, DoesNotSplitInNonFrenchBooks) {
  makeParser("en");
  feedWord("songeai-je");

  ASSERT_EQ(parser->currentTextBlock->size(), 1u);
  EXPECT_EQ(parser->currentTextBlock->wordAt(0), "songeai-je");
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
  std::vector<std::unique_ptr<TextBlock>> lines;

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

  void layout(const float lineCompression = 1.0f) {
    parser->currentTextBlock->layoutAndExtractLines(
        renderer, 0, static_cast<uint16_t>(renderer.getScreenWidth()),
        [this](std::unique_ptr<TextBlock> line, uint32_t) { lines.push_back(std::move(line)); }, true, lineCompression);
    stubLineWords.clear();
    stubLineXPos.clear();
    for (const auto& line : lines) captureLine(*line);
  }
};

// The reserved column is N line ADVANCES tall, and the advance is the font's leading times the
// reader's line-spacing factor. Sizing the glyph against the raw leading made it taller than the
// lines it reserved, so its foot ran through the first full-width line below them.
TEST_F(DropCapTest, SizesTheLetterAgainstTheCompressedLineAdvance) {
  makeParser("p::first-letter { font-size: 300%; }\n");
  openParagraph();
  feedParagraph(60);
  layout(0.5f);
  ASSERT_GT(lines.size(), 4u);

  const auto& cap = lines[0]->getDropCap();
  ASSERT_TRUE(cap.present());
  EXPECT_EQ(cap.scale, (LINE_HEIGHT / 2 * 3) / GLYPH_INK);
}

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
  // No column was reserved, so the line keeps the paragraph's ordinary first-line indent: the
  // default paragraphIndentSpaces, two spaces.
  EXPECT_EQ(stubLineXPos[0][0], SPACE_WIDTH * 2);
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
  ASSERT_EQ(parser.currentTextBlock->wordAt(0), "Before");
  ASSERT_EQ(parser.currentTextBlock->wordAt(1), "After");
}

TEST_F(ChapterHtmlSlimParserTest, DivWithHiddenAttributeContentShouldBeSkipped) {
  const XML_Char* attributes[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "div", attributes);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
}

// visibleTextOffset is the reading position KOReader sync resolves against, so it must count
// only text that actually reaches the layout. Hidden content is skipped by the renderer; if it
// still advanced the counter, every page after a hidden block would resolve to a later page than
// the text the reader can see.
TEST_F(ChapterHtmlSlimParserTest, HiddenTextDoesNotAdvanceTheVisibleOffset) {
  const XML_Char* hidden[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  parser.insideBody = true;

  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "abc", 3);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  const uint32_t afterVisible = parser.visibleTextOffset;
  ASSERT_EQ(afterVisible, 3u) << "visible text must be counted";

  ChapterHtmlSlimParser::startElement(&parser, "p", hidden);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  EXPECT_EQ(parser.visibleTextOffset, afterVisible) << "hidden text must not advance the offset";

  // ...and the counter resumes from the visible total, not from a position inflated by the
  // hidden run, so the next page's recorded offset still points at rendered text.
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "de", 2);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  EXPECT_EQ(parser.visibleTextOffset, afterVisible + 2);
}

TEST_F(ChapterHtmlSlimParserTest, HiddenSubtreeContentDoesNotAdvanceTheVisibleOffset) {
  const XML_Char* hidden[] = {"hidden", "hidden", nullptr};

  parser.beginParse();
  parser.insideBody = true;

  // Nested inside the hidden element: the skip covers the whole subtree, not just its own text.
  ChapterHtmlSlimParser::startElement(&parser, "div", hidden);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "buried", 6);
  ChapterHtmlSlimParser::endElement(&parser, "p");
  ChapterHtmlSlimParser::endElement(&parser, "div");

  EXPECT_EQ(parser.visibleTextOffset, 0u);
}

// HTML attribute names are case-insensitive, and real books do write HIDDEN.
TEST_F(ChapterHtmlSlimParserTest, UppercaseHiddenAttributeIsAlsoSkipped) {
  const XML_Char* attributes[] = {"HIDDEN", "HIDDEN", nullptr};

  parser.beginParse();
  parser.insideBody = true;
  ChapterHtmlSlimParser::startElement(&parser, "p", attributes);
  ChapterHtmlSlimParser::characterData(&parser, "[HIDDEN]", 8);

  ASSERT_EQ(parser.partWordBufferIndex, 0);
  EXPECT_EQ(parser.visibleTextOffset, 0u);
}

TEST_F(ChapterHtmlSlimParserTest, PassesIndentSettingsToNewTextBlock) {
  for (bool extraSpacing : {false, true}) {
    parser.extraParagraphSpacing = extraSpacing;
    parser.currentTextBlock.reset();
    parser.setParagraphIndentSpaces(5);
    parser.startNewTextBlock(BlockStyle());
    ASSERT_NE(parser.currentTextBlock, nullptr);
    EXPECT_EQ(parser.currentTextBlock->paragraphIndentSpaces, 5);
  }
}

}  // namespace

// The x position of the first word of the line just emitted.
static int16_t firstWordX(const TextBlock& line) { return line.wordXpos(0); }

TEST(ParagraphIndentation, OverridesNonnegativeCssAndPreservesHangingIndent) {
  GfxRenderer renderer;
  for (int cssIndent : {-6, 0, 13}) {
    for (uint8_t spaces : {0, 1, 2, 5}) {
      BlockStyle style;
      style.alignment = CssTextAlign::Left;
      style.textIndentDefined = true;
      style.textIndent = cssIndent;
      ParsedText text(false, false, style, spaces);
      text.addWord("word", EpdFontFamily::REGULAR);
      bool sawLine = false;
      text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) {
        sawLine = true;
        EXPECT_EQ(firstWordX(*line), cssIndent < 0 ? cssIndent : 4 * spaces);
      });
      EXPECT_TRUE(sawLine);
    }
  }
  for (uint8_t spaces : {0, 2}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    ParsedText text(false, false, style, spaces);
    text.addWord("word", EpdFontFamily::REGULAR);
    text.layoutAndExtractLines(
        renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) { EXPECT_EQ(firstWordX(*line), 4 * spaces); });
  }
}

TEST(ParagraphIndentation, PreservesAlignmentEligibilityAndScaledSpaceRounding) {
  GfxRenderer renderer;
  for (const auto alignment : {CssTextAlign::Left, CssTextAlign::Center}) {
    for (uint8_t spaces : {0, 2}) {
      BlockStyle style;
      style.alignment = alignment;
      style.textIndentDefined = true;
      style.textIndent = 0;
      ParsedText text(false, false, style, spaces);
      text.addWord("word", EpdFontFamily::REGULAR);
      text.layoutAndExtractLines(renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) {
        if (alignment == CssTextAlign::Left)
          EXPECT_EQ(firstWordX(*line), 4 * spaces);
        else
          EXPECT_EQ(firstWordX(*line), 84);
      });
    }
  }
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  ParsedText text(false, false, style, 2);
  text.addWord("word", EpdFontFamily::REGULAR);
  text.layoutAndExtractLines(
      renderer, 0, 200, [&](std::unique_ptr<TextBlock> line, auto) { EXPECT_EQ(firstWordX(*line), 6); }, true,
      /*lineCompression=*/1.0f, /*characterSpacing=*/0, /*wordSpacingPercent=*/75);
}

TEST(ParagraphIndentation, ReducesOnlyFirstLineAvailableWidth) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  for (uint8_t spaces : {1, 2, 5}) {
    ParsedText text(false, false, style, spaces);
    text.addWord("ab", EpdFontFamily::REGULAR);
    text.addWord("cd", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    text.layoutAndExtractLines(renderer, 0, 40, [&](std::unique_ptr<TextBlock>, auto) { ++lines; });
    EXPECT_EQ(lines, spaces == 1 ? 1u : 2u);
  }
}

TEST(ParagraphIndentation, PreviewKeyTracksOffAndWidths) {
  textsettings::PreviewKey off;
  EXPECT_EQ(off.paragraphIndentSpaces, 2);
  off.paragraphIndentSpaces = 0;
  auto on = off;
  on.paragraphIndentSpaces = 5;
  EXPECT_NE(off, on);
  on.paragraphIndentSpaces = 2;
  EXPECT_NE(off, on);
}

TEST(ParagraphIndentation, MigratesLegacySettingsAndClampsWidths) {
  EXPECT_EQ(migrateParagraphIndentSpaces(false, 0, true), 0);
  EXPECT_EQ(migrateParagraphIndentSpaces(false, 0, false), 2);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, 0, false), 0);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, 2, true), 2);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, 5, false), 5);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, -1, false), 0);
  EXPECT_EQ(migrateParagraphIndentSpaces(true, 300, false), 5);
}

TEST(TextSpacingLayout, TrackingSeparatesCjkTokensAndScalesWordSpaces) {
  GfxRenderer renderer;
  for (bool hyphenation : {false, true}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(hyphenation, false, style, 0);
    text.addWord("一二三", EpdFontFamily::REGULAR);
    text.addWord("四五", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    std::vector<int16_t> xpos;
    text.layoutAndExtractLines(
        renderer, 0, 200,
        [&](std::unique_ptr<TextBlock> block, auto) {
          ++lines;
          for (uint16_t i = 0; i < block->wordCount(); ++i) xpos.push_back(block->wordXpos(i));
        },
        true, 1.0f, -1, 150);
    EXPECT_EQ(lines, 1u);
    // 8 px glyph with -1 px tracking, and a 4 px space scaled to 150% between the two tokens.
    EXPECT_EQ(xpos, (std::vector<int16_t>{0, 7, 14, 28, 35}));
  }
  EXPECT_EQ(renderer.getTextAdvanceX(0, "ab", EpdFontFamily::REGULAR), 16);
  EXPECT_EQ(renderer.getSpaceWidth(0, EpdFontFamily::REGULAR), 4);
}

TEST(TextSpacingLayout, WordSpacingChangesWrapThreshold) {
  GfxRenderer renderer;
  for (uint8_t percent : {50, 100, 125, 200}) {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(false, false, style, 0);
    text.addWord("ab", EpdFontFamily::REGULAR);
    text.addWord("cd", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    text.layoutAndExtractLines(
        renderer, 0, 36, [&](std::unique_ptr<TextBlock>, auto) { ++lines; }, true, 1.0f, 0, percent);
    EXPECT_EQ(lines, percent > 100 ? 2u : 1u);  // 16 + 16 + scaled 4 px space
  }
}

TEST(TextSpacingLayout, CachedPageRestoresSpacing) {
  // Character spacing is a reader-wide setting here (part of the ReaderRenderSpec the section
  // header validates), not a per-block property: TextBlock serializes the CSS letter-spacing only.
  GTEST_SKIP() << "this fork keeps character spacing in the section header, not in each block";

  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.textIndentDefined = true;
  ParsedText text(false, false, style);
  text.addWord("一二三", EpdFontFamily::REGULAR);
  text.addWord("四五", EpdFontFamily::REGULAR);
  const auto path = (std::filesystem::temp_directory_path() / "crosspoint-text-spacing.bin").string();
  unsigned lines = 0;
  text.layoutAndExtractLines(
      renderer, 0, 200,
      [&](std::unique_ptr<TextBlock> line, auto) {
        ++lines;
        Page page;
        page.elements.push_back(std::make_unique<PageLine>(std::move(line), 4, 12));
        const auto* original = static_cast<const PageLine&>(*page.elements[0]).getBlock();
        {
          HalFile file;
          ASSERT_TRUE(file.open(path.c_str(), "wb"));
          ASSERT_TRUE(page.serialize(file));
        }
        HalFile file;
        ASSERT_TRUE(file.open(path.c_str(), "rb"));
        auto cachedPage = Page::deserialize(file);
        ASSERT_NE(cachedPage, nullptr);
        ASSERT_EQ(cachedPage->elements.size(), 1);
        const auto* cached = static_cast<const PageLine&>(*cachedPage->elements[0]).getBlock();
        ASSERT_NE(cached, nullptr);
        EXPECT_EQ(cached->getBlockStyle().characterSpacing, -2);
        ASSERT_EQ(cached->wordCount(), 5);
        EXPECT_EQ(cached->wordXpos(3) - cached->wordXpos(2), 10);  // 8 + half-width space
        EXPECT_EQ(file.position(), file.size());
        ASSERT_EQ(cached->wordCount(), original->wordCount());
        for (uint16_t i = 0; i < original->wordCount(); ++i) EXPECT_EQ(cached->wordXpos(i), original->wordXpos(i));
      },
      true, 1.0f, -2, 50);
  EXPECT_EQ(lines, 1u);
  std::filesystem::remove(path);
}

TEST_F(ChapterHtmlSlimParserTest, ParserAppliesTextSpacingToParagraphs) {
  parser.setTextSpacing(-1, 150);
  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  const std::string text = "\xe4\xb8\x80\xe4\xba\x8c\xe4\xb8\x89 \xe5\x9b\x9b\xe4\xba\x94";  // 一二三 四五
  ChapterHtmlSlimParser::characterData(&parser, text.c_str(), static_cast<int>(text.size()));
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  unsigned lines = 0;
  std::vector<int16_t> xpos;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = *static_cast<const PageLine&>(*element).getBlock();
    ++lines;
    EXPECT_EQ(block.getBlockStyle().characterSpacing, -1);
    for (uint16_t i = 0; i < block.wordCount(); ++i) xpos.push_back(block.wordXpos(i));
  }
  EXPECT_EQ(lines, 1u);
  ASSERT_EQ(xpos.size(), 5u);
  EXPECT_EQ(xpos[1] - xpos[0], 7);   // 8 px glyph, -1 px tracking
  EXPECT_EQ(xpos[3] - xpos[2], 14);  // glyph plus 150% of a 4 px space
}

TEST_F(ChapterHtmlSlimParserTest, NfdWordAcrossInlineStyleKeepsSourceRange) {
  parser.beginParse();
  ChapterHtmlSlimParser::startElement(&parser, "body", nullptr);
  ChapterHtmlSlimParser::startElement(&parser, "p", nullptr);
  const std::string prefix = "Cafe\xCC\x81";
  ChapterHtmlSlimParser::characterData(&parser, prefix.c_str(), static_cast<int>(prefix.size()));
  ChapterHtmlSlimParser::startElement(&parser, "b", nullptr);
  ChapterHtmlSlimParser::characterData(&parser, "s", 1);
  ChapterHtmlSlimParser::endElement(&parser, "b");
  ChapterHtmlSlimParser::endElement(&parser, "p");
  parser.makePages();
  ASSERT_NE(parser.currentPage, nullptr);
  std::string selected;
  uint32_t previousEnd = 0;
  unsigned words = 0;
  for (const auto& element : parser.currentPage->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto& block = *static_cast<const PageLine&>(*element).getBlock();
    for (uint16_t i = 0; i < block.wordCount(); ++i) {
      const auto range = block.wordSourceRange(i);
      EXPECT_EQ(range.start, words == 0 ? 0u : 5u);
      EXPECT_EQ(range.end, words == 0 ? 5u : 6u);
      ASSERT_TRUE(clippingText::append(selected, block.wordText(i), range.start > previousEnd ? ' ' : '\0', 4096,
                                       block.wordHasDiscretionaryHyphen(i)));
      previousEnd = range.end;
      ++words;
    }
  }
  EXPECT_EQ(words, 2u);
  EXPECT_EQ(selected, "Caf\xC3\xA9s");
}

class ClippingParagraphTest : public ChapterHtmlSlimParserTest {};

TEST_P(ClippingParagraphTest, CachedBlocksSeparateAdjacentParagraphs) {
  const std::string html = std::string("<html><body>") + GetParam() + "</body></html>";
  const std::string testId = std::to_string(std::hash<std::string>{}(html));
  const auto tempDir = std::filesystem::temp_directory_path();
  filepath = (tempDir / ("crosspoint-clipping-paragraphs-" + testId + ".xhtml")).string();
  {
    HalFile file;
    ASSERT_TRUE(file.open(filepath.c_str(), "wb"));
    ASSERT_EQ(file.write(html.data(), html.size()), html.size());
  }
  const auto cachePath = (tempDir / ("crosspoint-clipping-paragraphs-" + testId + ".bin")).string();
  std::string selected;
  uint32_t previousEnd = 0;
  bool paragraphStartPending = false;
  parser.completePageFn = [&](std::unique_ptr<Page> page, auto, auto, auto) {
    {
      HalFile file;
      ASSERT_TRUE(file.open(cachePath.c_str(), "wb"));
      ASSERT_TRUE(page->serialize(file));
    }
    HalFile file;
    ASSERT_TRUE(file.open(cachePath.c_str(), "rb"));
    auto cached = Page::deserialize(file);
    ASSERT_NE(cached, nullptr);
    for (const auto& element : cached->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& block = *static_cast<const PageLine&>(*element).getBlock();
      for (uint16_t i = 0; i < block.wordCount(); ++i) {
        if (block.wordStartsParagraph(i)) paragraphStartPending = true;
        if (!clippingText::hasVisibleText(block.wordText(i))) continue;
        const auto range = block.wordSourceRange(i);
        const char separator = paragraphStartPending ? '\n' : range.start > previousEnd ? ' ' : '\0';
        ASSERT_TRUE(clippingText::append(selected, block.wordText(i), separator, 4096));
        paragraphStartPending = false;
        previousEnd = range.end;
      }
    }
    EXPECT_EQ(file.position(), file.size());
  };
  parser.paragraphIndentSpaces = 0;
  ASSERT_TRUE(parser.parseAndBuildPages());
  EXPECT_EQ(selected, "one\ntwo");
  std::filesystem::remove(filepath);
  std::filesystem::remove(cachePath);
}

INSTANTIATE_TEST_SUITE_P(ClippingBoundaries, ClippingParagraphTest,
                         ::testing::Values("<p>one</p><p>two</p>", "<p>one</p>\n  <p>two</p>",
                                           "<div>one</div><div>two</div>", "<div>one</div>\n  <div>two</div>",
                                           "<p>one<br/>two</p>", "<p>o<b>ne</b></p><p>two</p>",
                                           "<p>one</p><p>&#160;two</p>", "<p>one</p><p>&#8239;two</p>"));

TEST(TextSpacingLayout, ParagraphMarkerSurvivesBidiAndOnlyMarksFirstExtractedLine) {
  GfxRenderer renderer;
  for (const bool focus : {false, true}) {
    BlockStyle style;
    style.isRtl = true;
    style.directionDefined = true;
    style.alignment = CssTextAlign::Right;
    ParsedText text(false, focus, style, 0);
    text.addWord("אחד", EpdFontFamily::REGULAR);
    text.addWord("alpha", EpdFontFamily::REGULAR);
    text.addWord("beta", EpdFontFamily::REGULAR);
    text.addWord("שני", EpdFontFamily::REGULAR);
    unsigned lines = 0;
    unsigned starts = 0;
    auto inspect = [&](std::unique_ptr<TextBlock> block, auto) {
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        if (!block->wordStartsParagraph(i)) continue;
        EXPECT_EQ(lines, 0u);
        EXPECT_STREQ(block->wordText(i), "אחד");
        ++starts;
      }
      ++lines;
    };
    text.layoutAndExtractLines(renderer, 0, 100, inspect, false);
    ASSERT_GT(lines, 0u);
    ASSERT_GT(text.size(), 0u);
    text.layoutAndExtractLines(renderer, 0, 100, inspect);
    EXPECT_EQ(starts, 1u);
    EXPECT_GT(lines, 1u);
  }
}

TEST(KoreanLayout, HangulWordsStayWholeAndWrapAtSpaces) {
  GfxRenderer renderer;
  {
    BlockStyle style;
    style.alignment = CssTextAlign::Left;
    style.textIndentDefined = true;
    ParsedText text(false, false, style, 0);
    text.addWord("가나다", EpdFontFamily::REGULAR);
    text.addWord("라마", EpdFontFamily::REGULAR);
    text.addWord("3개를", EpdFontFamily::REGULAR);
    text.addWord("iPhone을", EpdFontFamily::REGULAR);
    stubLineWords.clear();
    text.layoutAndExtractLines(renderer, 0, 60, [](std::unique_ptr<TextBlock> block, auto) { captureLine(*block); });
    // 가나다 라마 is 24 + 4 + 16 px; adding 3개를 would need 72 px, and no break exists inside it.
    const std::vector<std::vector<std::string>> expected{{"가나다", "라마"}, {"3개를"}, {"iPhone을"}};
    EXPECT_EQ(stubLineWords, expected);
  }
}

TEST(KoreanLayout, JustifiedHangulStretchesOnlyWordSpaces) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(false, false, style, 0);
  for (const char* word : {"가나", "다라", "마바", "사아"}) text.addWord(word, EpdFontFamily::REGULAR);
  unsigned lines = 0;
  stubLineXPos.clear();
  text.layoutAndExtractLines(renderer, 0, 60, [&](std::unique_ptr<TextBlock> block, auto) {
    lines++;
    captureLine(*block);
  });
  EXPECT_EQ(lines, 2u);
  // 3 x 16 px words + 2 x 4 px spaces leave 4 px, split across the two spaces only.
  ASSERT_FALSE(stubLineXPos.empty());
  EXPECT_EQ(stubLineXPos[0], (std::vector<int16_t>{0, 22, 44}));
}

TEST(KoreanLayout, HangulGluedAcrossInlineStyleIsUnbreakable) {
  GfxRenderer renderer;
  BlockStyle style;
  style.alignment = CssTextAlign::Justify;
  style.textIndentDefined = true;
  ParsedText text(false, false, style);
  text.addWord("가나", EpdFontFamily::REGULAR);
  text.addWord("한국", EpdFontFamily::REGULAR);
  text.addWord("어", EpdFontFamily::BOLD, false, /*attachToPrevious=*/true);
  stubLineWords.clear();
  text.layoutAndExtractLines(renderer, 0, 40, [](std::unique_ptr<TextBlock> block, auto) { captureLine(*block); });
  // 가나 한국 fits in 36 px, but 어 is glued to 한국, so the whole word moves down.
  const std::vector<std::vector<std::string>> expected{{"가나"}, {"한국", "어"}};
  EXPECT_EQ(stubLineWords, expected);
}

TEST(XhtmlDoctypeTest, FeedsADoctypeWhereTheChapterNamesNoDtd) {
  const auto patch = [](const char* text) { return xhtml::findDoctypePatch(text, std::strlen(text)); };

  // No DOCTYPE: fed right before the root element, after the XML declaration.
  auto p = patch("<?xml version=\"1.0\"?>\n<html><body>&nbsp;</body></html>");
  EXPECT_TRUE(p.inject);
  EXPECT_EQ(p.keep, 22u);
  EXPECT_EQ(p.skip, 0u);

  // UTF-16, with a BOM or without: an ASCII DOCTYPE would corrupt it, so it is left alone.
  const char utf16le[] = "\xFF\xFE<\0h\0t\0m\0l\0>\0";
  EXPECT_FALSE(xhtml::findDoctypePatch(utf16le, sizeof(utf16le) - 1).inject);
  const char utf16be[] = "\xFE\xFF\0<\0h\0t\0m\0l\0>";
  EXPECT_FALSE(xhtml::findDoctypePatch(utf16be, sizeof(utf16be) - 1).inject);
  const char utf16NoBom[] = "<\0h\0t\0m\0l\0>\0";
  EXPECT_FALSE(xhtml::findDoctypePatch(utf16NoBom, sizeof(utf16NoBom) - 1).inject);

  // HTML5's bare DOCTYPE is replaced.
  p = patch("<!DOCTYPE html><html/>");
  EXPECT_TRUE(p.inject);
  EXPECT_EQ(p.keep, 0u);
  EXPECT_EQ(p.skip, 15u);
  p = patch("<!-- c --><!doctype html>\n<html/>");
  EXPECT_TRUE(p.inject);
  EXPECT_EQ(p.keep, 10u);
  EXPECT_EQ(p.skip, 15u);

  // A DOCTYPE that names a DTD, or carries its own declarations, is the document's business.
  EXPECT_FALSE(patch("<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"x.dtd\"><html/>").inject);
  EXPECT_FALSE(patch("<!DOCTYPE html SYSTEM \"x.dtd\"><html/>").inject);
  EXPECT_FALSE(patch("<!DOCTYPE html [<!ENTITY nbsp \"&#160;\">]><html/>").inject);

  // A prolog that runs past the first read cannot be judged.
  EXPECT_FALSE(patch("<?xml version=\"1.0\"").inject);
  EXPECT_FALSE(patch("<!DOCTYPE html").inject);
  EXPECT_FALSE(patch("  \n").inject);
}
