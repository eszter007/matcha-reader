#pragma once

#include <MangaPanel.h>

#include <string>
#include <utility>
#include <vector>

#include "../../BookmarkEntry.h"
#include "EpubReaderMenuActivity.h"
#include "ReaderActivity.h"

struct PagedSyncBook;

// Readers whose position is one page index (XTC, manga). Owns that index and the pieces that
// only need it: bookmarks, the synced position, sync, percent jumps and the screenshot info.
class PagedReaderActivity : public ReaderActivity {
 protected:
  using ReaderActivity::ReaderActivity;

  uint32_t currentPage = 0;  // 0-based; pageCount() while the end-of-book screen shows

  std::vector<BookmarkEntry> cachedBookmarks;
  bool currentPageBookmarked = false;
  bool showBookmarkMessage = false;
  bool bookmarkRemoved = false;  // the last toggle removed a bookmark (popup text)
  unsigned long bookmarkMessageTime = 0UL;
  bool pendingScreenshot = false;
  // Swallows the Confirm release that opened the book, so it doesn't open the reader menu.
  bool ignoreNextConfirmRelease = false;

  virtual uint32_t pageCount() const = 0;
  // File or folder that bookmarks, the synced position and the saved page are keyed by.
  virtual std::string positionKey() const = 0;
  virtual void saveProgress() const = 0;
  virtual void loadProgress() = 0;
  // Moves to a page known to be in range; readers with per-page state reset it here.
  virtual void goToPage(uint32_t page) { currentPage = page; }
  virtual std::string bookmarkSummary() const;
  virtual std::vector<manga::TocEntry> bookmarkToc() const { return {}; }
  // Metadata beyond page, count, title and author.
  virtual void fillSyncBook(PagedSyncBook&) const {}

  void loadCachedBookmarks();
  void updateBookmarkFlag();
  // Bookmarks the page, or removes its bookmark, and shows the popup.
  void toggleBookmark();
  // Hides the bookmark popup once it has been up long enough. True when it was hidden.
  bool expireBookmarkMessage();
  // Handles the menu actions every paged reader shares. False for the others.
  bool handlePagedMenuAction(EpubReaderMenuActivity::MenuAction action);
  void launchPercentJump();
  bool launchSync(bool withClockAndInsights);

  void jumpToPercent(int percent) override;
  bool writeSyncedPosition() override;
  int syncedPositionPercent() override;
  bool startShortcutSync() override;
  bool isAtEndOfBook() const override { return hasBook() && currentPage >= pageCount(); }

 public:
  ScreenshotInfo getScreenshotInfo() const override;
};
