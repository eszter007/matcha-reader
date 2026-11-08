#include "PagedReaderActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "EpubReaderPercentSelectionActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MangaBookmarksActivity.h"
#include "ReaderUtils.h"
#include "activities/ActivityManager.h"
#include "util/BookProgress.h"
#include "util/BookmarkFile.h"
#include "util/SyncedPosition.h"

// Page bookmarks reuse BookmarkEntry: spine index 0, chapter page count = book page count,
// chapter progress = the bookmarked page.
namespace {
bool isPageBookmark(const BookmarkEntry& b, const uint32_t pageCount, const uint32_t page) {
  return b.computedSpineIndex == 0 && b.computedChapterPageCount == pageCount && b.computedChapterProgress == page;
}
}  // namespace

std::string PagedReaderActivity::bookmarkSummary() const {
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_PAGE_NUMBER_FORMAT), currentPage + 1);
  return buf;
}

void PagedReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (hasBook()) BookmarkFile::load(positionKey(), cachedBookmarks);
  updateBookmarkFlag();
}

void PagedReaderActivity::updateBookmarkFlag() {
  const uint32_t count = hasBook() ? pageCount() : 0;
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(),
                                      [&](const BookmarkEntry& b) { return isPageBookmark(b, count, currentPage); });
}

void PagedReaderActivity::toggleBookmark() {
  if (!hasBook()) return;
  const uint32_t count = pageCount();
  if (count == 0) return;

  const size_t countBefore = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) { return isPageBookmark(b, count, currentPage); }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != countBefore) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    BookmarkEntry entry;
    entry.percentage = static_cast<float>(currentPage) / static_cast<float>(count);
    entry.summary = bookmarkSummary();
    entry.computedSpineIndex = 0;
    entry.computedChapterPageCount = static_cast<uint16_t>(std::min<uint32_t>(count, 0xFFFF));
    entry.computedChapterProgress = static_cast<uint16_t>(std::min<uint32_t>(currentPage, 0xFFFF));
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(positionKey(), cachedBookmarks)) {
    LOG_ERR("PRA", "Failed to save bookmarks for: %s", positionKey().c_str());
  }
  showBookmarkMessage = true;
  bookmarkMessageTime = millis();
}

bool PagedReaderActivity::expireBookmarkMessage() {
  if (!showBookmarkMessage || millis() - bookmarkMessageTime < ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    return false;
  }
  showBookmarkMessage = false;
  return true;
}

void PagedReaderActivity::jumpToPercent(const int percent) {
  const uint32_t count = hasBook() ? pageCount() : 0;
  if (count == 0) return;
  goToPage(std::min<uint32_t>(static_cast<uint32_t>(std::max(percent, 0)) * count / 100, count - 1));
  requestUpdate();
}

void PagedReaderActivity::launchPercentJump() {
  const uint32_t count = hasBook() ? pageCount() : 0;
  if (count == 0) return;
  const int initialPercent = static_cast<int>((currentPage + 1) * 100 / count);
  startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent,
                                                                              /*scrubOnEnter=*/true),
                         [this, initialPercent](const ActivityResult& result) {
                           // The picker opens at the end of the visible page, which maps to the next page.
                           const int percent =
                               result.isCancelled ? initialPercent : std::get<PercentResult>(result.data).percent;
                           if (percent != initialPercent) jumpToPercent(percent);
                           requestUpdate();
                         });
}

bool PagedReaderActivity::handlePagedMenuAction(const EpubReaderMenuActivity::MenuAction action) {
  using MenuAction = EpubReaderMenuActivity::MenuAction;
  switch (action) {
    case MenuAction::GO_TO_PERCENT:
      launchPercentJump();
      return true;
    case MenuAction::TOGGLE_BOOKMARK:
      toggleBookmark();
      requestUpdate();
      return true;
    case MenuAction::BOOKMARKS:
      if (!hasBook()) return true;
      startActivityForResult(
          std::make_unique<MangaBookmarksActivity>(renderer, mappedInput, positionKey(), bookmarkToc()),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && hasBook()) {
              const uint32_t target = std::get<PageResult>(result.data).page;
              if (target < pageCount()) goToPage(target);
            }
            requestUpdate();
          });
      return true;
    case MenuAction::SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      return true;
    case MenuAction::SYNC:
      launchSync(false);
      return true;
    default:
      return false;
  }
}

ScreenshotInfo PagedReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  const uint32_t count = hasBook() ? pageCount() : 0;
  // The end-of-book screen sits at currentPage == count; report the last page.
  const uint32_t page = count > 0 ? std::min(currentPage, count - 1) : currentPage;
  if (hasBook()) {
    snprintf(info.title, sizeof(info.title), "%s", getBookTitle().c_str());
    info.totalPages = static_cast<int>(count);
    info.progressPercent = count > 0 ? static_cast<int>((page + 1) * 100 / count) : 0;
    // The percent already counts the visible page as read.
    info.endProgressPercent = info.progressPercent;
  }
  info.currentPage = static_cast<int>(page) + 1;
  return info;
}

bool PagedReaderActivity::writeSyncedPosition() {
  syncedPosition::Position position;
  if (syncedPositionPercent() < 0 || !syncedPosition::load(positionKey(), position)) return false;
  if (!writePageProgress(positionKey(), syncedPosition::pageOf(position, pageCount()))) return false;
  syncedPosition::dismiss(positionKey());
  // The page in memory too: leaving the reader saves it over the record otherwise.
  RenderLock lock(*this);
  loadProgress();
  return true;
}

int PagedReaderActivity::syncedPositionPercent() {
  syncedPosition::Position position;
  const uint32_t count = hasBook() ? pageCount() : 0;
  if (count == 0 || !syncedPosition::load(positionKey(), position)) return -1;
  const uint32_t targetPage = syncedPosition::pageOf(position, count);
  // Read past it here since it was fetched: going there would be going back. Pages, not the rounded
  // percentages, which put neighbouring pages of a long book on the same whole number.
  uint32_t ownPage = 0;
  if (readPageProgress(positionKey(), ownPage) && targetPage <= ownPage) {
    syncedPosition::dismiss(positionKey());
    return -1;
  }
  return static_cast<int>(targetPage * 100 / count);
}

bool PagedReaderActivity::startShortcutSync() { return KOREADER_STORE.hasCredentials() && launchSync(true); }

bool PagedReaderActivity::launchSync(const bool withClockAndInsights) {
  if (!hasBook()) return false;
  saveProgress();
  PagedSyncBook paged;
  paged.page = currentPage;
  paged.pageCount = pageCount();
  paged.title = getBookTitle();
  paged.author = getBookAuthor();
  fillSyncBook(paged);
  auto sync = makeUniqueNoThrow<KOReaderSyncActivity>(renderer, mappedInput, positionKey(), std::move(paged));
  if (!sync) {
    LOG_ERR("SYNC", "OOM: sync activity");
    return false;
  }
  if (withClockAndInsights) sync->alsoSyncClockAndInsights();
  activityManager.replaceActivity(std::move(sync));
  return true;
}
