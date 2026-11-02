#include "SyncedPosition.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "util/BookProgress.h"
#include "util/BookWalk.h"
#include "util/RemoteProgress.h"

namespace syncedPosition {

namespace {

constexpr uint8_t FILE_VERSION = 1;
// An XPath or a page number; a longer one is not a position this reader can place.
constexpr size_t MAX_PROGRESS_BYTES = 400;
// Each fetch is one request to the sync server: bounded per sync, the rest come with the next one.
constexpr int MAX_FETCHES_PER_SYNC = 30;
constexpr uint32_t MAX_FETCH_MS = 30000;

std::string filePath(const std::string& path) {
  const std::string cache = bookCachePath(path);
  return cache.empty() ? std::string() : cache + "/synced.bin";
}

bool save(const std::string& path, const KOReaderProgress& progress, const bool byShare) {
  const std::string cache = bookCachePath(path);
  if (cache.empty() || progress.progress.size() > MAX_PROGRESS_BYTES) return false;
  Storage.mkdir(cache.c_str());
  HalFile f;
  if (!Storage.openFileForWrite("SYNCPOS", cache + "/synced.bin", f)) return false;
  uint8_t head[8];
  head[0] = FILE_VERSION;
  memcpy(head + 1, &progress.percentage, sizeof(float));
  head[5] = byShare ? 1 : 0;
  const auto length = static_cast<uint16_t>(progress.progress.size());
  memcpy(head + 6, &length, sizeof(length));
  return f.write(head, sizeof(head)) == sizeof(head) &&
         f.write(reinterpret_cast<const uint8_t*>(progress.progress.data()), length) == length;
}

// The server's record for a book: by the configured document matching, else (a manga) by a twin file.
bool fetch(const std::string& path, KOReaderProgress& out, bool& byShare) {
  const std::string hash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                               ? KOReaderDocumentId::calculateFromFilename(path)
                               : KOReaderDocumentId::calculate(path);
  byShare = false;
  if (!hash.empty() && KOReaderSyncClient::getProgress(hash, out) == KOReaderSyncClient::OK) return true;
  std::string twins[KOReaderDocumentId::MAX_TWINS];
  const size_t count = KOReaderDocumentId::twinFilenameHashes(path, twins);
  for (size_t i = 0; i < count; i++) {
    if (twins[i] == hash) continue;
    if (KOReaderSyncClient::getProgress(twins[i], out) == KOReaderSyncClient::OK) {
      byShare = true;
      return true;
    }
  }
  return false;
}

}  // namespace

bool load(const std::string& path, Position& out) {
  const std::string file = filePath(path);
  if (file.empty() || !Storage.exists(file.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead("SYNCPOS", file, f)) return false;
  uint8_t head[8];
  if (f.read(head, sizeof(head)) != sizeof(head) || head[0] != FILE_VERSION) return false;
  uint16_t length;
  memcpy(&out.percentage, head + 1, sizeof(float));
  out.byShare = head[5] != 0;
  memcpy(&length, head + 6, sizeof(length));
  if (length > MAX_PROGRESS_BYTES || !std::isfinite(out.percentage)) return false;
  out.progress.resize(length);
  return f.read(reinterpret_cast<uint8_t*>(&out.progress[0]), length) == length;
}

void clear(const std::string& path) {
  const std::string file = filePath(path);
  if (!file.empty() && Storage.exists(file.c_str())) Storage.remove(file.c_str());
}

uint32_t pageOf(const Position& position, const uint32_t pageCount) {
  const std::string& text = position.progress;
  // A twin's page numbers belong to a different file; only the share of the book carries over.
  const unsigned long number =
      position.byShare || text.empty() || text.size() > 9 ||
              !std::all_of(text.begin(), text.end(), [](const char c) { return c >= '0' && c <= '9'; })
          ? 0
          : strtoul(text.c_str(), nullptr, 10);
  uint32_t page;
  if (number >= 1) {
    page = static_cast<uint32_t>(number - 1);
  } else {
    const float share = std::clamp(position.percentage, 0.0f, 1.0f);
    page = static_cast<uint32_t>(share * static_cast<float>(pageCount));
  }
  return pageCount > 0 && page >= pageCount ? pageCount - 1 : page;
}

void fetchAhead() {
  // Heap, not stack: the snapshot of the progress index can hold 20 KB.
  auto elsewhere = makeUniqueNoThrow<remoteProgress::Snapshot>();
  if (!elsewhere) {
    LOG_ERR("SYNCPOS", "OOM: synced positions");
    return;
  }
  elsewhere->load();
  const uint32_t start = millis();
  int fetched = 0;
  // Every book on the card, manga folders included (the library index leaves those out).
  bookwalk::forEachBook("/", 0, [&](const std::string& path) {
    if (fetched >= MAX_FETCHES_PER_SYNC || millis() - start > MAX_FETCH_MS) {
      LOG_INF("SYNCPOS", "More books ahead elsewhere; the next sync fetches them");
      return false;
    }
    if (bookCachePath(path).empty()) return true;  // not a book
    const int remote = elsewhere->percent(path);
    if (remote <= 0 || remote >= 100) return true;  // unknown, or finished there: a badge, not a position
    const int local = std::max(0, loadOwnBookProgress(path));
    if (remote <= local) return true;
    Position kept;
    if (load(path, kept) && static_cast<int>(kept.percentage * 100.0f) == remote) return true;  // already fetched
    KOReaderProgress progress;
    bool byShare = false;
    fetched++;
    if (!fetch(path, progress, byShare) || static_cast<int>(progress.percentage * 100.0f) <= local) return true;
    if (!save(path, progress, byShare)) LOG_ERR("SYNCPOS", "Could not keep the position of %s", path.c_str());
    return true;
  });
  LOG_INF("SYNCPOS", "Fetched %d positions from elsewhere", fetched);
}

}  // namespace syncedPosition
