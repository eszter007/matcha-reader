#include "ReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <MangaPanel.h>
#include <Memory.h>

#include "BookStats.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderActivity.h"
#include "MangaReaderActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "TxtReaderActivity.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"

ReaderActivity::ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::string bookPath, const bool allowFastInitialRefresh)
    : Activity(name, renderer, mappedInput), bookPath(std::move(bookPath)), pagesUntilFullRefresh(0) {
  if (allowFastInitialRefresh) {
    const int refreshFrequency = SETTINGS.getRefreshFrequency();
    pagesUntilFullRefresh = refreshFrequency > 1 ? refreshFrequency : 2;
  }
}

std::unique_ptr<Activity> ReaderActivity::create(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string path, bool allowFastInitialRefresh) {
  if (manga::MangaBook::isMangaFolder(path)) {
    return makeUniqueNoThrow<MangaReaderActivity>(renderer, mappedInput, std::move(path));
  }

  if (FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path)) {
    return makeUniqueNoThrow<BmpViewerActivity>(renderer, mappedInput, std::move(path));
  }

  if (FsHelpers::hasXtcExtension(path)) {
    return makeUniqueNoThrow<XtcReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }

  if (FsHelpers::hasTxtExtension(path) || FsHelpers::hasMarkdownExtension(path)) {
    return makeUniqueNoThrow<TxtReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
  }

  return makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh);
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  // Heap ledger for field crash reports: free vs largest block distinguishes a
  // leak (free falls) from fragmentation (free stable, largest collapses).
  LOG_INF("MEM", "reader enter: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  if (!Storage.exists(bookPath.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", bookPath.c_str());
    finish();
    return;
  }

  // Clear remembered book after opening it
  if (!APP_STATE.openEpubPath.empty()) {
    APP_STATE.openEpubPath.clear();
    APP_STATE.saveToFile();
  }

  sdFontSystem.ensureLoaded(renderer);
  if (!loadBook()) {
    finish();
    return;
  }

  readingSessionStartMs = millis();
  onReaderEnter();
  BookStats::recordOpen(bookPath.c_str());
  requestUpdate();
}

// The book is only remembered once a page has actually reached the panel. Recording it in
// onEnter() meant a book that cannot be rendered was reopened on every wake (upstream #3724).
void ReaderActivity::rememberBookOnceRendered() {
  if (bookRemembered || !pageRendered.load(std::memory_order_acquire)) return;
  bookRemembered = true;
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(bookPath, getBookTitle(), getBookAuthor(), getBookThumbBmpPath());
}

void ReaderActivity::onExit() {
  Activity::onExit();
  ReaderUtils::flushReadingStats(readingSessionStartMs, true, hasBook() ? bookPath.c_str() : nullptr,
                                 hasBook() ? getBookLanguage() : nullptr);
  onReaderExit();

  // Keep rebuildable font buffers from pinning the heap between reading sessions.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseAllFontMemory();
  }

  LOG_INF("MEM", "reader exit: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  endOfBookOptions.reset();
  endOfBookOptionsReady.store(false, std::memory_order_release);
}

void ReaderActivity::loop() {
  if (!hasBook()) {
    finish();
    return;
  }
  ReaderUtils::flushReadingStats(readingSessionStartMs, false, bookPath.c_str(), getBookLanguage());
  // Here rather than in each format's readerLoop(): every reader routes through this, and the
  // write it defers (APP_STATE + Recent Books) belongs on the loop task, not the render task.
  rememberBookOnceRendered();
  readerLoop();
}

void ReaderActivity::clearEndOfBookOptionsIfNeeded() {
  if (isAtEndOfBook() || !endOfBookOptionsReady.load(std::memory_order_acquire)) return;
  RenderLock lock{RenderLock::Try{}};
  if (!lock.held()) return;
  endOfBookOptionsReady.store(false, std::memory_order_release);
  endOfBookOptions.reset();
}

bool ReaderActivity::handleBackNavigation() {
  return ReaderUtils::handleBackNavigation(mappedInput, activityManager, bookPath.c_str(),
                                           {this, [](void* ctx) { static_cast<ReaderActivity*>(ctx)->onGoHome(); }});
}

bool ReaderActivity::endOfBookMenuActive() const {
  return isAtEndOfBook() && endOfBookOptionsReady.load(std::memory_order_acquire) && endOfBookOptions->menuActive();
}

bool ReaderActivity::handleEndOfBookMenu(const bool suppressConfirmRelease) {
  if (!endOfBookMenuActive() || suppressConfirmRelease) return false;

  std::string openPath;
  switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
    case EndOfBookOptions::Action::OpenBook:
      activityManager.goToReader(openPath);
      return true;
    case EndOfBookOptions::Action::GoHome:
      onGoHome();
      return true;
    case EndOfBookOptions::Action::LastPage:
      onReturnFromEndOfBook();
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::Redraw:
      requestUpdate();
      return true;
    case EndOfBookOptions::Action::None:
      return false;
  }
  return false;
}

bool ReaderActivity::handleEndOfBookPageTurn(const bool prevTriggered, const bool nextTriggered) {
  if (!isAtEndOfBook()) return false;
  if (endOfBookMenuActive()) return true;
  if (nextTriggered) {
    onGoHome();
  } else if (prevTriggered) {
    onReturnFromEndOfBook();
    requestUpdate();
  }
  return true;
}

bool ReaderActivity::renderEndOfBook(const char* logTag) {
  if (!isAtEndOfBook()) return false;
  if (!endOfBookOptions) {
    endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
    if (!endOfBookOptions) LOG_ERR(logTag, "OOM: EndOfBookOptions");
  }
  renderer.clearScreen();
  if (endOfBookOptions) {
    endOfBookOptions->loadOnce(bookPath);
    // Release-publish AFTER loadOnce() so the main task's acquire load can't
    // observe an object whose names/selector are still being populated.
    endOfBookOptionsReady.store(true, std::memory_order_release);
    endOfBookOptions->render(renderer, mappedInput);
  }
  renderer.displayBuffer();
  return true;
}

bool ReaderActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    pagesUntilFullRefresh = 1;
    forcedRefreshPending = true;
  }
  requestUpdate();
  return true;
}
