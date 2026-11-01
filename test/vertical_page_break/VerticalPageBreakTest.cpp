#include <Epub/VerticalParsedText.h>
#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

// 100x200 viewport with the fixture's 20px em: 5 columns of 10 rows per page.
constexpr uint16_t VIEWPORT_W = 100;
constexpr uint16_t VIEWPORT_H = 200;
constexpr char TEN[] = "あいうえおかきくけこ";

std::vector<VerticalPage> layout(const std::vector<std::string>& paragraphs) {
  GfxRenderer renderer;
  VerticalParsedText text(renderer, 0, VIEWPORT_W, VIEWPORT_H);
  for (const auto& p : paragraphs) text.addParagraph(p);
  return text.layoutPages();
}

const VerticalGlyph* firstGlyphOf(const VerticalPage& page, const uint32_t cp) {
  for (const auto& g : page.glyphs) {
    if (g.codepoint == cp) return &g;
  }
  return nullptr;
}

std::string fullPage(const char* lastColumn) { return std::string(TEN) + TEN + TEN + TEN + lastColumn; }

}  // namespace

TEST(VerticalPageBreak, ParagraphAfterFullLastColumnStartsInFirstColumn) {
  const auto pages = layout({fullPage("人物なのだかきくけ。"), "今やらなければ"});
  ASSERT_EQ(pages.size(), 2u);
  ASSERT_EQ(pages[0].columnCount, 5);
  ASSERT_EQ(pages[0].rowsPerColumn, 10);
  const VerticalGlyph* head = firstGlyphOf(pages[1], U'今');
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(head->column, 0);
  EXPECT_EQ(head->row, 0);
}

TEST(VerticalPageBreak, ParagraphAfterHangingFullStopStartsInFirstColumn) {
  // The 。 hangs past the foot of the page's last column (burasage).
  const auto pages = layout({fullPage("人物なのだかきくけこ。"), "今やらなければ"});
  ASSERT_EQ(pages.size(), 2u);
  const VerticalGlyph* stop = firstGlyphOf(pages[0], U'。');
  ASSERT_NE(stop, nullptr);
  EXPECT_EQ(stop->column, 4);
  const VerticalGlyph* head = firstGlyphOf(pages[1], U'今');
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(head->column, 0);
  EXPECT_EQ(head->row, 0);
}

TEST(VerticalPageBreak, ParagraphMidPageStillStartsFreshColumn) {
  const auto pages = layout({"あいう。", "今やら"});
  ASSERT_EQ(pages.size(), 1u);
  const VerticalGlyph* head = firstGlyphOf(pages[0], U'今');
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(head->column, 1);
  EXPECT_EQ(head->row, 0);
}

TEST(VerticalPageBreak, ShortenedColumnSpreadsAtMostAnEighthEmPerGap) {
  // た。…… may not be split, so all three move on and leave column 0 three rows short.
  const auto pages = layout({"あいうえおかきた。……と", std::string(TEN)});
  ASSERT_FALSE(pages.empty());
  std::vector<int> ys;
  for (const auto& g : pages[0].glyphs) {
    if (g.column == 0) ys.push_back(g.y);
  }
  ASSERT_EQ(ys.size(), 7u);
  int fullPitch = 0;  // pitch of the full column 2
  for (const auto& g : pages[0].glyphs) {
    if (g.column == 2 && g.row == 1) fullPitch = g.y;
  }
  ASSERT_GT(fullPitch, 0);
  constexpr int EM = 20;
  for (size_t i = 1; i < ys.size(); i++)
    EXPECT_LE(ys[i] - ys[i - 1], fullPitch + EM / 8 + 1) << "gap " << i;  // +1: rounding
}

TEST(VerticalPageBreak, OpeningBracketNeverEndsAColumn) {
  // Each case puts 「 on the last row of the first column.
  for (const char* paragraph : {"あいうえおかきくけ「こ」", "あいうえおかきく。「こ」", "あいうえおかきく、「こ」",
                                "あいうえおかき。「こ」さしすせそ", "あいうえおかきくけこさしすせそたちつ。「て」"}) {
    const auto pages = layout({paragraph});
    ASSERT_FALSE(pages.empty());
    for (const auto& g : pages[0].glyphs) {
      if (g.codepoint != U'「') continue;
      const bool lastRow = g.row + 1 >= pages[0].rowsPerColumn;
      EXPECT_FALSE(lastRow) << paragraph << ": 「 at column " << g.column << " row " << g.row;
    }
  }
}

TEST(VerticalPageBreak, OpeningBracketMovesWithRubyGroupThatBreaksColumn) {
  // 「 lands on row 7; the ruby group after it needs three rows, so it moves to the next column and
  // must take the bracket with it (device photo: 「 left at the foot before 養父母).
  GfxRenderer renderer;
  VerticalParsedText text(renderer, 0, VIEWPORT_W, VIEWPORT_H);
  text.setRubyShown(true);
  VerticalParsedText::RubyRun before;
  before.baseText = "あいうえおかさ「";
  VerticalParsedText::RubyRun ruby;
  ruby.baseText = "養父母";
  ruby.rubyText = "ふたり";
  VerticalParsedText::RubyRun after;
  after.baseText = "のことは";
  text.addAnnotatedParagraph({before, ruby, after});
  const auto pages = text.layoutPages();
  ASSERT_FALSE(pages.empty());
  const VerticalGlyph* bracket = firstGlyphOf(pages[0], U'「');
  const VerticalGlyph* base = firstGlyphOf(pages[0], U'養');
  ASSERT_NE(bracket, nullptr);
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(bracket->column, base->column);
  EXPECT_EQ(bracket->row + 1, base->row);
}

TEST(VerticalPageBreak, OpeningBracketFollowsRubyGroupOntoTheNextPage) {
  GfxRenderer renderer;
  VerticalParsedText text(renderer, 0, VIEWPORT_W, VIEWPORT_H);
  text.setRubyShown(true);
  VerticalParsedText::RubyRun before;
  before.baseText = std::string(TEN) + TEN + TEN + TEN + "あいうえおかさ「";
  VerticalParsedText::RubyRun ruby;
  ruby.baseText = "養父母";
  ruby.rubyText = "ふたり";
  text.addAnnotatedParagraph({before, ruby});
  const auto pages = text.layoutPages();
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(firstGlyphOf(pages[0], U'「'), nullptr);
  const VerticalGlyph* bracket = firstGlyphOf(pages[1], U'「');
  ASSERT_NE(bracket, nullptr);
  EXPECT_EQ(bracket->column, 0);
  EXPECT_EQ(bracket->row, 0);
}

namespace {

// One styled block holding two paragraphs ("あい", "うえお"), ending with the chapter.
std::vector<VerticalPage> layoutBlock(const VerticalBlockParams& params) {
  GfxRenderer renderer;
  VerticalParsedText text(renderer, 0, VIEWPORT_W, VIEWPORT_H);
  text.markBlockStart(params);
  text.addParagraph("あい");
  text.addParagraph("うえお");
  text.markBlockEnd();
  return text.layoutPages(nullptr, nullptr, true);
}

}  // namespace

TEST(VerticalPageBreak, BlockCentredAlongTheColumnKeepsItsColumnsStartingTogether) {
  VerticalBlockParams params;
  params.blockAlign = VerticalBlockParams::BlockAlign::Center;
  const auto pages = layoutBlock(params);
  ASSERT_EQ(pages.size(), 1u);
  // Three rows used of ten: the block moves down (10 - 3) / 2 = 3 rows, both columns alike.
  EXPECT_EQ(firstGlyphOf(pages[0], U'あ')->y, 3 * 20);
  EXPECT_EQ(firstGlyphOf(pages[0], U'う')->y, 3 * 20);
}

TEST(VerticalPageBreak, BlockPushedToTheFootEndsWithItsLongestColumn) {
  VerticalBlockParams params;
  params.blockAlign = VerticalBlockParams::BlockAlign::End;
  const auto pages = layoutBlock(params);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(firstGlyphOf(pages[0], U'う')->y, 7 * 20);
  EXPECT_EQ(firstGlyphOf(pages[0], U'あ')->y, 7 * 20);
}

TEST(VerticalPageBreak, EndAlignedColumnsEachEndAtTheFoot) {
  VerticalBlockParams params;
  params.alignEnd = true;
  const auto pages = layoutBlock(params);
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(firstGlyphOf(pages[0], U'い')->y, 9 * 20);
  EXPECT_EQ(firstGlyphOf(pages[0], U'お')->y, 9 * 20);
}

TEST(VerticalPageBreak, EndAlignedFullColumnWithAHangingStopStaysPut) {
  // A framed block's columns are a row short of the foot: nine cells and a 。 hanging past them.
  const auto layoutFramed = [](const bool alignEnd) {
    GfxRenderer renderer;
    VerticalParsedText text(renderer, 0, VIEWPORT_W, VIEWPORT_H);
    VerticalBlockParams params;
    params.borderEdges = 1;
    params.alignEnd = alignEnd;
    text.markBlockStart(params);
    text.addParagraph("人物なのだかきくけ。");
    text.markBlockEnd();
    return text.layoutPages(nullptr, nullptr, true);
  };
  const auto plain = layoutFramed(false);
  const auto aligned = layoutFramed(true);
  ASSERT_EQ(aligned.size(), 1u);
  ASSERT_EQ(plain.size(), 1u);
  const VerticalGlyph* stop = firstGlyphOf(aligned[0], U'。');
  ASSERT_NE(stop, nullptr);
  ASSERT_EQ(stop->column, firstGlyphOf(aligned[0], U'人')->column) << "the 。 must hang in the same column";
  EXPECT_EQ(firstGlyphOf(aligned[0], U'人')->y, firstGlyphOf(plain[0], U'人')->y);
  EXPECT_EQ(firstGlyphOf(aligned[0], U'物')->y, firstGlyphOf(plain[0], U'物')->y);
}
