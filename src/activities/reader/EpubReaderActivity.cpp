#include "EpubReaderActivity.h"

#include <DictIndex.h>
#include <Epub/Page.h>
#include <Epub/PageTextExtractor.h>
#include <Epub/VerticalSection.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/VerticalTextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderTranslationActivity.h"
#include "EpubReaderWordLookupActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include <ctime>

#include "MappedInputManager.h"
#include "ReadingStatsStore.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

// SD card folder finished books are moved into. Single source of truth for the path.
// constexpr ⇒ lives in flash .rodata, no DRAM cost.
constexpr char READ_FOLDER[] = "/read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // length of "/Read" (excludes NUL)
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
// On rename failure: LOG_ERR and leave everything in place (no UI alert subsystem here).
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  // Swallow the Confirm release that opened this book from the library,
  // so it doesn't immediately trigger the reader menu.
  ignoreNextConfirmRelease = true;
  readingSessionStartMs = millis();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[8];
    if (f.read(data, 8) == 8) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      verticalOverride = static_cast<int8_t>(data[6]);
      furiganaOverride = static_cast<int8_t>(data[7]);
      LOG_DBG("ERS", "Loaded cache: spine=%d page=%d vertical=%d furigana=%d", currentSpineIndex, nextPageNumber,
              verticalOverride, furiganaOverride);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());

  loadCachedBookmarks();

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Record reading time for stats
  if (readingSessionStartMs > 0) {
    unsigned long elapsed = millis() - readingSessionStartMs;
    uint16_t minutes = static_cast<uint16_t>(elapsed / 60000);
    if (minutes > 0) {
      time_t now = time(nullptr);
      struct tm* t = localtime(&now);
      READING_STATS.loadFromFile();
      READING_STATS.addMinutes(static_cast<uint16_t>(t->tm_year + 1900),
                               static_cast<uint8_t>(t->tm_mon + 1),
                               static_cast<uint8_t>(t->tm_mday), minutes);
      READING_STATS.saveToFile();
    }
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  // Record book completion if exiting at end-of-book (only once per book).
  const bool atEnd = currentSpineIndex > 0 && epub && currentSpineIndex >= epub->getSpineItemsCount();
  if (atEnd) {
    READING_STATS.loadFromFile();
    READING_STATS.markBookFinished(epub->getPath());
    READING_STATS.saveToFile();
  }

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0, verticalOverride, furiganaOverride);
  }

  section.reset();
  verticalSection.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      // Only treat the book as "removed by us" if it was actually in the list, so the
      // re-add branch below doesn't insert a book the feature never removed.
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      // Re-add (goes to front of the list via addBook — accepted ordering side effect).
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so ANY exit path (Back, Home, file browser) relocates the book into
  // /Read/ in onExit(); paging back off the end screen disarms it (book not actually
  // finished). If removeReadBooksFromRecents also fired, RecentBooksStore::updatePath in the
  // move path becomes a safe no-op since the entry was already removed.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  // Enter reader menu activity on short-press Confirm. A long-press that fired a bound
  // function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
    } else {
      const int currentPage = verticalSection ? verticalSection->currentPage + 1
                              : section       ? section->currentPage + 1
                                              : 0;
      const int totalPages = verticalSection ? verticalSection->pageCount
                             : section       ? section->pageCount
                                             : 0;
      float bookProgress = 0.0f;
      if (epub->getBookSize() > 0 && (section || verticalSection) && totalPages > 0) {
        const float chapterProgress = static_cast<float>(currentPage - 1) / static_cast<float>(totalPages);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      // Word Lookup is about the text's language, not its layout direction, so it must not be
      // gated by verticalOverride==0 (explicitly reading a Japanese book horizontally shouldn't
      // hide the dictionary) -- but a book whose EPUB metadata doesn't declare dc:language=ja
      // (isJapaneseBook() false) should still get it if the user has explicitly forced vertical
      // mode on for it, since that's the same signal useVerticalText() already treats as
      // sufficient evidence of Japanese content. Previously this used isJapaneseBook() alone,
      // so a mis-tagged EPUB with vertical text manually forced on would render vertically but
      // never show Word Lookup at all.
      const bool isJapaneseContent = isJapaneseBook() || verticalOverride == 1;
      const bool hasWordLookup = isJapaneseContent && (verticalSection || section) && DictIndex::isAvailable();
      const bool showVerticalToggle = isJapaneseBook();
      bool hasPageText = false;
      if (verticalSection) {
        const VerticalPage* page = verticalSection->getPage();
        hasPageText = page && !PageTextExtractor::fromVerticalPage(*page).empty();
      } else if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        hasPageText = !section->getTextFromSectionFile().empty();
      }
      startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                                 renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                                 SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty(),
                                 hasWordLookup, showVerticalToggle, useVerticalText(), useFurigana(), hasPageText),
                             [this](const ActivityResult& result) {
                               const auto& menu = std::get<MenuResult>(result.data);
                               applyOrientation(menu.orientation);
                               toggleAutoPageTurn(menu.pageTurnOption);
                               if (menu.verticalOverride >= 0 && menu.verticalOverride != (useVerticalText() ? 1 : 0)) {
                                 verticalOverride = menu.verticalOverride;
                                 section.reset();
                                 verticalSection.reset();
                               }
                               if (menu.furiganaOverride >= 0 && menu.furiganaOverride != (useFurigana() ? 1 : 0)) {
                                 furiganaOverride = menu.furiganaOverride;
                               }
                               if (!result.isCancelled) {
                                 onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                               }
                               requestUpdate();
                             });
    }
  }

  // Long-press Confirm runs the user-selected function (SETTINGS.longPressMenuFunction).
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        // Hold ~0.4s drops a bookmark at the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          ignoreNextConfirmRelease = true;  // Prevent accidental menu open after adding bookmark
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        // Hold ~1s launches KOReader sync. If sync can't run (no credentials stored), fall
        // through so the normal Confirm-release still opens the reader menu.
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
          if (launchKOReaderSync()) {
            ignoreNextConfirmRelease = true;  // sync launched or error shown; suppress menu open
            return;
          }
        }
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    const int curPage = verticalSection ? verticalSection->currentPage : (section ? section->currentPage : 0);
    if (!nextTriggered && (section || verticalSection) && curPage > 0) {
      if (verticalSection)
        verticalSection->currentPage = 0;
      else
        section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
      verticalSection.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section && !verticalSection) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
    verticalSection.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (!result.isCancelled) {
      const auto& sync = std::get<ProgressChangeResult>(result.data);
      const int curPage = verticalSection ? verticalSection->currentPage : (section ? section->currentPage : -1);
      if (currentSpineIndex != sync.spineIndex || curPage != sync.page) {
        RenderLock lock(*this);
        currentSpineIndex = sync.spineIndex;
        nextPageNumber = sync.page;
        section.reset();
        verticalSection.reset();
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
              verticalSection.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      {
        const int curPage = verticalSection ? verticalSection->currentPage : (section ? section->currentPage : 0);
        const int pgCount = verticalSection ? verticalSection->pageCount : (section ? section->pageCount : 0);
        if (epub && epub->getBookSize() > 0 && pgCount > 0) {
          const float chapterProgress = static_cast<float>(curPage) / static_cast<float>(pgCount);
          bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
        }
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      std::string pageText;
      if (verticalSection) {
        const VerticalPage* page = verticalSection->getPage();
        if (page) {
          pageText = PageTextExtractor::fromVerticalPage(*page);
        }
      } else if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        pageText = section->getTextFromSectionFile();
      }
      if (!pageText.empty()) {
        startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, pageText),
                               [this](const ActivityResult& result) {});
        break;
      }
      // If no text (e.g. an image-only page) or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && (section || verticalSection)) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = verticalSection ? verticalSection->currentPage : section->currentPage;
          uint16_t backupPageCount = verticalSection ? verticalSection->pageCount : section->pageCount;
          section.reset();
          verticalSection.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount, verticalOverride, furiganaOverride)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::WORD_LOOKUP: {
      if (verticalSection) {
        const VerticalPage* page = verticalSection->getPage();
        if (page) {
          startActivityForResult(
              std::make_unique<EpubReaderWordLookupActivity>(renderer, mappedInput, *page),
              [this](const ActivityResult&) { requestUpdate(); });
        }
      } else if (section) {
        auto page = section->loadPageFromSectionFile();
        if (page) {
          startActivityForResult(
              std::make_unique<EpubReaderWordLookupActivity>(renderer, mappedInput, *page),
              [this](const ActivityResult&) { requestUpdate(); });
        }
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRANSLATE_PAGE: {
      std::string pageText;
      if (verticalSection) {
        const VerticalPage* page = verticalSection->getPage();
        if (page) {
          pageText = PageTextExtractor::fromVerticalPage(*page);
        }
      } else if (section) {
        pageText = section->getTextFromSectionFile();
      }
      if (!pageText.empty()) {
        // The extracted text is all Translation needs -- the Section/VerticalSection object
        // itself (current page's resident glyphs, page index) is dead weight for the duration of
        // the activity, and Translation's TLS handshake needs every contiguous byte it can get
        // (see MIN_HEAP_FOR_TLS in EpubReaderTranslationActivity.cpp). Sync nextPageNumber first
        // so the normal reload-from-cache path in render() resumes on the same page when we
        // return -- same pattern as the page-turn/spine-change call sites in this file.
        nextPageNumber = verticalSection ? verticalSection->currentPage
                         : section       ? section->currentPage
                                         : nextPageNumber;
        section.reset();
        verticalSection.reset();
        if (auto* fcm = renderer.getFontCacheManager()) {
          fcm->releaseAllFontMemory();
        }
        startActivityForResult(
            std::make_unique<EpubReaderTranslationActivity>(renderer, mappedInput, std::move(pageText)),
            [this](const ActivityResult&) { requestUpdate(); });
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_VERTICAL:
    case EpubReaderMenuActivity::MenuAction::TOGGLE_FURIGANA:
      break;
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;  // no-op: nothing to launch

  const int currentPage = verticalSection ? verticalSection->currentPage
                           : section       ? section->currentPage
                                           : nextPageNumber;
  const int totalPages = verticalSection ? verticalSection->pageCount
                          : section       ? section->pageCount
                                          : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  // Pre-compute local KO position and chapter name while Epub is still in RAM.
  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  // Persist current position so the reader resumes at the right page on return.
  // goToReader() depends on this file, so abort the sync if the write fails.
  if (!saveProgress(currentSpineIndex, currentPage, totalPages, verticalOverride, furiganaOverride)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;  // acted: surfaced a save error to the user
  }

  // Release Epub and Section to free ~65KB RAM for the TLS handshake.
  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock(*this);
    if (verticalSection) {
      nextPageNumber = verticalSection->currentPage;
    } else if (section) {
      nextPageNumber = section->currentPage;
    }
    section.reset();
    verticalSection.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;  // acted: launched the sync activity
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (verticalSection) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = verticalSection->pageCount;
      nextPageNumber = verticalSection->currentPage;
    } else if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
    verticalSection.reset();
  }
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (verticalSection) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = verticalSection->pageCount;
      nextPageNumber = verticalSection->currentPage;
    } else if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
    verticalSection.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  const int curPage = verticalSection ? verticalSection->currentPage : (section ? section->currentPage : 0);
  const int pgCount = verticalSection ? verticalSection->pageCount : (section ? section->pageCount : 0);

  if (isForwardTurn) {
    if (curPage < pgCount - 1) {
      if (verticalSection)
        verticalSection->currentPage++;
      else if (section)
        section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
        verticalSection.reset();
      }
    }
  } else {
    if (curPage > 0) {
      if (verticalSection)
        verticalSection->currentPage--;
      else if (section)
        section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
        verticalSection.reset();
      }
    }
  }
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    // Save progress at spine=spineCount so the library shows 100%.
    saveProgress(currentSpineIndex, 0, 1, verticalOverride, furiganaOverride);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  // --- Vertical text mode path ---
  if (useVerticalText()) {
    if (failedVerticalSpineIndex == currentSpineIndex) {
      // This spine index already failed to build this session (typically a transient low-heap
      // allocation failure) -- show a real error instead of silently re-attempting the same
      // expensive (multi-second) build every render, which looks like an infinite "Indexing" hang.
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    if (!verticalSection) {
      LOG_DBG("ERS", "Loading vertical section, index: %d", currentSpineIndex);
      verticalSection = std::unique_ptr<VerticalSection>(new VerticalSection(epub, currentSpineIndex, renderer));

      const int fontId = SETTINGS.getReaderFontId();
      if (!verticalSection->loadSectionFile(fontId, viewportWidth, viewportHeight)) {
        LOG_DBG("ERS", "Vertical cache not found, building...");
        GUI.drawPopup(renderer, tr(STR_INDEXING));

        // Building a chapter's vertical section is the single most memory-intensive step in the
        // reader (whole-chapter text extraction + XML parsing + layout, observed on a real device
        // needing 50-100+ KB of headroom for image-heavy Japanese chapters). FontDecompressor's
        // "hot group" buffer (up to ~64KB, retained across renders for reuse -- see
        // FontDecompressor.cpp) is dead weight at this exact moment since nothing has been drawn
        // yet; free it (and the persistent glyph slab) to hand that headroom to the
        // extraction/layout step that needs it most.
        if (auto* fcm = renderer.getFontCacheManager()) {
          fcm->releaseAllFontMemory();
        }

        if (!verticalSection->createSectionFile(fontId, viewportWidth, viewportHeight)) {
          LOG_ERR("ERS", "Failed to build vertical section");
          failedVerticalSpineIndex = currentSpineIndex;
          verticalSection.reset();
          showPendingSyncSaveError();
          return;
        }
      } else {
        LOG_DBG("ERS", "Vertical cache found");
      }

      if (pendingPageJump.has_value()) {
        if (verticalSection->pageCount == 0) {
          verticalSection->currentPage = 0;
        } else if (*pendingPageJump >= verticalSection->pageCount) {
          verticalSection->currentPage = verticalSection->pageCount - 1;
        } else {
          verticalSection->currentPage = *pendingPageJump;
        }
        pendingPageJump.reset();
      } else {
        verticalSection->currentPage = nextPageNumber;
        if (verticalSection->pageCount == 0) {
          verticalSection->currentPage = 0;
        } else if (verticalSection->currentPage < 0) {
          verticalSection->currentPage = 0;
        } else if (verticalSection->currentPage >= verticalSection->pageCount) {
          verticalSection->currentPage = verticalSection->pageCount - 1;
        }
      }
      pendingAnchor.clear();

      if (cachedChapterTotalPageCount > 0) {
        if (currentSpineIndex == cachedSpineIndex && verticalSection->pageCount != cachedChapterTotalPageCount) {
          float progress =
              static_cast<float>(verticalSection->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
          int newPage = static_cast<int>(progress * verticalSection->pageCount);
          verticalSection->currentPage = newPage;
        }
        cachedChapterTotalPageCount = 0;
      }

      if (pendingPercentJump && verticalSection->pageCount > 0) {
        int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(verticalSection->pageCount));
        if (newPage >= verticalSection->pageCount) {
          newPage = verticalSection->pageCount - 1;
        }
        verticalSection->currentPage = newPage;
      }
      pendingPercentJump = false;
    }

    renderer.clearScreen();

    if (verticalSection->pageCount == 0) {
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    if (verticalSection->currentPage < 0 || verticalSection->currentPage >= verticalSection->pageCount) {
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    updateBookmarkFlag();

    {
      const auto* vpage = verticalSection->getPage();
      if (!vpage) {
        LOG_ERR("ERS", "Failed to get vertical page");
        verticalSection->clearCache();
        verticalSection.reset();
        requestUpdate();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }

      currentPageFootnotes.clear();
      const auto start = millis();
      if (vpage->isImagePage()) {
        const int reserve = UITheme::getInstance().getStatusBarHeight() +
                            UITheme::getInstance().getMetrics().statusBarVerticalMargin +
                            SETTINGS.screenMargin;
        ImageBlock imgBlock(vpage->imagePath, vpage->imageWidth, vpage->imageHeight);
        if (vpage->imageRotated) {
          imgBlock.setRotated(true, static_cast<int16_t>(reserve));
          imgBlock.render(renderer, 0, 0);  // ImageBlock handles rotation + centering
        } else {
          int iw = vpage->imageWidth;
          int ih = vpage->imageHeight;
          if (iw > viewportWidth || ih > viewportHeight) {
            const float sx = static_cast<float>(viewportWidth) / iw;
            const float sy = static_cast<float>(viewportHeight) / ih;
            const float s = (sx < sy) ? sx : sy;
            iw = static_cast<int>(iw * s + 0.5f);
            ih = static_cast<int>(ih * s + 0.5f);
          }
          ImageBlock fitBlock(vpage->imagePath, static_cast<int16_t>(iw), static_cast<int16_t>(ih));
          const int imgX = orientedMarginLeft + (viewportWidth - iw) / 2;
          const int imgY = orientedMarginTop + (viewportHeight - ih) / 2;
          fitBlock.render(renderer, imgX, imgY);
        }
      } else {
        // Bulk-load every glyph this page needs before drawing a single one. Without this, each
        // draw call for a codepoint that isn't already cached falls through to the on-demand
        // fallback path (for SD-card fonts: a fresh SD file open + two seeks + two reads per
        // miss, into an 8-slot ring buffer) -- for a page with hundreds of distinct kanji, that's
        // hundreds of individual SD round-trips. Confirmed on a real device: this was the entire
        // cause of vertical page turns taking 1.5-2+ seconds of pure render time with an SD font.
        //
        // clearCache() first is required, not optional: FontDecompressor's prewarmCache() claims
        // one of only MAX_PAGE_SLOTS (4) page-buffer slots per call and never self-evicts --
        // "the caller must call freePageBuffer/clearCache to reset" (FontDecompressor.h). Without
        // this, every page turn permanently claims more slots; confirmed on a real device that
        // after ~1 page's worth of prewarm calls, all 4 slots were stuck full, "cannot prewarm"
        // fired for every subsequent page, and glyphs stopped resolving at all (blank pages).
        //
        // styleMask must reflect only the styles ACTUALLY present on this page, not a blanket "all
        // 4" request: FontCacheManager::prewarmCache() claims one slot per requested style, PLUS
        // another slot per style for the family's fallback font if it has one -- up to 8 slot
        // claims against only 4 slots total. Confirmed on a real device: even right after
        // clearCache(), a single page still hit "all 4 slots full" because the blanket request
        // needed more slots than exist, wasting them on styles the page doesn't even use.
        if (auto* fcm = renderer.getFontCacheManager()) {
          fcm->clearCache();
          uint8_t styleMask = 0;
          for (const auto& g : vpage->glyphs) styleMask |= static_cast<uint8_t>(1u << (g.style & 0x03));
          if (styleMask == 0) styleMask = 1 << EpdFontFamily::REGULAR;
          const std::string pageText = PageTextExtractor::fromVerticalPage(*vpage);
          fcm->prewarmCache(SETTINGS.getReaderFontId(), pageText.c_str(), styleMask);
        }
        VerticalTextBlock block(*vpage);
        if (useFurigana()) {
          block.render(renderer, SETTINGS.getReaderFontId(), SETTINGS.getRubyFontId(), orientedMarginLeft,
                       orientedMarginTop, true);
        } else {
          block.render(renderer, SETTINGS.getReaderFontId(), orientedMarginLeft, orientedMarginTop, true);
        }
      }
      LOG_DBG("ERS", "Rendered vertical page in %dms", millis() - start);
    }

    renderStatusBar();
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    saveProgress(currentSpineIndex, verticalSection->currentPage, verticalSection->pageCount, verticalOverride, furiganaOverride);

    showPendingSyncSaveError();

    if (pendingScreenshot) {
      pendingScreenshot = false;
      ScreenshotUtil::takeScreenshot(renderer);
    }

    if (showBookmarkMessage) {
      GUI.drawPopup(renderer, tr(STR_BOOKMARK_ADDED));
    }
    return;
  }

  // --- Horizontal text mode path (existing) ---
  if (failedSectionSpineIndex == currentSpineIndex) {
    // This spine index already failed to build this session -- show a real error instead of
    // silently re-attempting the same expensive build every render (see failedVerticalSpineIndex).
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
      LOG_DBG("ERS", "Cache not found, building...");

      GUI.drawPopup(renderer, tr(STR_INDEXING));

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      // Building can need a large single contiguous allocation (e.g. the zip inflate window) --
      // free the font decompressor's buffers (hot group + glyph slab) first to hand that
      // headroom to the build, same rationale as the identical call on the vertical-mode build
      // path above.
      if (auto* fcm = renderer.getFontCacheManager()) {
        fcm->releaseAllFontMemory();
      }

      if (!section->createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        failedSectionSpineIndex = currentSpineIndex;
        section.reset();
        showPendingSyncSaveError();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      if (section->pageCount == 0) {
        section->currentPage = 0;
      } else if (*pendingPageJump >= section->pageCount) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  saveProgress(currentSpineIndex, section->currentPage, section->pageCount, verticalOverride, furiganaOverride);

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (useVerticalText()) {
    if (!epub || !verticalSection || verticalSection->pageCount < 2) return;
    if (verticalSection->currentPage != verticalSection->pageCount - 2) return;

    const int nextSpineIndex = currentSpineIndex + 1;
    if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) return;

    VerticalSection nextVSection(epub, nextSpineIndex, renderer);
    const int fontId = SETTINGS.getReaderFontId();
    if (nextVSection.loadSectionFile(fontId, viewportWidth, viewportHeight)) return;

    LOG_DBG("ERS", "Silently indexing next vertical chapter: %d", nextSpineIndex);
    if (!nextVSection.createSectionFile(fontId, viewportWidth, viewportHeight)) {
      LOG_ERR("ERS", "Failed silent indexing for vertical chapter: %d", nextSpineIndex);
    }
    return;
  }

  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering, SETTINGS.focusReadingEnabled)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount, int8_t vertOverride,
                                      int8_t furiOverride) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, vertOverride, furiOverride);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, !useFurigana());  // scan pass
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, !useFurigana());
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, !useFurigana());
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, !useFurigana());
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Tiled grayscale: render each plane band-by-band into a small scratch and
  // stream straight to the controller, leaving the BW framebuffer intact so no
  // full-frame storeBwBuffer is needed; controller RAM is re-synced from the
  // live framebuffer afterward. The page is re-rendered ceil(H/STRIP_ROWS) times
  // per plane, but renderCharImpl culls out-of-band glyphs before decode so the
  // cost stays close to one render. Both text (drawPixel) and images
  // (DirectPixelWriter) honor the active strip target.
  if (needsAnyGrayscale && renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();

    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
    } else {
      // Bands may be streamed in any order: X4 windows each via setRamArea, X3
      // via PTL.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
      }
      const auto tGrayLsb = millis();

      // MSB plane.
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(scratch.get(), y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
        renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
      }
      const auto tGrayMsb = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tCleanup = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
              "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
              tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
    }
  } else {
    // Fallback path for a controller without strip support. grayscale rendering
    // TODO: Only do this if font supports it
    if (needsAnyGrayscale) {
      // Save the BW frame before the grayscale passes overwrite it, restore
      // after. Only needed when grayscale actually renders.
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        const auto tEnd = millis();
        LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
                tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      // Render and copy to MSB buffer
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      // display grayscale part
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      // No text AA and no images: BW frame already displayed above, no grayscale
      // to render, so no save/restore.
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int rawCurrentPage = verticalSection ? verticalSection->currentPage
                           : section       ? section->currentPage
                                           : 0;
  const int rawPageCount = verticalSection ? verticalSection->pageCount
                         : section       ? section->pageCount
                                         : 0;

  // Keep status bar sane on empty chapters: show a single skippable page (1/1)
  // instead of sentinel/underflow values like 65536/0.
  const int currentPage =
      (rawPageCount > 0) ? std::clamp(rawCurrentPage + 1, 1, rawPageCount) : 1;
  const int pageCount = (rawPageCount > 0) ? rawPageCount : 1;

  const float sectionChapterProg = (rawPageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0.0f;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && (section || verticalSection) && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    const int curPage = verticalSection ? verticalSection->currentPage
                        : section       ? section->currentPage
                                        : 0;
    savedPositions[footnoteDepth] = {currentSpineIndex, curPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, curPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
    verticalSection.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
    verticalSection.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  const std::string bmPath = BookmarkUtil::getBookmarkPath(epub->getPath());
  if (Storage.exists(bmPath.c_str())) {
    String json = Storage.readFile(bmPath.c_str());
    if (!json.isEmpty()) {
      JsonSettingsIO::loadBookmarks(cachedBookmarks, json.c_str());
    }
  }
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if ((!section && !verticalSection) || !epub) {
    return;
  }
  const int curPage = verticalSection ? verticalSection->currentPage
                      : section       ? section->currentPage
                                      : -1;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, curPage);
  int currentPage;
  int pageCount;
  {
    RenderLock lock(*this);
    pageCount = verticalSection ? verticalSection->pageCount : section->pageCount;
    currentPage = verticalSection ? verticalSection->currentPage : section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (verticalSection) {
      const VerticalPage* vp = verticalSection->getPage();
      if (vp) pageText = PageTextExtractor::fromVerticalPage(*vp);
    } else if (section && currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  const std::string path = BookmarkUtil::getBookmarkPath(epub->getPath());
  const std::string bookmarksDir = BookmarkUtil::getBookmarksDir();
  Storage.mkdir(bookmarksDir.c_str());
  const bool ok = JsonSettingsIO::saveBookmarks(cachedBookmarks, path.c_str());
  if (!ok) {
    LOG_ERR("ERS", "Failed to save bookmarks to: %s", path.c_str());
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if ((!section && !verticalSection) || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int page = verticalSection ? verticalSection->currentPage : section->currentPage;
  const int pageCount = verticalSection ? verticalSection->pageCount : section->pageCount;
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, page, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, page, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (verticalSection) {
    info.currentPage = verticalSection->currentPage + 1;
    info.totalPages = verticalSection->pageCount;
    if (epub && epub->getBookSize() > 0 && verticalSection->pageCount > 0) {
      const float chapterProgress =
          static_cast<float>(verticalSection->currentPage) / static_cast<float>(verticalSection->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  } else if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = verticalSection ? verticalSection->currentPage
                          : section       ? section->currentPage
                                          : nextPageNumber;
  const int totalPages = verticalSection ? verticalSection->pageCount
                         : section       ? section->pageCount
                                         : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}

bool EpubReaderActivity::isJapaneseBook() const {
  if (!epub) return false;
  const auto& lang = epub->getLanguage();
  return lang.size() >= 2 && lang[0] == 'j' && lang[1] == 'a';
}

bool EpubReaderActivity::useVerticalText() const {
  if (verticalOverride == 0) return false;
  if (verticalOverride == 1) return true;
  return isJapaneseBook();
}

bool EpubReaderActivity::useFurigana() const {
  if (furiganaOverride == 0) return false;
  if (furiganaOverride == 1) return true;
  return true;
}
