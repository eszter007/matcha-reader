#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// EPUB font obfuscation: a publisher XORs the head of each embedded font with a key derived from
// the book's unique identifier, and lists the font in META-INF/encryption.xml.
namespace fontobfuscation {

enum class Scheme : uint8_t { None, Idpf, Adobe };

// The scheme an encryption.xml Algorithm URI names.
Scheme schemeForAlgorithm(std::string_view algorithm);

// The key for a scheme from the package's unique identifier. Idpf: SHA-1 of the identifier with
// whitespace removed (20 bytes). Adobe: the 16 bytes of its UUID hex digits. Returns the key
// length, 0 when the identifier cannot make a key.
size_t deriveKey(Scheme scheme, std::string_view uniqueIdentifier, uint8_t key[20]);

// XORs the obfuscated head (1040 bytes for Idpf, 1024 for Adobe) of a font held whole in data.
void deobfuscate(Scheme scheme, const uint8_t* key, size_t keyLen, uint8_t* data, size_t size);

// SHA-1 of data (FIPS 180-1). Here so host tests and every board share one implementation.
void sha1(const uint8_t* data, size_t size, uint8_t out[20]);

}  // namespace fontobfuscation
