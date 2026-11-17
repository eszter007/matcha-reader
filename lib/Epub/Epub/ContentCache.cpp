#include "ContentCache.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace content_cache {
namespace {
// Reading state, kept when the book's file is replaced.
constexpr const char* KEPT_ON_UPDATE[] = {"progress.bin", "readerprefs.bin", "language.bin", "pace.bin", "synced.bin"};
// The book file this cache was built from: its size, modified time and a hash of its last bytes. An
// EPUB ends with the ZIP central directory, which lists every entry's size, CRC and time. A book copied
// over USB or edited on a computer is caught at its next open; one only touched is rebuilt needlessly.
constexpr char SOURCE_STAMP_NAME[] = "source.bin";
constexpr char SOURCE_STAMP_FILE[] = "/source.bin";
constexpr uint32_t SOURCE_STAMP_VERSION = 1;
constexpr size_t SOURCE_TAIL_BYTES = 1024;

struct SourceStamp {
  uint32_t version = SOURCE_STAMP_VERSION;
  uint32_t tailHash = 0;
  uint64_t size = 0;
  uint32_t modified = 0;  // HalFile::modifiedStamp()
  uint32_t reserved = 0;
};

bool readSourceStamp(const std::string& bookPath, SourceStamp& out) {
  HalFile file;
  if (!Storage.openFileForRead("EBP", bookPath, file)) return false;
  out.size = file.fileSize64();
  out.modified = file.modifiedStamp();
  const uint64_t start = out.size > SOURCE_TAIL_BYTES ? out.size - SOURCE_TAIL_BYTES : 0;
  if (!file.seek64(start)) return false;
  uint32_t hash = 2166136261u;  // FNV-1a
  uint8_t chunk[128];
  for (uint64_t left = out.size - start; left > 0;) {
    const int got = file.read(chunk, static_cast<size_t>(std::min<uint64_t>(left, sizeof(chunk))));
    if (got <= 0) return false;
    for (int i = 0; i < got; ++i) hash = (hash ^ chunk[i]) * 16777619u;
    left -= static_cast<uint64_t>(got);
  }
  out.tailHash = hash;
  return true;
}

bool loadSavedStamp(const std::string& cachePath, SourceStamp& out) {
  HalFile file;
  if (!Storage.openFileForRead("EBP", cachePath + SOURCE_STAMP_FILE, file)) return false;
  return file.read(&out, sizeof(out)) == static_cast<int>(sizeof(out)) && out.version == SOURCE_STAMP_VERSION;
}

void saveStamp(const std::string& cachePath, const SourceStamp& stamp) {
  HalFile file;
  if (!Storage.openFileForWrite("EBP", cachePath + SOURCE_STAMP_FILE, file)) return;
  if (file.write(&stamp, sizeof(stamp)) != sizeof(stamp)) LOG_ERR("EBP", "Failed to write source stamp");
}

bool isKept(const char* name) {
  return std::any_of(std::begin(KEPT_ON_UPDATE), std::end(KEPT_ON_UPDATE),
                     [name](const char* kept) { return strcmp(name, kept) == 0; });
}
}  // namespace

bool clear(const std::string& cachePath) {
  bool ok = true;
  char name[64];
  std::string path;
  // A second walk picks up anything the first skipped while entries were being removed under it:
  // a layout left behind would show the old book's text.
  for (int pass = 0; pass < 2; ++pass) {
    HalFile dir = Storage.open(cachePath.c_str());
    if (!dir || !dir.isDirectory()) return true;  // nothing cached yet
    bool removedAny = false;
    for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      const size_t nameLength = entry.getName(name, sizeof(name));
      const bool isDir = entry.isDirectory();
      entry.close();          // before removing it
      if (nameLength == 0) {  // too long for the buffer: an empty name would mean the folder itself
        ok = false;
        continue;
      }
      // The stamp goes last, below: while it names the old file, a failed clear is retried.
      if (isKept(name) || strcmp(name, SOURCE_STAMP_NAME) == 0) continue;
      path = cachePath + "/" + name;
      if (isDir ? Storage.removeDir(path.c_str()) : Storage.remove(path.c_str())) {
        removedAny = true;
      } else {
        LOG_ERR("EBP", "Failed to remove %s", path.c_str());
        ok = false;
      }
    }
    if (!removedAny) break;
  }
  if (!ok) return false;
  path = cachePath + SOURCE_STAMP_FILE;
  if (Storage.exists(path.c_str())) Storage.remove(path.c_str());  // the next open stamps the new file
  LOG_DBG("EBP", "Content cache cleared, reading state kept: %s", cachePath.c_str());
  return true;
}

// A cache from before the stamp existed is trusted once and stamped. A book file that cannot be read
// now changes nothing and writes nothing, so a passing SD hiccup does not cost the book its caches.
void checkSource(const std::string& bookPath, const std::string& cachePath) {
  if (!Storage.exists(cachePath.c_str())) return;  // stamp() records a new cache
  SourceStamp now;
  if (!readSourceStamp(bookPath, now)) return;
  SourceStamp saved;
  const bool hadStamp = loadSavedStamp(cachePath, saved);
  if (hadStamp && saved.size == now.size && saved.modified == now.modified && saved.tailHash == now.tailHash) return;
  if (hadStamp) {
    LOG_INF("EBP", "Book file changed; rebuilding its cache, keeping the reading position: %s", bookPath.c_str());
    if (!clear(cachePath)) return;  // stamp only a cache that matches the file
  }
  saveStamp(cachePath, now);
}

void stamp(const std::string& bookPath, const std::string& cachePath) {
  SourceStamp now;
  if (readSourceStamp(bookPath, now)) saveStamp(cachePath, now);
}
}  // namespace content_cache
