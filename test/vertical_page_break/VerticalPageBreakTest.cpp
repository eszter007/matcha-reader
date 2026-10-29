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
  EXPECT_EQ(firstGlyphOf(pages[0], U'。')->column, 4);
  const VerticalGlyph* head = firstGlyphOf(pages[1], U'今');
  ASSERT_NE(head, nullptr);
  EXPECT_EQ(head->column, 0);
  EXPECT_EQ(head->row, 0);
}

TEST(VerticalPageBreak, ParagraphMidPageStillStartsFreshColumn) {
  const auto pages = layout({"あいう。", "今やら"});
  ASSERT_EQ(pages.size(), 1u);
  EXPECT_EQ(firstGlyphOf(pages[0], U'今')->column, 1);
  EXPECT_EQ(firstGlyphOf(pages[0], U'今')->row, 0);
}
