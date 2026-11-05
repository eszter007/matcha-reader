#include <gtest/gtest.h>

#include "network/ProtectedPaths.h"

using protectedpaths::isPluginPath;
using protectedpaths::isSensitivePath;

TEST(ProtectedPaths, CredentialStoresAreSensitive) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/opds.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/koreader.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json.tmp"));
  EXPECT_TRUE(isSensitivePath("/.CrossPoint/WIFI.json"));
}

TEST(ProtectedPaths, SpellingVariantsCannotDodgeTheMatch) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/./wifi.json"));
  EXPECT_TRUE(isSensitivePath("//.crosspoint//wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/plugins/../wifi.json"));
  EXPECT_TRUE(isSensitivePath(".crosspoint/opds.json"));
}

TEST(ProtectedPaths, NamesSdFatWouldTrimOrAliasStillMatch) {
  EXPECT_TRUE(isSensitivePath("/.crosspoint/wifi.json."));
  EXPECT_TRUE(isSensitivePath("/.crosspoint./wifi.json"));
  EXPECT_TRUE(isSensitivePath("/ .crosspoint/ wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint ./opds.json"));
  EXPECT_TRUE(isSensitivePath("/CROSSP~1/wifi.json"));
  EXPECT_TRUE(isSensitivePath("/.crosspoint/WIFI~1.JSO"));
  EXPECT_FALSE(isSensitivePath("/Books/Photos/IMG~1.JPG"));  // too deep to reach a store
  EXPECT_FALSE(isSensitivePath("/Books/a~b.epub"));
}

TEST(ProtectedPaths, EverythingElseStaysReachable) {
  EXPECT_FALSE(isSensitivePath("/.crosspoint/settings.json"));
  EXPECT_FALSE(isSensitivePath("/.crosspoint/library.idx"));
  EXPECT_FALSE(isSensitivePath("/.crosspoint/month-wallpaper.json"));
  EXPECT_FALSE(isSensitivePath("/.fonts/Noto/Noto_14.cpfont"));
  EXPECT_FALSE(isSensitivePath("/Books/wifi.json"));
}

// Renaming, moving or deleting the folder would take the stores along under a path the file check
// no longer matches.
TEST(ProtectedPaths, TheFolderHoldingTheStoresIsProtectedFromRelocation) {
  EXPECT_TRUE(protectedpaths::holdsSensitivePath("/.crosspoint"));
  EXPECT_TRUE(protectedpaths::holdsSensitivePath("/.CrossPoint/"));
  EXPECT_TRUE(protectedpaths::holdsSensitivePath("//.crosspoint/./"));
  EXPECT_TRUE(protectedpaths::isSensitiveOrHoldsOne("/.crosspoint"));
  EXPECT_TRUE(protectedpaths::isSensitiveOrHoldsOne("/.crosspoint/wifi.json"));
}

TEST(ProtectedPaths, OtherFoldersAreNotHolders) {
  EXPECT_FALSE(protectedpaths::holdsSensitivePath("/"));
  EXPECT_FALSE(protectedpaths::holdsSensitivePath("/Books"));
  EXPECT_FALSE(protectedpaths::holdsSensitivePath("/.Trashes"));
  EXPECT_FALSE(protectedpaths::holdsSensitivePath("/.crosspoint/epub_123"));
  EXPECT_FALSE(protectedpaths::holdsSensitivePath("/.cross"));
  EXPECT_FALSE(protectedpaths::isSensitiveOrHoldsOne("/.crosspoint/epub_123/book.bin"));
}

TEST(ProtectedPaths, PluginPathRules) {
  EXPECT_FALSE(isPluginPath("/.crosspoint/plugins/../wifi.json"));
  EXPECT_FALSE(isPluginPath("/.crosspoint/./koreader.json"));
  EXPECT_FALSE(isPluginPath("relative/path"));
  EXPECT_FALSE(isPluginPath("/"));
  EXPECT_TRUE(isPluginPath("/.crosspoint/month-wallpaper.json"));
  EXPECT_TRUE(isPluginPath("/Books/new.epub"));
}
