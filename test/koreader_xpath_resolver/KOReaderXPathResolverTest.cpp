#include <Epub/VisibleTextUtils.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ChapterXPathResolver.h"

namespace {
std::shared_ptr<Epub> epubWith(std::string xhtml) {
  std::vector<std::string> spine;
  spine.push_back(std::move(xhtml));
  return std::make_shared<Epub>(std::move(spine));
}

constexpr char kNestedFixture[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><div><section><p>Alpha bravo</p><p>Second <em>nested</em> tail</p></section></div></body></html>)";

constexpr char kNonVisibleInlineFixture[] =
    R"(<html><body><p><RP><span>hidden</span></RP>Visible text</p></body></html>)";

constexpr char kCommentBoundaryFixture[] = R"(<html><body><p>before<!--comment-->after</p></body></html>)";
constexpr char kProcessingInstructionBoundaryFixture[] = R"(<html><body><p>before<?marker?>after</p></body></html>)";
constexpr char kCdataBoundaryFixture[] = R"(<html><body><p>before<![CDATA[middle]]>after</p></body></html>)";
constexpr char kHiddenCdataFixture[] = R"(<html><body><p>before<rp><![CDATA[hidden]]></rp>after</p></body></html>)";
}  // namespace

TEST(KOReaderXPathResolver, ResolvesExactOffsetWithFullAncestry) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].6");
}

TEST(KOReaderXPathResolver, PreservesNestedInlineTextNode) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 26),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[2].2");
}

TEST(KOReaderXPathResolver, EmitsDetailedAnchorForOffsetZero) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, IgnoresNestedNonVisibleInlineText) {
  const auto epub = epubWith(kNonVisibleInlineFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, ResolvesProgressAfterNestedNonVisibleInlineText) {
  const auto epub = epubWith(kNonVisibleInlineFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f), "/body/DocFragment[1]/body/p[1]/text()[1].12");
}

TEST(KOReaderXPathResolver, CountsUtf8CodepointsInsteadOfBytes) {
  const auto epub = epubWith(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><html><body><p>A\xC3\xA9\xE4\xB8\xAD"
      "B</p></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 3),
            "/body/DocFragment[1]/body/p[1]/text()[1].3");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundComments) {
  const auto epub = epubWith(kCommentBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/p[1]/text()[2].0");
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 11).empty());
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundProcessingInstructions) {
  const auto epub = epubWith(kProcessingInstructionBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, SplitsTextNodesAroundCdata) {
  const auto epub = epubWith(kCdataBoundaryFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 13),
            "/body/DocFragment[1]/body/p[1]/text()[3].1");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstComment) {
  const auto epub = epubWith(R"(<html><body><p><!--comment-->text</p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstProcessingInstruction) {
  const auto epub = epubWith(R"(<html><body><p><?marker?>text</p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, DoesNotCreateNodesBeforeFirstCdata) {
  const auto epub = epubWith(R"(<html><body><p><![CDATA[text]]></p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, CountsVisibleCdataAndIgnoresHiddenCdata) {
  const auto visible = epubWith(kCdataBoundaryFixture);
  const auto hidden = epubWith(kHiddenCdataFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(visible, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(hidden, 0, 7),
            "/body/DocFragment[1]/body/p[1]/text()[2].1");
}

TEST(KOReaderXPathResolver, ReturnsEmptyForUnusableContent) {
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith(""), 0, 0).empty());
  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epubWith("<html><body><p>broken"), 0, 100).empty());
}

// ChapterHtmlSlimParser counts every visible body text node when it builds the offsets resolved
// here, so heading and bare-div text must resolve too -- restricting resolution to <p>/<li> would
// leave the two counting in different units and shift every anchor after such text.
TEST(KOReaderXPathResolver, ResolvesVisibleTextOutsideParagraphs) {
  const auto epub = epubWith("<html><body><h1>Chapter One</h1><p>Alpha bravo</p></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0),
            "/body/DocFragment[1]/body/h1[1]/text()[1].0");
  // The paragraph anchor sits after the 11 codepoints of the heading.
  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 11),
            "/body/DocFragment[1]/body/p[1]/text()[1].0");
}

TEST(KOReaderXPathResolver, AnchorsTextDirectlyUnderBody) {
  const auto epub = epubWith("<html><body>Bare body text<p>Alpha</p></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 5), "/body/DocFragment[1]/body/text()[1].5");
}

// findXPathForProgress() measures the target against a total; both must count the same text, or
// a percentage resolves to an anchor before the intended one.
TEST(KOReaderXPathResolver, ProgressCountsVisibleTextOutsideParagraphs) {
  // 11 heading codepoints + 5 paragraph codepoints: full progress must reach the paragraph's end,
  // not the point 100% of the paragraph-only total would give.
  const auto epub = epubWith("<html><body><h1>Chapter One</h1><p>Alpha</p></body></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(epub, 0, 1.0f), "/body/DocFragment[1]/body/p[1]/text()[1].5");
}

TEST(KOReaderXPathResolver, ResolvesInsideUppercaseBody) {
  const auto epub = epubWith("<html><BODY><p>Alpha bravo</p></BODY></html>");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 6),
            "/body/DocFragment[1]/body/p[1]/text()[1].6");
}

TEST(KOReaderXPathResolver, SkipsPagebreakSubtrees) {
  // The layout parser skips pagebreak markers, so their text must not advance the offset.
  const auto epub =
      epubWith(R"(<html><body><p>Alpha</p><span epub:type="pagebreak">99</span><p>Bravo</p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 5),
            "/body/DocFragment[1]/body/p[2]/text()[1].0");
}

TEST(KOReaderXPathResolver, SkipsHiddenSubtrees) {
  // The layout parser treats the HTML hidden attribute as display:none and leaves its text out of
  // visibleTextOffset, so the resolver must not count it either.
  const auto epub = epubWith(R"(<html><body><p>Alpha</p><p hidden="">Skipped</p><p>Bravo</p></body></html>)");

  EXPECT_EQ(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 5),
            "/body/DocFragment[1]/body/p[3]/text()[1].0");
}

TEST(KOReaderXPathResolver, TreatsHiddenBodyAsInvisible) {
  const auto epub = epubWith(R"(<html><body hidden=""><p>Alpha</p></body></html>)");

  EXPECT_TRUE(ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, 0).empty());
}

// The shared rule every offset counter uses: the layout parsers, the resolver above and
// ProgressMapper's reverse streamer must agree on which subtrees are skipped.
TEST(VisibleTextUtils, SkippedSubtreeAttributeRule) {
  EXPECT_TRUE(VisibleTextUtils::isSkippedSubtreeAttribute("hidden", ""));
  EXPECT_TRUE(VisibleTextUtils::isSkippedSubtreeAttribute("HIDDEN", "hidden"));
  EXPECT_TRUE(VisibleTextUtils::isSkippedSubtreeAttribute("role", "doc-pagebreak"));
  EXPECT_TRUE(VisibleTextUtils::isSkippedSubtreeAttribute("epub:type", "pagebreak"));

  EXPECT_FALSE(VisibleTextUtils::isSkippedSubtreeAttribute("class", "hidden"));
  EXPECT_FALSE(VisibleTextUtils::isSkippedSubtreeAttribute("role", "doc-chapter"));
  EXPECT_FALSE(VisibleTextUtils::isSkippedSubtreeAttribute("epub:type", "footnote"));
  EXPECT_FALSE(VisibleTextUtils::isSkippedSubtreeAttribute("id", "pagebreak"));
}

TEST(KOReaderXPathResolver, KeepsParagraphOnlyResolutionUnchanged) {
  const auto epub = epubWith(kNestedFixture);

  EXPECT_EQ(ChapterXPathResolver::findXPathForParagraph(epub, 0, 2),
            "/body/DocFragment[1]/body/div[1]/section[1]/p[2]");
}
