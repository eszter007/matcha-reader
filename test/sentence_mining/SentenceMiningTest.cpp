#include <gtest/gtest.h>

#include "SentenceMiningCsv.h"

using namespace sentencemining;

TEST(SentenceMiningCsv, PlainFieldIsLeftAlone) { EXPECT_EQ(csvField("漏らす"), "漏らす"); }

TEST(SentenceMiningCsv, FieldWithCommaOrQuoteIsQuotedAndQuotesDoubled) {
  EXPECT_EQ(csvField("to leak, to let out"), "\"to leak, to let out\"");
  EXPECT_EQ(csvField("say \"hi\""), "\"say \"\"hi\"\"\"");
  EXPECT_EQ(csvField("two\nlines"), "\"two\nlines\"");
}

TEST(SentenceMiningCsv, HtmlSpecialsAreEscaped) { EXPECT_EQ(htmlEscape("a<b & c>d"), "a&lt;b &amp; c&gt;d"); }

TEST(SentenceMiningCsv, GuidIsStableAndSeparatesWordFromSentence) {
  EXPECT_EQ(makeGuid("漏らす", "秘密を漏らした。"), makeGuid("漏らす", "秘密を漏らした。"));
  EXPECT_NE(makeGuid("ab", "c"), makeGuid("a", "bc"));
  EXPECT_EQ(makeGuid("x", "y").size(), 17u);
  EXPECT_EQ(makeGuid("x", "y")[0], 'm');
}

TEST(SentenceMiningCsv, TitleTagHasNoSpaces) {
  EXPECT_EQ(tagFromTitle("The Brothers Karamazov"), "The_Brothers_Karamazov");
  EXPECT_EQ(tagFromTitle("銀河　鉄道 "), "銀河_鉄道");
  EXPECT_EQ(tagFromTitle(""), "");
}

TEST(SentenceMiningCsv, JapaneseSentenceIsCutAtTheSurroundingEnds) {
  // Previous sentence ends at 。 -- the card starts after it and ends at the next 。
  EXPECT_EQ(sentenceHtml("雨だった。彼女は秘密を", "漏らし", "た。次の日、"), "彼女は秘密を<b>漏らし</b>た。");
}

TEST(SentenceMiningCsv, ClosingBracketStaysWithItsSentence) {
  EXPECT_EQ(sentenceHtml("「行こう。」と", "言った", "。また"), "と<b>言った</b>。");
  EXPECT_EQ(sentenceHtml("", "行く", "よ。」と"), "<b>行く</b>よ。」");
}

TEST(SentenceMiningCsv, EnglishKeepsSpacesAroundTheWord) {
  EXPECT_EQ(sentenceHtml("It rained. Did ", "anyone", " see it? No."), "Did <b>anyone</b> see it?");
}

TEST(SentenceMiningCsv, PageLineBreaksCollapseToOneLine) {
  EXPECT_EQ(sentenceHtml("the old\n  man ", "walked", "\nhome."), "the old man <b>walked</b> home.");
}

TEST(SentenceMiningCsv, SentenceTextIsHtmlEscaped) {
  EXPECT_EQ(sentenceHtml("if a < b ", "then", " c."), "if a &lt; b <b>then</b> c.");
}

TEST(SentenceMiningCsv, UnterminatedSentenceIsCappedAroundTheWord) {
  std::string before, after;
  for (int i = 0; i < 400; ++i) before += "あ";
  for (int i = 0; i < 400; ++i) after += "い";
  const std::string html = sentenceHtml(before, "語", after);
  // Strip the markup and count codepoints: at most the cap.
  std::string plain = html;
  for (const char* tag : {"<b>", "</b>"}) plain.erase(plain.find(tag), std::string(tag).size());
  size_t cps = 0;
  for (const char c : plain) cps += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
  EXPECT_LE(cps, MAX_SENTENCE_CODEPOINTS);
  EXPECT_NE(html.find("<b>語</b>"), std::string::npos);
}

TEST(SentenceMiningCsv, SentenceEndDetection) {
  EXPECT_TRUE(hasSentenceEnd("た。次"));
  EXPECT_TRUE(hasSentenceEnd("see it? No"));
  EXPECT_FALSE(hasSentenceEnd("それから彼は"));
}

TEST(SentenceMiningCsv, CapCutsOnACharacterBoundary) {
  EXPECT_EQ(capUtf8("abc", 10), "abc");
  const std::string capped = capUtf8("あいうえお", 8);  // 15 bytes -> 5 bytes of text + "…"
  EXPECT_EQ(capped, "あ…");
  EXPECT_LE(capped.size(), 8u);
}

TEST(SentenceMiningCsv, HeaderTellsAnkiTheLayout) {
  const std::string h = headerLines();
  EXPECT_NE(h.find("#separator:comma\n"), std::string::npos);
  EXPECT_NE(h.find("#html:true\n"), std::string::npos);
  EXPECT_NE(h.find("#guid column:1\n"), std::string::npos);
  EXPECT_NE(h.find("#tags column:10\n"), std::string::npos);
  EXPECT_NE(h.find("#columns:GUID,Word,Reading,Sentence,Definition,Book,Author,Date,Dictionary,Tags\n"),
            std::string::npos);
}

TEST(SentenceMiningCsv, RowHasTenFieldsInOrder) {
  Card c;
  c.word = "漏らす";
  c.reading = "もらす";
  c.sentence = "秘密を<b>漏らし</b>た。";
  c.definition = "1. to let leak, to let out<br>2. to divulge";
  c.book = "銀河鉄道の夜";
  c.author = "宮沢賢治";
  c.date = "2026-09-29";
  c.dictionary = "JMdict";
  const std::string row = formatRow(c);
  const std::string guid = makeGuid(c.word, c.sentence);
  EXPECT_EQ(row, guid +
                     ",漏らす,もらす,秘密を<b>漏らし</b>た。,\"1. to let leak, to let out<br>2. to divulge\","
                     "銀河鉄道の夜,宮沢賢治,2026-09-29,JMdict,matcha 銀河鉄道の夜\n");
}

TEST(SentenceMiningCsv, LongDefinitionIsCapped) {
  Card c;
  c.word = "w";
  c.definition = std::string(5000, 'x');
  const std::string row = formatRow(c);
  EXPECT_LT(row.size(), MAX_DEFINITION_BYTES + 200);
}

TEST(SentenceMiningFiles, OneFilePerLanguage) {
  EXPECT_EQ(exportPath("ja"), "/sentence-mining/sentences-ja.csv");
  EXPECT_EQ(exportPath("EN"), "/sentence-mining/sentences-en.csv");
  EXPECT_EQ(exportPath("pt_BR"), "/sentence-mining/sentences-pt-br.csv");
  EXPECT_EQ(exportPath(""), "/sentence-mining/sentences-other.csv");
  EXPECT_EQ(exportPath("../x"), "/sentence-mining/sentences-x.csv");  // never a path out of the folder
}

TEST(SentenceMiningFiles, LanguageComesFromTheDictionaryFolderFirst) {
  EXPECT_EQ(languageForDictionary("en/Collins", "fr"), "en");
  EXPECT_EQ(languageForDictionary("pt-BR/Aulete", ""), "pt-BR");
  // No language segment: the book's language, reduced to its primary subtag.
  EXPECT_EQ(languageForDictionary("Collins", "en-US"), "en");
  EXPECT_EQ(languageForDictionary("", "fr_CA"), "fr");
  EXPECT_EQ(languageForDictionary("", ""), "");
  // A first segment that is not a language code is not taken for one.
  EXPECT_EQ(languageForDictionary("My Dicts/Collins", "de"), "de");
}

TEST(SentenceMiningFiles, PlainDefinitionBecomesEscapedHtmlWithBreaks) {
  EXPECT_EQ(definitionHtml("5-dan verb\n1. to leak <sth>\n2. to divulge"),
            "5-dan verb<br>1. to leak &lt;sth&gt;<br>2. to divulge");
  // Blank lines between entries collapse to one gap.
  EXPECT_EQ(definitionHtml("a\n\n\n\nb"), "a<br><br>b");
}

TEST(SentenceMiningFiles, HtmlCapNeverLeavesATagOpen) {
  std::string html;
  while (html.size() < MAX_DEFINITION_BYTES + 100) html += "<span class=\"x\">word</span> ";
  const std::string capped = capHtml(html);
  EXPECT_LE(capped.size(), MAX_DEFINITION_BYTES);
  EXPECT_GT(capped.rfind('>'), capped.rfind('<'));
  EXPECT_EQ(capHtml("<b>short</b>"), "<b>short</b>");
}

// From a device export: the card began with the previous line of dialogue, which ends in 」 with
// no 。, and kept the full-width space after it.
TEST(SentenceMiningCsv, DialogueLineEndingInBracketIsItsOwnSentence) {
  EXPECT_EQ(
      sentenceHtml("「まあ、刑務所よりは居心地がよさそうですね」　実際、その評価は", "甘んじて", "受けることにした。"),
      "実際、その評価は<b>甘んじて</b>受けることにした。");
}

TEST(SentenceMiningCsv, BracketFollowedByTextStaysInTheSentence) {
  // 「行こう」と言った is one sentence: the bracket is followed by text, not a break.
  EXPECT_EQ(sentenceHtml("雨だ。「行こう」と", "言った", "。"), "「行こう」と<b>言った</b>。");
}

TEST(SentenceMiningCsv, BracketBeforeANewQuoteEndsTheSentence) {
  EXPECT_EQ(sentenceHtml("「はい」「", "いいえ", "」"), "「<b>いいえ</b>」");
}

TEST(SentenceMiningCsv, ParagraphIndentIsNotPartOfTheSentence) {
  EXPECT_EQ(sentenceHtml("　彼は", "走った", "。"), "彼は<b>走った</b>。");
}

TEST(SentenceMiningFiles, DefinitionDropsThePanelsExampleBars) {
  EXPECT_EQ(definitionHtml("1. to content oneself with\n  │ 運命に甘んじる。\n  │ To accept one's fate."),
            "1. to content oneself with<br>運命に甘んじる。<br>To accept one's fate.");
}
