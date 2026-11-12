#include "AnkiHtml.h"

#include <expat.h>
#include <gtest/gtest.h>

#include <string>

namespace {

constexpr size_t MAX = 1 << 20;

std::string conv(std::string_view html) {
  std::string out;
  EXPECT_TRUE(ankihtml::toXhtml(html, out, MAX));
  return out;
}

// Same parser setup as the chapter parser: non-namespaced, no DTD.
bool parsesAsXml(const std::string& fragment) {
  const std::string doc = "<div>" + fragment + "</div>";
  XML_Parser p = XML_ParserCreate(nullptr);
  const bool ok = XML_Parse(p, doc.data(), static_cast<int>(doc.size()), 1) == XML_STATUS_OK;
  if (!ok) ADD_FAILURE() << XML_ErrorString(XML_GetErrorCode(p)) << " in: " << doc;
  XML_ParserFree(p);
  return ok;
}

std::string convChecked(std::string_view html) {
  std::string out = conv(html);
  EXPECT_TRUE(parsesAsXml(out));
  return out;
}

TEST(AnkiHtml, VoidElementsSelfClosed) {
  EXPECT_EQ(convChecked("a<br>b<hr><wbr><BR>"), "a<br/>b<hr/><wbr/><br/>");
  EXPECT_EQ(convChecked("<img src=foo.jpg>"), "<img src=\"foo.jpg\"/>");
  EXPECT_EQ(convChecked("<br/><br />"), "<br/><br/>");
  EXPECT_EQ(convChecked("x</br>"), "x<br/>");
}

TEST(AnkiHtml, UnclosedClosedAtEnd) {
  EXPECT_EQ(convChecked("<div><b>bold<i>it"), "<div><b>bold<i>it</i></b></div>");
}

TEST(AnkiHtml, EndTagClosesElementsAbove) {
  EXPECT_EQ(convChecked("<div><b><i>x</div>y"), "<div><b><i>x</i></b></div>y");
}

TEST(AnkiHtml, UnmatchedEndTagDropped) {
  EXPECT_EQ(convChecked("a</span>b</p></custom>"), "ab");
}

TEST(AnkiHtml, ImplicitCloses) {
  EXPECT_EQ(convChecked("<ul><li>a<li>b</ul>"), "<ul><li>a</li><li>b</li></ul>");
  EXPECT_EQ(convChecked("<p>a<p>b"), "<p>a</p><p>b</p>");
  EXPECT_EQ(convChecked("<p>a<div>b</div>"), "<p>a</p><div>b</div>");
  EXPECT_EQ(convChecked("<p>a<ul><li>x</ul>"), "<p>a</p><ul><li>x</li></ul>");
  EXPECT_EQ(convChecked("<p>a<h2>t</h2>"), "<p>a</p><h2>t</h2>");
  EXPECT_EQ(convChecked("<p>a<table><tr><td>1<td>2<tr><th>3</table>"),
            "<p>a</p><table><tr><td>1</td><td>2</td></tr><tr><th>3</th></tr></table>");
  EXPECT_EQ(convChecked("<dl><dt>a<dd>b<dt>c</dl>"), "<dl><dt>a</dt><dd>b</dd><dt>c</dt></dl>");
  EXPECT_EQ(convChecked("<select><option>a<option>b</select>"),
            "<select><option>a</option><option>b</option></select>");
  // Nested lists: the inner <li> does not close the outer one.
  EXPECT_EQ(convChecked("<ul><li>a<ul><li>b</ul><li>c</ul>"),
            "<ul><li>a<ul><li>b</li></ul></li><li>c</li></ul>");
  // <p> inside a <div> is not closed by a block outside that div.
  EXPECT_EQ(convChecked("<p>a<b>b<div>c"), "<p>a<b>b</b></p><div>c</div>");
}

TEST(AnkiHtml, AttributeNormalisation) {
  EXPECT_EQ(convChecked("<DIV CLASS=back>x</DIV>"), "<div class=\"back\">x</div>");
  EXPECT_EQ(convChecked("<span title='say \"hi\"'>x</span>"), "<span title=\"say &quot;hi&quot;\">x</span>");
  EXPECT_EQ(convChecked("<input checked disabled>"), "<input checked=\"checked\" disabled=\"disabled\"/>");
  EXPECT_EQ(convChecked("<span id=a id=b ID=c>x</span>"), "<span id=\"a\">x</span>");
  EXPECT_EQ(convChecked("<a href=\"x?a=1&b=2\" onclick=\"go()\" ONLOAD=x>l</a>"),
            "<a href=\"x?a=1&amp;b=2\">l</a>");
  EXPECT_EQ(convChecked("<span a<b=1 xml:lang=en 1x=2 @x=3 style=\"color:red\">x</span>"),
            "<span style=\"color:red\">x</span>");
  EXPECT_EQ(convChecked("<span title=\"a<b>c\">x</span>"), "<span title=\"a&lt;b&gt;c\">x</span>");
}

TEST(AnkiHtml, TextEscaping) {
  EXPECT_EQ(convChecked("AT&T"), "AT&amp;T");
  EXPECT_EQ(convChecked("a < b"), "a &lt; b");
  EXPECT_EQ(convChecked("a&b;c"), "a&amp;b;c");
  EXPECT_EQ(convChecked("]]>"), "]]&gt;");
  EXPECT_EQ(convChecked("a\x01" "b"), "ab");
}

TEST(AnkiHtml, NamedEntities) {
  EXPECT_EQ(convChecked("a&nbsp;b"), "a\xC2\xA0" "b");
  EXPECT_EQ(convChecked("&mdash;&hellip;&eacute;&szlig;&uuml;&euro;&frac12;&copy;"), "—…éßü€½©");
  EXPECT_EQ(convChecked("&amp;&lt;&gt;&quot;&apos;"), "&amp;&lt;&gt;&quot;&apos;");
  EXPECT_EQ(convChecked("&nosuchentity;"), "&amp;nosuchentity;");
  EXPECT_EQ(convChecked("&nbsp"), "&amp;nbsp");
}

TEST(AnkiHtml, NumericEntities) {
  EXPECT_EQ(convChecked("&#26408;&#x6728;&#X6728;"), "木木木");
  EXPECT_EQ(convChecked("&#65;&#x26;&#60;"), "A&amp;&lt;");
  EXPECT_EQ(convChecked("&#0;&#x110000;&#xD800;&#;&#x;&#12"), "&amp;#0;&amp;#x110000;&amp;#xD800;&amp;#;&amp;#x;&amp;#12");
  EXPECT_EQ(convChecked("&#x1F600;"), "\xF0\x9F\x98\x80");
}

TEST(AnkiHtml, RemovedElements) {
  EXPECT_EQ(convChecked("a<script>if (a < b && c) { x = '</div>'; }</script>b"), "ab");
  EXPECT_EQ(convChecked("a<SCRIPT type=x>1</Script >b"), "ab");
  EXPECT_EQ(convChecked("a<noscript><p>x</p></noscript>b"), "ab");
  EXPECT_EQ(convChecked("a<audio controls><source src=x.mp3></audio>b"), "ab");
  EXPECT_EQ(convChecked("a<video><track src=x></video>b"), "ab");
  EXPECT_EQ(convChecked("a<svg viewBox='0 0 1 1'><path d='M0'/><g><text>t</text></g></svg>b"), "ab");
  EXPECT_EQ(convChecked("a<iframe src=x>junk < here</iframe>b"), "ab");
  EXPECT_EQ(convChecked("a<object><embed src=x></object><embed src=y>b"), "ab");
  EXPECT_EQ(convChecked("a<template><div>t</div></template>b"), "ab");
  EXPECT_EQ(convChecked("<!DOCTYPE html>a<!-- c <b> -->b<?xml version='1.0'?>c"), "abc");
  EXPECT_EQ(convChecked("a<!-- never closed"), "a");
  EXPECT_EQ(convChecked("a<script>never closed"), "a");
  EXPECT_EQ(convChecked("<html><head><title>T</title></head><body>x</body></html>"), "x");
}

TEST(AnkiHtml, StyleDroppedAndExtracted) {
  const std::string html = "<style>.card { color: red; } a > b {}</style>x<STYLE media=all>p{}</style>";
  EXPECT_EQ(convChecked(html), "x");
  std::string css;
  ASSERT_TRUE(ankihtml::extractStyles(html, css, MAX));
  EXPECT_EQ(css, ".card { color: red; } a > b {}\np{}\n");
  ASSERT_TRUE(ankihtml::extractStyles("no styles", css, MAX));
  EXPECT_EQ(css, "");
  EXPECT_FALSE(ankihtml::extractStyles(html, css, 5));
  EXPECT_EQ(css, "");
}

TEST(AnkiHtml, IconFontElementsDropped) {
  EXPECT_EQ(convChecked("<span class=\"material-icons\">play_arrow</span>Word"), "Word");
  EXPECT_EQ(convChecked("<i class='btn Material-Symbols-Outlined'><b>volume_up</b></i>x"), "x");
  EXPECT_EQ(convChecked("<div><span class=material-icons>play_arrow</div>y"), "<div></div>y");
}

TEST(AnkiHtml, KeptElementsUnchanged) {
  EXPECT_EQ(convChecked("<ruby><rb>木</rb><rt>こ</rt></ruby>"), "<ruby><rb>木</rb><rt>こ</rt></ruby>");
  EXPECT_EQ(convChecked("<ruby>漢<rp>(</rp><rt>かん</rt><rp>)</rp></ruby>"),
            "<ruby>漢<rp>(</rp><rt>かん</rt><rp>)</rp></ruby>");
  EXPECT_EQ(convChecked("<ruby>木<rt>き<rt>x</ruby>"), "<ruby>木<rt>き</rt><rt>x</rt></ruby>");
  EXPECT_EQ(convChecked("<font color=red>x</font><sub>1</sub><sup>2</sup><small>s</small>"),
            "<font color=\"red\">x</font><sub>1</sub><sup>2</sup><small>s</small>");
}

TEST(AnkiHtml, UnknownTagsStripped) {
  EXPECT_EQ(convChecked("<o:p>a</o:p><custom-el x=1>b</custom-el><averyveryverylongtagname>c"), "abc");
}

TEST(AnkiHtml, StrayLessThan) {
  EXPECT_EQ(convChecked("1 <2 and 3< 4 6 <"), "1 &lt;2 and 3&lt; 4 6 &lt;");
  // "</" not followed by a letter is a bogus comment up to '>', as in browsers.
  EXPECT_EQ(convChecked("a</ x>b"), "ab");
}

TEST(AnkiHtml, MaxDepth) {
  std::string html;
  for (int i = 0; i < 100; ++i) html += "<span>";
  html += "x";
  const std::string out = convChecked(html);
  size_t opens = 0;
  for (size_t p = out.find("<span>"); p != std::string::npos; p = out.find("<span>", p + 1)) ++opens;
  EXPECT_EQ(opens, 64u);
}

TEST(AnkiHtml, OverflowReturnsFalse) {
  std::string out = "stale";
  EXPECT_FALSE(ankihtml::toXhtml("<div>abcdefgh</div>", out, 10));
  EXPECT_TRUE(out.empty());
  EXPECT_TRUE(ankihtml::toXhtml("<div>abcdefgh</div>", out, 19));
  EXPECT_EQ(out, "<div>abcdefgh</div>");
  // Closing tags added at the end count too.
  EXPECT_FALSE(ankihtml::toXhtml("<div>abcdefgh", out, 18));
}

TEST(AnkiHtml, InvalidUtf8PassedThrough) {
  EXPECT_EQ(conv("a\xFF" "b"), "a\xFF" "b");
}

TEST(AnkiHtml, RealWorldCard) {
  const std::string html =
      "<style>.card{font-family:serif}</style>"
      "<div class=front>日本語<br><br>"
      "<span class=\"material-icons\" onclick=\"play()\">play_arrow</span>[sound:x.mp3]</div>"
      "<hr id=answer>"
      "<div class=back><ruby><rb>木</rb><rt>こ</rt></ruby>&nbsp;漏れ日"
      "<ul><li>sunlight<li>through trees</ul>"
      "<script>if (a < b && c) { show(); }</script>"
      "<img src=foo.jpg> AT&T";
  EXPECT_EQ(convChecked(html),
            "<div class=\"front\">日本語<br/><br/>[sound:x.mp3]</div><hr id=\"answer\"/>"
            "<div class=\"back\"><ruby><rb>木</rb><rt>こ</rt></ruby>\xC2\xA0漏れ日"
            "<ul><li>sunlight</li><li>through trees</li></ul>"
            "<img src=\"foo.jpg\"/> AT&amp;T</div>");
}

TEST(AnkiHtml, GarbageAlwaysWellFormed) {
  const char* inputs[] = {
      "<", "<<>>", "</", "<a", "<a href", "<a href=", "<a href='x", "<p<p>", "<div =x>", "&", "&#", "&#x",
      "<table><td>x</tr></table></td>", "<ul><li><p>a<li>b", "</div></div><div>", "<b><i></b></i>", "<p/>x",
      "<span class=material-icons><svg><script>x</span>", "<br/ >", "<img src=\"a\"b=c>",
  };
  for (const char* in : inputs) {
    SCOPED_TRACE(in);
    convChecked(in);
  }
}

}  // namespace
