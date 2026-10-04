#include <gtest/gtest.h>

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
