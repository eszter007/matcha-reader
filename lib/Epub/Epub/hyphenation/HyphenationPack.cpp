#include "HyphenationPack.h"

#include <FlashBlobCache.h>
#include <Fnv1a.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The German trie (~201 KB) cannot live in the C3's internal heap; there a pack loads only into flash.
#if defined(ESP_PLATFORM) && !defined(SIMULATOR) && !defined(BOARD_HAS_PSRAM)
#define HYPHENATION_PACK_HEAP 0
#else
#define HYPHENATION_PACK_HEAP 1
#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
#include <esp_heap_caps.h>
#endif
#endif

namespace HyphenationPack {
namespace {

constexpr char kMagic[4] = {'C', 'P', 'H', 'Y'};

struct Header {
  uint32_t rootOffset;
  uint32_t dataLength;
  uint32_t dataKey;  // FNV-1a of the trie, | 1
};

const uint8_t* loadedData = nullptr;
bool loadedMapped = false;

uint32_t readU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

bool readHeader(HalFile& file, const char* tag, Header& out) {
  uint8_t raw[HEADER_SIZE];
  if (file.read(raw, HEADER_SIZE) != static_cast<int>(HEADER_SIZE)) return false;
  if (memcmp(raw, kMagic, sizeof(kMagic)) != 0 || raw[4] != FORMAT_VERSION) return false;
  char packTag[5] = {};
  memcpy(packTag, raw + 8, 4);
  if (strcmp(packTag, tag) != 0) return false;
  out.rootOffset = readU32(raw + 12);
  out.dataLength = readU32(raw + 16);
  out.dataKey = readU32(raw + 20);
  return out.dataLength > 0 && out.rootOffset < out.dataLength && file.fileSize() == HEADER_SIZE + out.dataLength;
}

bool readChunk(void* ctx, const uint32_t offset, uint8_t* buf, const uint32_t length) {
  auto* file = static_cast<HalFile*>(ctx);
  return file->seekSet(HEADER_SIZE + offset) && file->read(buf, length) == static_cast<int>(length);
}

#if HYPHENATION_PACK_HEAP
// One block the size of the trie, held while its language is in use. PSRAM on boards that have it,
// so it never competes with the reader's internal heap; malloc on the host and simulator.
uint8_t* allocate(const size_t length) {
#if defined(ESP_PLATFORM) && !defined(SIMULATOR)
  return static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM));
#else
  return static_cast<uint8_t*>(malloc(length));
#endif
}
#endif

}  // namespace

bool pathFor(const char* tag, char* out, const size_t outSize) {
  const int n = snprintf(out, outSize, "%s/%s.cphyph", DIR, tag);
  return n > 0 && static_cast<size_t>(n) < outSize;
}

bool exists(const char* path) { return Storage.exists(path); }

bool isValid(const char* path, const char* tag) {
  HalFile file;
  if (!Storage.openFileForRead("HYPH", path, file)) return false;
  Header header;
  return readHeader(file, tag, header);
}

bool load(const char* path, const char* tag, SerializedHyphenationPatterns& out) {
  release();
  HalFile file;
  if (!Storage.openFileForRead("HYPH", path, file)) return false;
  Header header;
  if (!readHeader(file, tag, header)) {
    LOG_ERR("HYPH", "%s is not a hyphenation pack for %s; removing it", path, tag);
    file.close();
    Storage.remove(path);
    return false;
  }

  if (const uint8_t* mapped = FlashBlobCache::acquire(header.dataKey, header.dataLength, readChunk, &file)) {
    loadedData = mapped;
    loadedMapped = true;
  } else {
#if HYPHENATION_PACK_HEAP
    uint8_t* data = allocate(header.dataLength);
    if (data == nullptr) {
      LOG_ERR("HYPH", "OOM: %u-byte hyphenation pack", static_cast<unsigned>(header.dataLength));
      return false;
    }
    if (!readChunk(&file, 0, data, header.dataLength)) {
      LOG_ERR("HYPH", "%s could not be read", path);
      free(data);
      return false;
    }
    if ((fnv1a::hash(data, header.dataLength) | 1u) != header.dataKey) {
      LOG_ERR("HYPH", "%s is damaged; removing it", path);
      free(data);
      file.close();
      Storage.remove(path);
      return false;
    }
    loadedData = data;
    loadedMapped = false;
#else
    LOG_ERR("HYPH", "%u-byte pack %s could not be mapped from flash", static_cast<unsigned>(header.dataLength), path);
    return false;
#endif
  }
  out = SerializedHyphenationPatterns{header.rootOffset, loadedData, header.dataLength};
  return true;
}

void release() {
  if (loadedData == nullptr) return;
  if (loadedMapped) {
    FlashBlobCache::release(const_cast<uint8_t*>(loadedData));
  } else {
    free(const_cast<uint8_t*>(loadedData));
  }
  loadedData = nullptr;
  loadedMapped = false;
}

}  // namespace HyphenationPack
