#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "util/CjkScript.h"

namespace {

using cjk::scriptForLanguage;

TEST(CjkScript, JapaneseTags) {
  EXPECT_EQ(scriptForLanguage("ja"), CjkScript::Japanese);
  EXPECT_EQ(scriptForLanguage("JA"), CjkScript::Japanese);
  EXPECT_EQ(scriptForLanguage("ja-JP"), CjkScript::Japanese);
  EXPECT_EQ(scriptForLanguage("jpn"), CjkScript::Japanese);
}

TEST(CjkScript, ChineseDefaultsToSimplified) {
  EXPECT_EQ(scriptForLanguage("zh"), CjkScript::SimplifiedChinese);
  EXPECT_EQ(scriptForLanguage("zh-CN"), CjkScript::SimplifiedChinese);
  EXPECT_EQ(scriptForLanguage("zh-Hans"), CjkScript::SimplifiedChinese);
  EXPECT_EQ(scriptForLanguage("zh_SG"), CjkScript::SimplifiedChinese);
  EXPECT_EQ(scriptForLanguage("cmn"), CjkScript::SimplifiedChinese);
  EXPECT_EQ(scriptForLanguage("zho"), CjkScript::SimplifiedChinese);
  EXPECT_EQ(scriptForLanguage("cmn-Hans-CN"), CjkScript::SimplifiedChinese);
}

TEST(CjkScript, TraditionalRegionsAndScripts) {
  EXPECT_EQ(scriptForLanguage("zh-TW"), CjkScript::TraditionalChinese);
  EXPECT_EQ(scriptForLanguage("zh-Hant"), CjkScript::TraditionalChinese);
  EXPECT_EQ(scriptForLanguage("zh-Hant-TW"), CjkScript::TraditionalChinese);
  EXPECT_EQ(scriptForLanguage("zh_HK"), CjkScript::TraditionalChinese);
  EXPECT_EQ(scriptForLanguage("zh-MO"), CjkScript::TraditionalChinese);
  EXPECT_EQ(scriptForLanguage("yue"), CjkScript::TraditionalChinese);
  EXPECT_EQ(scriptForLanguage("ZH-tw"), CjkScript::TraditionalChinese);
}

TEST(CjkScript, NonCjkAndMalformedTags) {
  EXPECT_EQ(scriptForLanguage(""), CjkScript::None);
  EXPECT_EQ(scriptForLanguage("en"), CjkScript::None);
  EXPECT_EQ(scriptForLanguage("fr-FR"), CjkScript::None);
  EXPECT_EQ(scriptForLanguage("ko"), CjkScript::None);
  // A prefix match is not a language: "jam" (Jamaican Creole), "zha" (Zhuang).
  EXPECT_EQ(scriptForLanguage("jam"), CjkScript::None);
  EXPECT_EQ(scriptForLanguage("zha"), CjkScript::None);
  EXPECT_EQ(scriptForLanguage("zh-"), CjkScript::SimplifiedChinese);
}

TEST(CjkScript, Helpers) {
  EXPECT_TRUE(cjk::isChinese(CjkScript::SimplifiedChinese));
  EXPECT_TRUE(cjk::isChinese(CjkScript::TraditionalChinese));
  EXPECT_FALSE(cjk::isChinese(CjkScript::Japanese));
  EXPECT_FALSE(cjk::isChinese(CjkScript::None));
  EXPECT_STREQ(cjk::dictIndexFolder(CjkScript::Japanese), "jp");
  EXPECT_STREQ(cjk::dictIndexFolder(CjkScript::SimplifiedChinese), "zh");
  EXPECT_STREQ(cjk::dictIndexFolder(CjkScript::TraditionalChinese), "zh");
  EXPECT_EQ(cjk::dictIndexFolder(CjkScript::None), nullptr);
  EXPECT_STREQ(cjk::dictIndexFolderForLanguage("zh-TW"), "zh");
  EXPECT_STREQ(cjk::dictIndexFolderForLanguage("yue"), "yue");
  EXPECT_STREQ(cjk::dictIndexFolderForLanguage("yue-Hant-HK"), "yue");
  EXPECT_STREQ(cjk::dictIndexFolderForLanguage("ja"), "jp");
  EXPECT_EQ(cjk::dictIndexFolderForLanguage("en"), nullptr);
  EXPECT_EQ(cjk::probeCodepoint(CjkScript::Japanese), 0x3042u);
  EXPECT_EQ(cjk::probeCodepoint(CjkScript::TraditionalChinese), 0x7684u);
}

}  // namespace

namespace {

cjk::ScriptSniff sniff(const std::string& text) {
  cjk::ScriptSniff s;
  // Feed in odd-sized chunks so a multi-byte character is split across calls.
  for (size_t i = 0; i < text.size(); i += 5) {
    s.feed(reinterpret_cast<const uint8_t*>(text.data()) + i, std::min<size_t>(5, text.size() - i));
  }
  return s;
}

std::string repeat(const std::string& s, int n) {
  std::string out;
  for (int i = 0; i < n; i++) out += s;
  return out;
}

TEST(ScriptSniff, DetectsJapaneseByKana) {
  EXPECT_EQ(sniff(repeat("私は学生です。", 10)).verdict(), CjkScript::Japanese);
}

TEST(ScriptSniff, DetectsSimplifiedAndTraditionalChinese) {
  EXPECT_EQ(sniff(repeat("<p>这是一个说话的时候。</p>", 10)).verdict(), CjkScript::SimplifiedChinese);
  EXPECT_EQ(sniff(repeat("<p>這是一個說話的時候。</p>", 10)).verdict(), CjkScript::TraditionalChinese);
  // No distinguishing characters at all: simplified, the mainland default.
  EXPECT_EQ(sniff(repeat("人山人海，天上人间。", 10)).verdict(), CjkScript::SimplifiedChinese);
}

TEST(ScriptSniff, LatinBooksStayLatin) {
  EXPECT_EQ(sniff(repeat("The quick brown fox jumps over the lazy dog. ", 20)).verdict(), CjkScript::None);
  EXPECT_EQ(sniff(repeat("He wrote 中国 once in a long English paragraph about travel. ", 20)).verdict(),
            CjkScript::None);
  EXPECT_EQ(sniff("短").verdict(), CjkScript::None);  // too little to judge
  // Markup does not count as Latin text.
  EXPECT_EQ(sniff(repeat("<p class=\"calibre1\"><span>这是说话</span></p>", 10)).verdict(),
            CjkScript::SimplifiedChinese);
}

TEST(ScriptSniff, HeadStylesheetAndEntitiesAreNotText) {
  // A Sigil-style chapter: 1500 characters of CSS in <head>, then the text with &nbsp; indents.
  const std::string css =
      "<html><head><title>Chapter 1</title><style>" + repeat("p { margin: 0 1em; } ", 70) + "</style></head><body>";
  EXPECT_EQ(sniff(css + repeat("<p>&nbsp;&nbsp;这是一个说话的时候。</p>", 10) + "</body></html>").verdict(),
            CjkScript::SimplifiedChinese);
  EXPECT_EQ(sniff(css + repeat("<p>&nbsp;&nbsp;Hello there friend.</p>", 10)).verdict(), CjkScript::None);
  // A chapter that is only its head decides nothing.
  const cjk::ScriptSniff headOnly = sniff(css + "</body></html>");
  EXPECT_EQ(headOnly.han + headOnly.kana + headOnly.latin, 0u);
  // A self-closing <script/> in the body (EPUB 3 XHTML) opens no block to skip.
  EXPECT_EQ(sniff(css + "<script src=\"../js/x.js\"/>" + repeat("<p>這是一個說話的時候。</p>", 10)).verdict(),
            CjkScript::TraditionalChinese);
}

TEST(ScriptSniff, EnoughStopsEarly) {
  cjk::ScriptSniff s = sniff(repeat("中国", 250));
  EXPECT_TRUE(s.enough());
}

TEST(LanguageChoice, Tags) {
  EXPECT_EQ(cjk::languageTagForChoice(cjk::LANG_AUTO), nullptr);
  EXPECT_STREQ(cjk::languageTagForChoice(cjk::LANG_ZH_HANT), "zh-Hant");
  EXPECT_EQ(cjk::scriptForLanguage(cjk::languageTagForChoice(cjk::LANG_YUE)), CjkScript::TraditionalChinese);
}

}  // namespace
