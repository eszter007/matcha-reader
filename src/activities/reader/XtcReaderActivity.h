/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "BookmarkEntry.h"
#include "ReaderActivity.h"
#include "activities/reader/EpubReaderMenuActivity.h"

class XtcReaderActivity final : public ReaderActivity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;

  // Reader menu / bookmarks / screenshot state (page-based, mirrors the manga reader).
  std::vector<BookmarkEntry> cachedBookmarks;
  bool currentPageBookmarked = false;
  bool showBookmarkMessage = false;
  bool bookmarkRemoved = false;
  uint32_t bookmarkMessageTime = 0;
  bool pendingScreenshot = false;
  // Swallow the Confirm release that opened this book (from the library) so it doesn't
  // immediately open the reader menu -- which would freePageBuffer() mid first-render.
  bool ignoreNextConfirmRelease = true;

  void launchMenu();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void addBookmark();
  void loadCachedBookmarks();
  void updateBookmarkFlag();

  // Page pixel buffer, allocated once (a full XTH 2-bit page is ~104KB) and reused for every page
  // turn. Page dimensions are fixed per file, so one buffer serves all pages -- avoids a large
  // malloc/free on every turn (slow) and grabs the block once while the heap is freshest.
  uint8_t* pageBuffer = nullptr;
  size_t pageBufferSize = 0;
  bool ensurePageBuffer(size_t needed);
  void freePageBuffer();
  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage();
  void renderStatusBarOverlay(StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress() const;
  void loadProgress();

  bool loadBook() override;
  bool hasBook() const override { return xtc != nullptr; }
  std::string getBookTitle() const override { return xtc ? xtc->getTitle() : ""; }
  std::string getBookAuthor() const override { return xtc ? xtc->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return xtc ? xtc->getThumbBmpPath() : ""; }
  void onReaderEnter() override;
  void onReaderExit() override;
  void readerLoop() override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("XtcReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  void render(RenderLock&&) override;
  ScreenshotInfo getScreenshotInfo() const override;
};
