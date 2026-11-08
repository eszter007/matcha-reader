#include "FontObfuscation.h"

#include <cstring>

namespace fontobfuscation {

Scheme schemeForAlgorithm(const std::string_view algorithm) {
  if (algorithm == "http://www.idpf.org/2008/embedding") return Scheme::Idpf;
  if (algorithm == "http://ns.adobe.com/pdf/enc#RC") return Scheme::Adobe;
  return Scheme::None;
}

namespace {
constexpr uint32_t rotl(const uint32_t v, const int n) { return (v << n) | (v >> (32 - n)); }

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

void sha1(const uint8_t* data, const size_t size, uint8_t out[20]) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  uint8_t block[64];
  const uint64_t bitLen = static_cast<uint64_t>(size) * 8;
  // size bytes, the 0x80 marker, zero padding, then the 8-byte length: a whole number of blocks.
  const size_t total = ((size + 9 + 63) / 64) * 64;
  for (size_t offset = 0; offset < total; offset += 64) {
    for (size_t i = 0; i < 64; ++i) {
      const size_t at = offset + i;
      if (at < size) {
        block[i] = data[at];
      } else if (at == size) {
        block[i] = 0x80;
      } else if (at >= total - 8) {
        block[i] = static_cast<uint8_t>(bitLen >> (8 * (total - 1 - at)));
      } else {
        block[i] = 0;
      }
    }
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = static_cast<uint32_t>(block[i * 4]) << 24 | static_cast<uint32_t>(block[i * 4 + 1]) << 16 |
             static_cast<uint32_t>(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
    }
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t t = rotl(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotl(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  for (int i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
    out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
    out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
    out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
  }
}

size_t deriveKey(const Scheme scheme, const std::string_view uniqueIdentifier, uint8_t key[20]) {
  if (scheme == Scheme::Idpf) {
    std::string compact;
    compact.reserve(uniqueIdentifier.size());
    for (const char c : uniqueIdentifier) {
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') compact.push_back(c);
    }
    if (compact.empty()) return 0;
    sha1(reinterpret_cast<const uint8_t*>(compact.data()), compact.size(), key);
    return 20;
  }
  if (scheme == Scheme::Adobe) {
    std::string_view id = uniqueIdentifier;
    constexpr std::string_view PREFIX = "urn:uuid:";
    if (id.substr(0, PREFIX.size()) == PREFIX) id.remove_prefix(PREFIX.size());
    size_t n = 0;
    int high = -1;
    for (const char c : id) {
      const int v = hexValue(c);
      if (v < 0) continue;  // dashes
      if (n >= 16) return 0;
      if (high < 0) {
        high = v;
      } else {
        key[n++] = static_cast<uint8_t>(high << 4 | v);
        high = -1;
      }
    }
    return n == 16 ? 16 : 0;
  }
  return 0;
}

void deobfuscate(const Scheme scheme, const uint8_t* key, const size_t keyLen, uint8_t* data, const size_t size) {
  if (keyLen == 0 || scheme == Scheme::None) return;
  const size_t head = scheme == Scheme::Idpf ? 1040 : 1024;
  const size_t n = size < head ? size : head;
  for (size_t i = 0; i < n; ++i) data[i] ^= key[i % keyLen];
}

}  // namespace fontobfuscation
