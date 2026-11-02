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

#include "stats/StatsSync.h"
#include "util/BookProgress.h"
#include "util/BookWalk.h"
#include "util/RemoteProgress.h"

namespace syncedPosition {

namespace {

constexpr uint8_t FILE_VERSION = 2;
// An XPath or a page number; a longer one is not a position this reader can place.
constexpr size_t MAX_PROGRESS_BYTES = 400;
// Each fetch is one request to the sync server: bounded per sync, the rest come with the next one.
constexpr int MAX_FETCHES_PER_SYNC = 30;
constexpr uint32_t MAX_FETCH_MS = 30000;

// [0] version, [1-4] percentage, [5] byShare, [6] handled, [7-10] account key, [11-12] length, progress.
constexpr size_t HEAD_BYTES = 13;
constexpr size_t HANDLED_AT = 6;

std::string filePath(const std::string& path) {
  const std::string cache = bookCachePath(path);
  return cache.empty() ? std::string() : cache + "/synced.bin";
}

// The record for the current account, handled or not; false when there is none.
bool read(const std::string& path, Position& out, bool& handled) {
  const std::string file = filePath(path);
  if (file.empty() || !Storage.exists(file.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead("SYNCPOS", file, f)) return false;
  uint8_t head[HEAD_BYTES];
  if (f.read(head, sizeof(head)) != sizeof(head) || head[0] != FILE_VERSION) return false;
  uint32_t account;
  uint16_t length;
  memcpy(&out.percentage, head + 1, sizeof(float));
  out.byShare = head[5] != 0;
  handled = head[HANDLED_AT] != 0;
  memcpy(&account, head + 7, sizeof(account));
  memcpy(&length, head + 11, sizeof(length));
  // Another account's position is not this one's: a switch of account leaves it unused. The sync
  // account's own key, not Insights' (0 for every account with "This device"); 0 is no account.
  if (account == 0 || account != statsSync::accountKey()) return false;
  if (length > MAX_PROGRESS_BYTES || !std::isfinite(out.percentage)) return false;
  out.progress.resize(length);
  return f.read(reinterpret_cast<uint8_t*>(&out.progress[0]), length) == length;
}

bool save(const std::string& path, const KOReaderProgress& progress, const bool byShare) {
  const std::string cache = bookCachePath(path);
  if (cache.empty() || progress.progress.size() > MAX_PROGRESS_BYTES) return false;
  Storage.mkdir(cache.c_str());
  HalFile f;
  if (!Storage.openFileForWrite("SYNCPOS", cache + "/synced.bin", f)) return false;
  uint8_t head[HEAD_BYTES];
  head[0] = FILE_VERSION;
  memcpy(head + 1, &progress.percentage, sizeof(float));
  head[5] = byShare ? 1 : 0;
  head[HANDLED_AT] = 0;
  const uint32_t account = statsSync::accountKey();
  if (account == 0) return false;
  memcpy(head + 7, &account, sizeof(account));
  const auto length = static_cast<uint16_t>(progress.progress.size());
  memcpy(head + 11, &length, sizeof(length));
  return f.write(head, sizeof(head)) == sizeof(head) &&
         f.write(reinterpret_cast<const uint8_t*>(progress.progress.data()), length) == length;
}

// The server's record for a book: by the configured document matching, then by its file name (Binary
// matching has no content hash for a manga folder, and the feed may know a book by its name only), then
// (a manga) by a twin file.
bool fetch(const std::string& path, KOReaderProgress& out, bool& byShare) {
  byShare = false;
  const std::string byName = KOReaderDocumentId::calculateFromFilename(path);
  std::string hash = byName;
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::BINARY) {
    hash = KOReaderDocumentId::calculate(path);
    if (!hash.empty() && KOReaderSyncClient::getProgress(hash, out) == KOReaderSyncClient::OK) return true;
  }
  if (!byName.empty() && KOReaderSyncClient::getProgress(byName, out) == KOReaderSyncClient::OK) return true;
  std::string twins[KOReaderDocumentId::MAX_TWINS];
  const size_t count = KOReaderDocumentId::twinFilenameHashes(path, twins);
  for (size_t i = 0; i < count; i++) {
    if (twins[i] == byName || twins[i] == hash) continue;
    if (KOReaderSyncClient::getProgress(twins[i], out) == KOReaderSyncClient::OK) {
      byShare = true;
      return true;
    }
  }
  return false;
}

}  // namespace

bool load(const std::string& path, Position& out) {
  bool handled = false;
  return read(path, out, handled) && !handled;
}

void dismiss(const std::string& path) {
  const std::string file = filePath(path);
  if (file.empty() || !Storage.exists(file.c_str())) return;
  // Kept, flagged: the record is what tells the next sync this position was already fetched.
  constexpr size_t RECORD_BYTES = HEAD_BYTES + MAX_PROGRESS_BYTES;
  auto record = makeUniqueNoThrow<uint8_t[]>(RECORD_BYTES);  // heap: over the stack budget
  int size = 0;
  if (record) {
    HalFile f;
    if (Storage.openFileForRead("SYNCPOS", file, f)) size = f.read(record.get(), RECORD_BYTES);
  }
  bool kept = false;
  if (size >= static_cast<int>(HEAD_BYTES)) {
    record[HANDLED_AT] = 1;
    HalFile f;
    kept = Storage.openFileForWrite("SYNCPOS", file, f) && f.write(record.get(), size) == static_cast<size_t>(size);
  }
  if (!kept) Storage.remove(file.c_str());  // gone is handled too; fetched again only if the feed moves
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

void fetchAhead(const remoteProgress::Feed& feed) {
  const uint32_t start = millis();
  int fetched = 0;
  bool deferred = false;
  // A changed book whose new position is not saved this sync loses its old record: the feed cursor has
  // moved past the change, so a kept record would never be refreshed. Without one the next sync fetches.
  const auto dropStale = [&feed](const std::string& path) {
    const std::string file = filePath(path);
    if (feed.changed(path) && !file.empty() && Storage.exists(file.c_str())) Storage.remove(file.c_str());
  };
  // Every book on the card, manga folders included (the library index leaves those out).
  bookwalk::forEachBook("/", 0, [&](const std::string& path) {
    if (bookCachePath(path).empty()) return true;  // not a book
    if (fetched >= MAX_FETCHES_PER_SYNC || millis() - start > MAX_FETCH_MS) {
      // Past the budget: only the cheap clean-up above, the fetches come with the next sync.
      deferred = true;
      dropStale(path);
      return true;
    }
    const int remote = feed.percent(path);
    if (remote <= 0 || remote >= 100) return true;  // unknown, or finished there: a badge, not a position
    const int local = std::max(0, loadOwnBookProgress(path));
    if (remote <= local) return true;
    // Fetched before, and the server's record has not changed since: nothing new to fetch.
    Position kept;
    bool handled = false;
    if (read(path, kept, handled) && !feed.changed(path)) return true;
    KOReaderProgress progress;
    bool byShare = false;
    fetched++;
    if (!fetch(path, progress, byShare) || static_cast<int>(progress.percentage * 100.0f) <= local) {
      dropStale(path);
      return true;
    }
    if (!save(path, progress, byShare)) {
      LOG_ERR("SYNCPOS", "Could not keep the position of %s", path.c_str());
      dropStale(path);
    }
    return true;
  });
  if (deferred) LOG_INF("SYNCPOS", "More books ahead elsewhere; the next sync fetches them");
  LOG_INF("SYNCPOS", "Fetched %d positions from elsewhere", fetched);
}

}  // namespace syncedPosition
