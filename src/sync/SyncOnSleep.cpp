#include <CrossPointPosition.h>
#include <Epub.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <ProgressMapper.h>

#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "SyncEngine.h"
#include "activities/reader/ProgressFile.h"

namespace {
struct SleepBook {
  std::string path;
  CrossPointPosition position;
  KOReaderProgress progress;  // filled by prepareSleepSync()
};
// Lives only between the reader's pre-sleep hook and syncOnSleep().
std::optional<SleepBook> sleepBook;
}  // namespace

KOReaderProgress syncEngine::localProgress(const std::string& documentHash, const std::string& bookPath, Epub* epub,
                                           const CrossPointPosition& position, const SavedProgressPosition& saved) {
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = saved.xpath;
  progress.percentage = saved.percentage;

  // Rich position for server profiles that explicitly support the CrossPoint extension.
  // The HTTP client enforces the same boundary before serializing the extension.
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition pos;
    const float pct = saved.percentage < 0.0f ? 0.0f : saved.percentage > 1.0f ? 1.0f : saved.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(position.spineIndex);
    pos.pageNumber = static_cast<uint16_t>(position.pageNumber);
    pos.totalPages = static_cast<uint16_t>(position.totalPages > 0 ? position.totalPages : 1);
    if (position.hasParagraphIndex) {
      pos.paragraphIndex = position.paragraphIndex;
    }
    pos.xpath = saved.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    const auto lastSlash = bookPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? bookPath.substr(lastSlash + 1) : bookPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
      // CrossPoint and custom servers store it; plain KOSync has no such field.
      if (KOREADER_STORE.supportsRichProgress()) meta.language = epub->getLanguage();
      if (KOREADER_STORE.supportsExtendedMetadata()) {
        Epub::SyncMetadata syncMetadata;
        if (epub->loadSyncMetadata(syncMetadata)) {
          meta.isbn = std::move(syncMetadata.isbn);
          meta.asin = std::move(syncMetadata.asin);
          meta.series = std::move(syncMetadata.series);
          meta.seriesIndex = syncMetadata.seriesIndex;
        } else {
          LOG_DBG("SYNC", "Could not read extended EPUB metadata; sending core metadata only");
        }
      }
    } else {
      LOG_ERR("SYNC", "Epub unavailable for metadata; sending filename only");
    }
    // Plugin sidecar fields ("<book>.meta.json", written at download time via
    // the catalog sidecar mechanism or /api/plugin-fs) ride along so a custom
    // sync server can route progress by a service book id.
    // A missing sidecar (the common case) leaves extraJson empty.
    Storage.readFileToString("SYNC", bookPath + ".meta.json", 2 * 1024, meta.extraJson);
    progress.metadata = std::move(meta);
  }
  return progress;
}

void syncEngine::noteSleepBook(const std::string& bookPath, const CrossPointPosition& position) {
  sleepBook = SleepBook{bookPath, position, {}};
}

void syncEngine::sleepBookMoved(const std::string& from, const std::string& to) {
  if (sleepBook && sleepBook->path == from) sleepBook->path = to;
}

bool syncEngine::sleepSyncPending() { return sleepBook.has_value(); }

bool syncEngine::prepareSleepSync() {
  if (!sleepBook) return false;
  // Metadata only, as the manual sync does: no CSS, and no cache rebuild when it is missing.
  std::shared_ptr<Epub> epub(new (std::nothrow) Epub(sleepBook->path, "/.crosspoint"));
  if (!epub) {
    LOG_ERR("SYNC", "Sync on sleep: OOM loading the book");
    sleepBook.reset();
    return false;
  }
  epub->setupCacheDir();
  if (!epub->load(false, true)) {
    LOG_ERR("SYNC", "Sync on sleep: could not load the book");
    sleepBook.reset();
    return false;
  }
  sleepBook->progress = localProgress("", sleepBook->path, epub.get(), sleepBook->position,
                                      ProgressMapper::toSavedProgress(epub, sleepBook->position));
  return true;
}

namespace {
// Spine, page, chapter page count and text offset from progress.bin, in either layout (see ProgressFile).
bool readSavedPosition(const std::string& cachePath, CrossPointPosition& out) {
  HalFile f;
  if (!Storage.openFileForRead("SYNC", cachePath + "/progress.bin", f)) return false;
  uint8_t data[13];
  const int size = f.read(data, sizeof(data));
  if (size < 4) return false;
  const auto le32 = [&data](const int at) {
    return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8) |
           (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
  };
  const int page = data[2] | (data[3] << 8);
  const int count = size >= 6 ? (data[4] | (data[5] << 8)) : 0;
  out = {data[0] | (data[1] << 8), page == UINT16_MAX ? 0 : page, count > 0 ? count : 1};
  out.hasResolvedSpineIndex = true;
  out.hasMappedPage = true;
  const int offsetAt = ProgressFile::isStockLayout(size) ? (size >= 10 ? 6 : -1) : (size >= 13 ? 9 : -1);
  if (offsetAt >= 0) {
    out.visibleTextOffset = le32(offsetAt);
    out.hasVisibleTextOffset = true;
  }
  return true;
}
}  // namespace

bool syncEngine::savedBookProgress(const std::string& bookPath, KOReaderProgress& out) {
  // Metadata only, as the manual sync does: no CSS, and no cache rebuild when it is missing.
  std::shared_ptr<Epub> epub(new (std::nothrow) Epub(bookPath, "/.crosspoint"));
  if (!epub) {
    LOG_ERR("SYNC", "OOM loading %s", bookPath.c_str());
    return false;
  }
  CrossPointPosition position{};
  if (!readSavedPosition(epub->getCachePath(), position)) return false;
  epub->setupCacheDir();
  if (!epub->load(false, true)) {
    LOG_ERR("SYNC", "Could not load %s", bookPath.c_str());
    return false;
  }
  out = localProgress("", bookPath, epub.get(), position, ProgressMapper::toSavedProgress(epub, position));
  return true;
}

bool syncEngine::uploadUnlessBehind(const std::string& bookPath, KOReaderProgress progress) {
  const Pull remote = pull(bookPath, false);
  if (remote.documentHash.empty() || !remote.extensionsSynced) {
    LOG_ERR("SYNC", "Stopped before progress: %s", bookPath.c_str());
    return false;
  }
  // Never move the server backwards without the user: a further remote position waits for a manual sync.
  const bool upload = remote.result == KOReaderSyncClient::NOT_FOUND ||
                      (remote.result == KOReaderSyncClient::OK && progress.percentage >= remote.remote.percentage);
  if (!upload) {
    LOG_INF("SYNC", "Progress not uploaded (result=%d remote=%.4f local=%.4f)", remote.result, remote.remote.percentage,
            progress.percentage);
    return remote.result == KOReaderSyncClient::OK;
  }
  progress.document = remote.documentHash;
  const KOReaderSyncClient::Error result = push(progress);
  LOG_INF("SYNC", "Progress upload result=%d", result);
  return result == KOReaderSyncClient::OK;
}

void syncEngine::syncOnSleep() {
  if (!sleepBook) return;
  const SleepBook book = std::move(*sleepBook);
  sleepBook.reset();
  LOG_INF("SYNC", "Sync on sleep: contacting the server");
  uploadUnlessBehind(book.path, book.progress);
}
