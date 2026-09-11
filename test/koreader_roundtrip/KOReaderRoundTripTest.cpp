#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ChapterXPathResolver.h"
#include "VisibleTextStreamer.h"

namespace {
std::shared_ptr<Epub> epubWith(std::string xhtml) {
  std::vector<std::string> spine;
  spine.push_back(std::move(xhtml));
  return std::make_shared<Epub>(std::move(spine));
}

// The property that makes KOSync positions survive a round trip: an anchor produced from a visible
// offset must resolve back to that same offset. The upload side (ChapterXPathResolver) and the
// download side (ParagraphStreamer) are independent scanners, so this is the only thing that keeps
// their counting rules honest.
void expectRoundTrip(const std::string& xhtml, const uint32_t offset) {
  const auto epub = epubWith(xhtml);
  const std::string xpath = ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset);
  ASSERT_FALSE(xpath.empty()) << "no anchor for offset " << offset;

  const auto resolved = koreader_sync::resolveXPathVisibleOffset(xhtml, xpath);
  ASSERT_TRUE(resolved.has_value()) << "anchor " << xpath << " did not resolve back";
  EXPECT_EQ(*resolved, offset) << "anchor " << xpath;
}
}  // namespace

TEST(KOReaderRoundTrip, PlainNestedParagraphs) {
  const std::string doc = "<html><body><div><p>Alpha bravo</p><p>Charlie delta</p></div></body></html>";

  expectRoundTrip(doc, 0);
  expectRoundTrip(doc, 6);
  expectRoundTrip(doc, 11);
  expectRoundTrip(doc, 15);
}

TEST(KOReaderRoundTrip, TextAfterInlineMarkup) {
  const std::string doc = "<html><body><p>Second <em>nested</em> tail</p></body></html>";

  expectRoundTrip(doc, 0);
  expectRoundTrip(doc, 8);
  expectRoundTrip(doc, 14);
}

TEST(KOReaderRoundTrip, TextAfterComment) {
  const std::string doc = "<html><body><p>before<!--note-->after</p></body></html>";

  expectRoundTrip(doc, 6);
  expectRoundTrip(doc, 8);
}

TEST(KOReaderRoundTrip, TextAfterProcessingInstruction) {
  const std::string doc = "<html><body><p>before<?marker?>after</p></body></html>";

  expectRoundTrip(doc, 7);
}

TEST(KOReaderRoundTrip, TextInAndAfterCdata) {
  const std::string doc = "<html><body><p>before<![CDATA[middle]]>after</p></body></html>";

  expectRoundTrip(doc, 7);
  expectRoundTrip(doc, 13);
}

TEST(KOReaderRoundTrip, TextAfterHiddenAndPagebreakSubtrees) {
  const std::string doc =
      R"(<html><body><p>Alpha</p><p hidden="">Skipped</p><span epub:type="pagebreak">99</span><p>Bravo</p></body></html>)";

  expectRoundTrip(doc, 5);
  expectRoundTrip(doc, 8);
}

TEST(KOReaderRoundTrip, SurvivesDoctypeAndSelfClosingTags) {
  const std::string doc =
      "<?xml version=\"1.0\"?><!DOCTYPE html><html><body><p>Alpha<br />bravo</p><img src=\"x.png\" />"
      "<p>Charlie</p></body></html>";

  expectRoundTrip(doc, 0);
  expectRoundTrip(doc, 5);
  expectRoundTrip(doc, 10);
}

TEST(KOReaderRoundTrip, CdataContainingBrackets) {
  const std::string doc = "<html><body><p>a<![CDATA[b]c]]>d</p></body></html>";

  expectRoundTrip(doc, 0);
  expectRoundTrip(doc, 4);
}

TEST(KOReaderRoundTrip, NonVisibleSubtreesAreNotCounted) {
  const std::string doc =
      "<html><head><title>Title</title></head><body><style>p{color:red}</style><p>Alpha</p></body></html>";

  expectRoundTrip(doc, 0);
  expectRoundTrip(doc, 4);
}
