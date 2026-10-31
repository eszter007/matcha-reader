#include "MatchaContainer.h"

#include <strings.h>

#include <cstring>

namespace matcha {
namespace {

constexpr uint32_t kZipLocalSignature = 0x04034b50;
constexpr char kIndexName[] = "matcha.idx";
constexpr uint32_t kIndexVersion = 1;
constexpr size_t kIndexHeader = 12;
constexpr size_t kRecordBytes = 12;

uint32_t readLe32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t fnv1a(const char* s, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) {
    h ^= static_cast<uint8_t>(s[i]);
    h *= 16777619u;
  }
  return h;
}

}  // namespace

bool splitPath(const char* path, std::string& container, std::string& entry) {
  // Case-insensitive, like FsHelpers::hasMatchaExtension: FAT copies often upper-case the name.
  constexpr size_t extLen = sizeof(kExtension) - 1;
  const char* hit = nullptr;
  for (const char* p = strchr(path, '.'); p && !hit; p = strchr(p + 1, '.')) {
    if (strncasecmp(p, kExtension, extLen) == 0 && p[extLen] == '/') hit = p;
  }
  if (!hit) return false;
  const size_t split = static_cast<size_t>(hit - path) + extLen;
  container.assign(path, split);
  entry.assign(path + split + 1);
  return !entry.empty();
}

bool findEntry(const ReadAt readAt, void* ctx, const std::string& entry, Entry& out) {
  // The index is the first local entry, stored, so its data follows the 30-byte local header.
  uint8_t local[30];
  if (!readAt(ctx, 0, local, sizeof(local)) || readLe32(local) != kZipLocalSignature || readLe16(local + 8) != 0) {
    return false;
  }
  const uint16_t nameLen = readLe16(local + 26);
  const uint16_t extraLen = readLe16(local + 28);
  char name[sizeof(kIndexName)] = {};
  if (nameLen != sizeof(kIndexName) - 1 || !readAt(ctx, 30, name, nameLen) || memcmp(name, kIndexName, nameLen) != 0) {
    return false;
  }
  const uint32_t indexStart = 30u + nameLen + extraLen;
  uint8_t header[kIndexHeader];
  if (!readAt(ctx, indexStart, header, sizeof(header)) || memcmp(header, "MTCH", 4) != 0 ||
      readLe32(header + 4) != kIndexVersion) {
    return false;
  }
  const uint32_t count = readLe32(header + 8);

  size_t len = entry.size();
  while (len > 0 && entry[len - 1] == '/') len--;
  // A name with an extension is a file, so only an extensionless name is also tried as a
  // directory ("panels" -> "panels/"); this keeps a miss on a page or crop to one search.
  const size_t base = entry.find_last_of('/', len ? len - 1 : 0);
  const bool maybeDir = entry.find('.', base == std::string::npos ? 0 : base + 1) >= len;
  uint32_t keys[2] = {fnv1a(entry.data(), len), 0};
  if (maybeDir) keys[1] = fnv1a((entry.substr(0, len) + '/').c_str(), len + 1);
  for (int k = 0; k < (maybeDir ? 2 : 1); k++) {
    const uint32_t key = keys[k];
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      uint8_t rec[kRecordBytes];
      if (!readAt(ctx, indexStart + kIndexHeader + mid * kRecordBytes, rec, sizeof(rec))) return false;
      const uint32_t hash = readLe32(rec);
      if (hash == key) {
        out.offset = readLe32(rec + 4);
        out.size = readLe32(rec + 8);
        return true;
      }
      if (hash < key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
  }
  return false;
}

}  // namespace matcha
