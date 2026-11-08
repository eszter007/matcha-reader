#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "Epub/FontObfuscation.h"
#include "Epub/css/CssStyle.h"

using namespace fontobfuscation;

TEST(FontObfuscation, Sha1KnownVectors) {
  uint8_t out[20];
  sha1(reinterpret_cast<const uint8_t*>("abc"), 3, out);
  const uint8_t abc[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                           0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
  EXPECT_EQ(0, memcmp(out, abc, 20));
  // 56 bytes: the padding spills into a second block.
  const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  sha1(reinterpret_cast<const uint8_t*>(two), strlen(two), out);
  const uint8_t twoBlocks[20] = {0x84, 0x98, 0x3e, 0x44, 0x1c, 0x3b, 0xd2, 0x6e, 0xba, 0xae,
                                 0x4a, 0xa1, 0xf9, 0x51, 0x29, 0xe5, 0xe5, 0x46, 0x70, 0xf1};
  EXPECT_EQ(0, memcmp(out, twoBlocks, 20));
}

TEST(FontObfuscation, SchemesFromAlgorithm) {
  EXPECT_EQ(Scheme::Idpf, schemeForAlgorithm("http://www.idpf.org/2008/embedding"));
  EXPECT_EQ(Scheme::Adobe, schemeForAlgorithm("http://ns.adobe.com/pdf/enc#RC"));
  EXPECT_EQ(Scheme::None, schemeForAlgorithm("http://www.w3.org/2001/04/xmlenc#aes128-cbc"));
}

TEST(FontObfuscation, IdpfKeyIgnoresWhitespace) {
  uint8_t a[20], b[20];
  ASSERT_EQ(20u, deriveKey(Scheme::Idpf, "urn:uuid:1234", a));
  ASSERT_EQ(20u, deriveKey(Scheme::Idpf, "  urn:uuid:\n1234 ", b));
  EXPECT_EQ(0, memcmp(a, b, 20));
  EXPECT_EQ(0u, deriveKey(Scheme::Idpf, " \t", a));
}

TEST(FontObfuscation, AdobeKeyFromUuid) {
  uint8_t key[20];
  ASSERT_EQ(16u, deriveKey(Scheme::Adobe, "urn:uuid:12345678-1234-5678-1234-567812345678", key));
  EXPECT_EQ(0x12, key[0]);
  EXPECT_EQ(0x78, key[15]);
  EXPECT_EQ(0u, deriveKey(Scheme::Adobe, "isbn:9781234567890", key));
}

TEST(FontObfuscation, RoundTripTouchesOnlyTheHead) {
  std::vector<uint8_t> font(4000);
  for (size_t i = 0; i < font.size(); ++i) font[i] = static_cast<uint8_t>(i * 7);
  const std::vector<uint8_t> original = font;
  uint8_t key[20];
  const size_t len = deriveKey(Scheme::Idpf, "book-id", key);
  deobfuscate(Scheme::Idpf, key, len, font.data(), font.size());
  EXPECT_NE(original[0], font[0]);
  EXPECT_EQ(original[1040], font[1040]);
  deobfuscate(Scheme::Idpf, key, len, font.data(), font.size());
  EXPECT_EQ(original, font);
  // A file shorter than the obfuscated head is XORed whole, never past its end.
  std::vector<uint8_t> tiny(10, 0);
  deobfuscate(Scheme::Adobe, key, 16, tiny.data(), tiny.size());
  EXPECT_EQ(key[9], tiny[9]);
}

TEST(FontFamilyKey, FirstNonGenericFamily) {
  EXPECT_EQ(fontFamilyKeyOf("Body Mono"), fontFamilyListKey("\"Body Mono\", Georgia, serif"));
  EXPECT_EQ(fontFamilyKeyOf("Georgia"), fontFamilyListKey("serif, Georgia"));
  EXPECT_EQ(0, fontFamilyListKey("serif"));
  EXPECT_EQ(0, fontFamilyListKey("  Sans-Serif , monospace "));
  EXPECT_EQ(fontFamilyKeyOf("Display"), fontFamilyKeyOf("'display'"));
  EXPECT_NE(0, fontFamilyKeyOf("x"));
}
