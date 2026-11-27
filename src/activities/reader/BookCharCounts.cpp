#include "BookCharCounts.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <VisibleTextStreamer.h>

#include <cstring>
#include <new>

namespace {
constexpr uint8_t kVersion = 1;
// version (1) + spine count (2), then 4 bytes per spine.
constexpr size_t kHeaderSize = 3;

std::string pathFor(const Epub& epub) { return epub.getCachePath() + "/chars.bin"; }
}  // namespace

bool BookCharCounts::load(const Epub& epub) {
  const int spines = epub.getSpineItemsCount();
  if (spines <= 0) return false;
  // 4 bytes per spine for the session; reserve-free: assign() is one allocation.
  counts.clear();
  counts.shrink_to_fit();
  if (ESP.getMaxAllocHeap() < static_cast<size_t>(spines) * sizeof(uint32_t) + 4096) {
    LOG_ERR("CHARS", "No heap for %d spine counts", spines);
    return false;
  }
  counts.assign(static_cast<size_t>(spines), UNKNOWN);
  dirty = false;

  HalFile f;
  if (!Storage.openFileForRead("CHARS", pathFor(epub), f)) return true;
  uint8_t header[kHeaderSize];
  if (f.read(header, sizeof(header)) != static_cast<int>(sizeof(header)) || header[0] != kVersion) return true;
  const int stored = header[1] | (header[2] << 8);
  if (stored != spines) return true;  // another book at this path: count again
  for (int i = 0; i < spines; i++) {
    uint8_t b[4];
    if (f.read(b, sizeof(b)) != static_cast<int>(sizeof(b))) {
      counts.assign(static_cast<size_t>(spines), UNKNOWN);
      return true;
    }
    uint32_t v;
    memcpy(&v, b, sizeof(v));
    counts[static_cast<size_t>(i)] = v;
  }
  return true;
}

uint32_t BookCharCounts::get(const int spine) const {
  return spine >= 0 && static_cast<size_t>(spine) < counts.size() ? counts[static_cast<size_t>(spine)] : UNKNOWN;
}

void BookCharCounts::set(const int spine, const uint32_t chars) {
  if (spine < 0 || static_cast<size_t>(spine) >= counts.size() || counts[static_cast<size_t>(spine)] == chars) return;
  counts[static_cast<size_t>(spine)] = chars;
  dirty = true;
}

int BookCharCounts::nextUnknown(const int from) const {
  const int n = static_cast<int>(counts.size());
  for (int k = 0; k < n; k++) {
    const int i = (std::max(from, 0) + k) % n;
    if (counts[static_cast<size_t>(i)] == UNKNOWN) return i;
  }
  return -1;
}

void BookCharCounts::save(const Epub& epub) {
  if (!dirty || counts.empty()) return;
  HalFile f;
  if (!Storage.openFileForWrite("CHARS", pathFor(epub), f)) return;
  const uint16_t n = static_cast<uint16_t>(counts.size());
  const uint8_t header[kHeaderSize] = {kVersion, static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>(n >> 8)};
  bool ok = f.write(header, sizeof(header)) == sizeof(header);
  for (const uint32_t v : counts) {
    uint8_t b[4];
    memcpy(b, &v, sizeof(b));
    ok = ok && f.write(b, sizeof(b)) == sizeof(b);
  }
  if (ok) dirty = false;
}

bool BookCharCounts::count(const std::shared_ptr<Epub>& epub, const int spine, bool (*cancel)(void*), void* ctx,
                           uint32_t& out) {
  const auto href = epub->getSpineItem(spine).href;
  if (href.empty()) return false;
  // A few hundred bytes of scanner state; on the heap, off the loop task's stack.
  auto streamer = makeUniqueNoThrow<koreader_sync::ParagraphStreamer>(SIZE_MAX);
  if (!streamer) {
    LOG_ERR("CHARS", "OOM: streamer");
    return false;
  }
  if (!epub->readItemContentsToStream(href, *streamer, 1024, false, cancel, ctx)) return false;
  out = streamer->getReadableChars();
  return true;
}
