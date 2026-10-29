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
