#pragma once

#include <cstddef>
#include <cstdint>

#include "SerializedHyphenationTrie.h"

// Hyphenation patterns for a language not built into the firmware, downloaded to
// HyphenationPack::DIR as <tag>.cphyph. Format: docs/file-formats.md. One pack is loaded at a time:
// mapped from internal flash on boards without PSRAM (FlashBlobCache), read into PSRAM otherwise.
namespace HyphenationPack {

#ifndef HYPHENATION_PACK_DIR
#define HYPHENATION_PACK_DIR "/.crosspoint/hyph"
#endif
constexpr const char* DIR = HYPHENATION_PACK_DIR;
// Also names the release the packs download from (hyphenation-<version>).
#define HYPHENATION_PACK_FORMAT_VERSION 1
constexpr uint8_t FORMAT_VERSION = HYPHENATION_PACK_FORMAT_VERSION;
constexpr size_t HEADER_SIZE = 24;
// Room for "<DIR>/<tag>.cphyph" with a tag of up to 4 letters.
constexpr size_t PATH_SIZE = sizeof(HYPHENATION_PACK_DIR) + sizeof("/xxxx.cphyph");

// Writes "<DIR>/<tag>.cphyph" to out; false when it does not fit.
bool pathFor(const char* tag, char* out, size_t outSize);

bool exists(const char* path);

// The file's header is a version-1 pack for `tag` and its size matches. The trie itself is
// verified against its hash when loaded.
bool isValid(const char* path, const char* tag);

// Loads the pack at `path`, replacing the loaded one. On success `out` points at its trie until release().
// A file that is not a valid pack for `tag` is deleted, so the language reads "Download" again.
bool load(const char* path, const char* tag, SerializedHyphenationPatterns& out);

// Unmaps or frees the loaded pack.
void release();

}  // namespace HyphenationPack
