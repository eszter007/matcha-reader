#include "EpubReaderActivity.h"

#include <DictIndex.h>
#include <Epub/Page.h>
#include <Epub/PageTextExtractor.h>
#include <Epub/VerticalSection.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/blocks/VerticalTextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <ctime>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>

#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderTranslationActivity.h"
#include "EpubReaderUtils.h"
#include "EpubReaderWordLookupActivity.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/settings/SettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
// Below this largest-contiguous-block, the render path first reclaims font memory (see
// render()). Chosen above the word-lookup self-heal floor (28K) so the reader hands off a
// coalesced heap BEFORE the dictionary activity opens on top of it.
constexpr uint32_t RESUME_HEAP_FLOOR = 40 * 1024;
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;
// Bump when ReaderPrefs gains/changes fields; stale files fall back to globals.
constexpr uint8_t READER_PREFS_VERSION = 1;
constexpr char READER_PREFS_FILE[] = "/readerprefs.bin";

// Largest-contiguous-block floor for the background image warm. The warm needs the same
// working set the page-turn decode it replaces would need (JPEG decoder ~20KB contiguous +
// pixel-cache band <= 24KB); below this the decode would fail either way, so don't try --
// the page-turn path keeps its existing on-demand behavior.
constexpr uint32_t IMAGE_WARM_MIN_ALLOC = 30 * 1024;

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

EpubReaderActivity::ReaderPrefs EpubReaderActivity::capturePrefsFromSettings() {
  ReaderPrefs p;
  p.fontFamily = SETTINGS.fontFamily;
  strncpy(p.sdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(p.sdFontFamilyName) - 1);
  p.sdFontFamilyName[sizeof(p.sdFontFamilyName) - 1] = '\0';
  p.fontSize = SETTINGS.fontSize;
  p.lineSpacing = SETTINGS.lineSpacing;
  p.screenMargin = SETTINGS.screenMargin;
  p.bookCssMargins = SETTINGS.bookCssMargins;
  p.paragraphAlignment = SETTINGS.paragraphAlignment;
  p.embeddedStyle = SETTINGS.embeddedStyle;
  p.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  p.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  p.imageRendering = SETTINGS.imageRendering;
  p.orientation = SETTINGS.orientation;
  return p;
}

void EpubReaderActivity::applyPrefsToSettings(const ReaderPrefs& prefs) {
  SETTINGS.fontFamily = prefs.fontFamily;
  strncpy(SETTINGS.sdFontFamilyName, prefs.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName) - 1);
  SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  SETTINGS.fontSize = prefs.fontSize;
  SETTINGS.lineSpacing = prefs.lineSpacing;
  SETTINGS.screenMargin = prefs.screenMargin;
  SETTINGS.bookCssMargins = prefs.bookCssMargins;
  SETTINGS.paragraphAlignment = prefs.paragraphAlignment;
  SETTINGS.embeddedStyle = prefs.embeddedStyle;
  SETTINGS.hyphenationEnabled = prefs.hyphenationEnabled;
  SETTINGS.focusReadingEnabled = prefs.focusReadingEnabled;
  SETTINGS.imageRendering = prefs.imageRendering;
  SETTINGS.orientation = prefs.orientation;
}

bool EpubReaderActivity::loadBookPrefs(ReaderPrefs& out) const {
  if (!epub) return false;
  const std::string path = epub->getCachePath() + READER_PREFS_FILE;
  if (!Storage.exists(path.c_str())) return false;
  HalFile f;
  if (!Storage.openFileForRead("ERS", path, f)) return false;
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != READER_PREFS_VERSION) return false;
  ReaderPrefs p;
  if (f.read(reinterpret_cast<uint8_t*>(&p), sizeof(p)) != sizeof(p)) return false;
  out = p;
  return true;
}

void EpubReaderActivity::saveBookPrefs(const ReaderPrefs& prefs) const {
  if (!epub) return;
  const std::string path = epub->getCachePath() + READER_PREFS_FILE;
  HalFile f;
  if (!Storage.openFileForWrite("ERS", path, f)) return;
  serialization::writePod(f, READER_PREFS_VERSION);
  f.write(reinterpret_cast<const uint8_t*>(&prefs), sizeof(prefs));
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();

  // Swallow the Confirm release that opened this book from the library,
  // so it doesn't immediately trigger the reader menu.
  ignoreNextConfirmRelease = true;
  readingSessionStartMs = millis();

  if (!epub) {
    return;
  }

  epub->setupCacheDir();

  // Per-book reader prefs: snapshot the global defaults, then pin this book's
  // saved settings if it has been opened before. Must run before orientation
  // and any font use so layout math and the SD font selection match the book.
  globalPrefsSnapshot = capturePrefsFromSettings();
  {
    ReaderPrefs bookPrefs;
    if (loadBookPrefs(bookPrefs) && !(bookPrefs == globalPrefsSnapshot)) {
      LOG_DBG("ERS", "Applying per-book reader prefs");
      applyPrefsToSettings(bookPrefs);
    }
  }
  // Always re-sync the SD font system, even when the book's prefs match the
  // globals: SETTINGS naming a family does not guarantee the manager has it
  // loaded (a failed load elsewhere clears the manager but the name can
  // reappear via prefs/restore) -- skipping this rendered the built-in font
  // despite the settings page showing the SD family. Cheap no-op when the
  // wanted family and size are already loaded.
  sdFontSystem.ensureLoaded(renderer);

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

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

  // Japanese books (or forced vertical text) need the proper-size JP fallback font; plain
  // Latin books must not pay its SD load / RAM (user-reported).
  sdFontSystem.setJpFallbackNeeded(renderer, isJapaneseBook() || useVerticalText());

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

  // Record the sub-interval tail of the session; whole minutes were already flushed
  // periodically from loop() (see ReaderUtils::flushReadingStats).
  ReaderUtils::flushReadingStats(readingSessionStartMs, /*force=*/true);

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

  // Per-book reader prefs: pin the current reading settings to this book, then
  // restore the global defaults. Settings edited while reading (via the pushed
  // settings screen or the orientation shortcut) saved themselves into the
  // global file mid-session; re-saving after the restore keeps the global page
  // as the untouched default for books without their own prefs.
  //
  // Deliberately AFTER the section resets: restoring can swap the SD font, and
  // SdCardFontSystem's loadFamily fails (and clears the selection) under a
  // tight heap. With the chapter freed first, the load gets the coalesced heap
  // instead of running at the worst moment with the whole book still resident.
  if (epub) {
    const ReaderPrefs current = capturePrefsFromSettings();
    ReaderPrefs existing;
    if (!loadBookPrefs(existing) || !(existing == current)) {
      saveBookPrefs(current);
    }
    if (!(current == globalPrefsSnapshot)) {
      applyPrefsToSettings(globalPrefsSnapshot);
      SETTINGS.saveToFile();
      // Re-sync the loaded SD font to the restored globals so the next
      // consumer (home UI thumbnails, TXT/XTC readers) sees a consistent state.
      sdFontSystem.ensureLoaded(renderer);
    }
  }

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

  // Cancel any in-flight background image warm the moment the user touches a button -- BEFORE
  // any handler below can request a render, push a subactivity, or pop this activity (push/pop
  // block on the RenderLock the warm's render() call is still holding; the warm polls this
  // stamp per decode block, so the wait stays in the milliseconds).
  if (mappedInput.wasAnyPressed()) {
    imageWarmInputStamp_.fetch_add(1, std::memory_order_relaxed);
  }

  // Crash-proof stats: flush whole minutes every few minutes so an exit path that
  // never reaches onExit() (hang/reset on sleep, battery pull) can't lose the day.
  ReaderUtils::flushReadingStats(readingSessionStartMs);

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
      openReaderMenu();
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
      openFootnotesPanel();
    }
    return;
  }

  // Handle short power button press for word lookup
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::WORD_LOOKUP &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openWordLookupPanel();
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

  // Page-based jump matching the displayed page-based progress: percent -> absolute page
  // index -> containing spine + fraction within it.
  updateChapterPageSpan(lastViewportWidth, lastViewportHeight);
  if (bookPagesTotal > 0 && !spinePagesEffective.empty()) {
    const int targetPage = std::clamp((percent * bookPagesTotal + 50) / 100, 1, bookPagesTotal);
    int acc = 0;
    int targetSpine = static_cast<int>(spinePagesEffective.size()) - 1;
    int pageInSpine = spinePagesEffective.back();
    for (size_t i = 0; i < spinePagesEffective.size(); i++) {
      if (targetPage <= acc + spinePagesEffective[i]) {
        targetSpine = static_cast<int>(i);
        pageInSpine = targetPage - acc;
        break;
      }
      acc += spinePagesEffective[i];
    }
    // Fraction within the spine; the real page resolves after the section loads (estimated
    // counts may differ slightly from the built section's).
    pendingSpineProgress =
        (spinePagesEffective[targetSpine] > 0)
            ? static_cast<float>(pageInSpine - 1) / static_cast<float>(spinePagesEffective[targetSpine])
            : 0.0f;
    pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);
    RenderLock lock(*this);
    currentSpineIndex = targetSpine;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
    verticalSection.reset();
    return;
  }

  // Fallback (no page model yet): convert percent into a byte-like absolute position across the spine sizes.
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

void EpubReaderActivity::openReaderMenu() {
  const int sectionPage = verticalSection ? verticalSection->currentPage + 1 : section ? section->currentPage + 1 : 0;
  const int sectionPageCount = verticalSection ? verticalSection->pageCount : section ? section->pageCount : 0;
  // The menu header shows the same ToC-chapter-wide numbering and page-based book
  // progress as the status bar.
  updateChapterPageSpan(lastViewportWidth, lastViewportHeight);
  float bookProgress = 0.0f;
  if (bookPagesTotal > 0) {
    bookProgress = 100.0f * static_cast<float>(bookPagesBefore + sectionPage) / static_cast<float>(bookPagesTotal);
  } else if (epub->getBookSize() > 0 && (section || verticalSection) && sectionPageCount > 0) {
    const float chapterProgress = static_cast<float>(sectionPage - 1) / static_cast<float>(sectionPageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int currentPage = chapterPagesBefore + sectionPage;
  const int totalPages = std::max(chapterPagesTotal, currentPage);
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
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
          !sectionFootnotes.empty() || !currentPageFootnotes.empty(), !cachedBookmarks.empty(), hasWordLookup,
          showVerticalToggle, useVerticalText(), useFurigana(), hasPageText),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        toggleAutoPageTurn(menu.pageTurnOption);
        if (menu.verticalOverride >= 0 && menu.verticalOverride != (useVerticalText() ? 1 : 0)) {
          verticalOverride = menu.verticalOverride;
          section.reset();
          verticalSection.reset();
          // Forcing vertical text on a non-ja book is the same signal
          // isJapaneseBook() covers at open: JP fallback follows it.
          sdFontSystem.setJpFallbackNeeded(renderer, isJapaneseBook() || useVerticalText());
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
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS: {
      // Push settings opened on the Reader tab; Back pops straight back into the book. Font or
      // margin changes are picked up on return: the SD font system reloads to the new selection
      // and the next render's section-cache parameter check rebuilds the layout if needed.
      startActivityForResult(std::make_unique<SettingsActivity>(renderer, mappedInput, /*initialCategory=*/1,
                                                                /*finishOnBack=*/true),
                             [this](const ActivityResult&) {
                               sdFontSystem.ensureLoaded(renderer);
                               sdFontSystem.setJpFallbackNeeded(renderer, isJapaneseBook() || useVerticalText());
                               // Return to the reader MENU (where the user came from), not the page.
                               openReaderMenu();
                             });
      break;
    }
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
      openFootnotesPanel();
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
      openWordLookupPanel();
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
    // Also release the resident font caches (SD font slab + advance tables -- tens of KB on a
    // Japanese book). Freeing the Epub alone leaves those pinned, fragmenting the heap so WiFi +
    // the TLS handshake dip below MIN_HEAP_FOR_TLS and OOM. The Translate Page path (same
    // handshake) already does this; the reader re-warms fonts lazily on return.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
    }
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
  // Tilt- and auto-page-turn calls reach here without a button press, so the loop()-side stamp
  // bump didn't fire -- cancel a running image warm before the chapter-boundary branches below
  // block on the RenderLock it holds.
  imageWarmInputStamp_.fetch_add(1, std::memory_order_relaxed);
  lastTurnForward_.store(isForwardTurn, std::memory_order_relaxed);

  // A vertical chapter is still building on the render task: pageCount is 0 until the build
  // ends, so the normal path below would misread every press as "past the last page", block
  // on the RenderLock for the rest of the build, and then jump to the NEXT SPINE (observed on
  // device: one press during the build teleported the reader to the end of the book). Route
  // the turn to the build's page-request hook instead -- the page shows as soon as it exists.
  if (verticalBuildInProgress_.load(std::memory_order_relaxed)) {
    const int shown = earlyDisplayedPage_.load(std::memory_order_relaxed);
    if (shown >= 0 && verticalSection) {
      const int target = isForwardTurn ? shown + 1 : (shown > 0 ? shown - 1 : 0);
      verticalSection->requestPageDuringBuild(target);
    }
    lastPageTurnTime = millis();
    return;
  }

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
  lastViewportWidth = viewportWidth;
  lastViewportHeight = viewportHeight;

  // Reflow a resident section when a layout-affecting setting changed under it.
  // The reader menu is pushed on top of this activity, so editing e.g. screenMargin
  // never null-resets the section: the new margin moves the draw origin (oL/oR) but
  // the cached line layout keeps the old viewport width, so justified text overflows
  // one side until the book is reopened. Detect the change and reflow in place,
  // preserving the reading position the same way applyOrientation() does. This runs
  // in the render task, which already holds the render lock, so no RenderLock here.
  {
    const LayoutSig currentSig{effectiveReaderFontId(),
                               viewportWidth,
                               viewportHeight,
                               SETTINGS.getReaderLineCompression(),
                               SETTINGS.paragraphAlignment,
                               static_cast<bool>(SETTINGS.extraParagraphSpacing),
                               static_cast<bool>(SETTINGS.hyphenationEnabled),
                               static_cast<bool>(SETTINGS.embeddedStyle),
                               SETTINGS.imageRendering,
                               static_cast<bool>(SETTINGS.focusReadingEnabled),
                               static_cast<bool>(SETTINGS.bookCssMargins)};
    if ((section || verticalSection) && currentSig != sectionLayoutSig) {
      LOG_DBG("ERS", "Layout params changed; reflowing section in place");
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
    sectionLayoutSig = currentSig;
  }

  // Low-heap floor for the resume-into-book path. A sleep wake reboots straight into the reader
  // (lastSleepFromReader) and renders on whatever fragmented heap the boot produced -- unlike
  // opening from home, which the reporter confirms is always clean. On the X3 the wider 528px
  // viewport packs more glyphs per page (bigger prewarm + font slab) than the X4's 480px, so a
  // marginal render that survives on X4 (device log: maxAlloc bottoms at ~16-34K here) OOMs
  // partway on X3: missing glyph chunks, then the dictionary can't claim its caches on top of
  // the resident font buffers ("no matches"), then home titles fail -- all cleared by going
  // home, which frees this activity. Reclaiming the font decompressor's hot-group + glyph slab
  // now coalesces the heap before the render/prewarm and the following word lookup, matching
  // what the cache-MISS build path already does. Gated so normal reading (maxAlloc 34K+ in the
  // log) is untouched; fonts reload lazily on the next prewarm.
  if (auto* fcm = renderer.getFontCacheManager()) {
    const uint32_t maxAlloc = ESP.getMaxAllocHeap();
    if (maxAlloc < RESUME_HEAP_FLOOR) {
      LOG_INF("ERS", "Low heap before render (maxAlloc=%u < %u); releasing font memory", maxAlloc, RESUME_HEAP_FLOOR);
      fcm->releaseAllFontMemory();
      prewarmedVPage_ = -1;  // the release just emptied the mini-font cache (vertical)
      prewarmedHPage_ = -1;  // ...and the horizontal warm
      LOG_INF("ERS", "After font release: maxAlloc=%u", ESP.getMaxAllocHeap());
    }
  }

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
      prewarmedVPage_ = -1;  // fresh section: whatever sits in the mini-font cache is stale
      verticalSection = std::unique_ptr<VerticalSection>(new VerticalSection(epub, currentSpineIndex, renderer));
      sectionFootnotes.clear();  // vertical sections don't collect footnotes

      const int fontId = effectiveReaderFontId();
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

        // Early first render: show the reader's page the moment it is laid out (a couple of
        // seconds in) instead of after the whole chapter builds (~17s for a 431-page book).
        // Percent jumps need the final pageCount and keep the classic wait-for-the-build
        // path. Rebuilds with saved progress carry cachedChapterTotalPageCount; the remap
        // below only moves the page when the final count actually differs (a reflow), so the
        // saved page number is an exact early target for unchanged layouts and a close
        // estimate otherwise -- the post-build render corrects the rare off-by-a-few case
        // with its one refresh. A target beyond the final page count (e.g. the "last page"
        // sentinel when paging backwards) simply never fires the hook -- classic path again.
        if (!pendingPercentJump) {
          const int earlyTarget =
              pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber > 0 ? nextPageNumber : 0);
          verticalSection->setEarlyRenderHook(this, &EpubReaderActivity::earlyRenderVerticalPageThunk, earlyTarget);
        }

        earlyDisplayedPage_.store(-1, std::memory_order_relaxed);
        verticalBuildInProgress_.store(true, std::memory_order_relaxed);
        const bool built = verticalSection->createSectionFile(fontId, viewportWidth, viewportHeight);
        verticalBuildInProgress_.store(false, std::memory_order_relaxed);
        if (!built) {
          LOG_ERR("ERS", "Failed to build vertical section");
          failedVerticalSpineIndex = currentSpineIndex;
          verticalSection.reset();
          showPendingSyncSaveError();
          return;
        }

        // Mid-build page turns moved the displayed page; the position the user READ to is
        // the truth now, not the pre-build progress restore.
        const int shown = earlyDisplayedPage_.load(std::memory_order_relaxed);
        if (shown >= 0) {
          pendingPageJump.reset();
          nextPageNumber = shown;
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

    bool imagePageDisplayed = false;
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
                            UITheme::getInstance().getMetrics().statusBarVerticalMargin + SETTINGS.screenMargin;
        const auto drawImagePage = [&]() {
          if (vpage->imageRotated) {
            ImageBlock imgBlock(vpage->imagePath, vpage->imageWidth, vpage->imageHeight);
            imgBlock.setRotated(true, static_cast<int16_t>(reserve));
            imgBlock.render(renderer, 0, 0);  // ImageBlock handles rotation + centering
          } else {
            int iw = vpage->imageWidth;
            int ih = vpage->imageHeight;
            // Shared with warmNextPageImageCache so a background-warmed cache has EXACTLY the
            // dimensions this render asks for.
            ImageBlock::fitWithin(viewportWidth, viewportHeight, iw, ih);
            ImageBlock fitBlock(vpage->imagePath, static_cast<int16_t>(iw), static_cast<int16_t>(ih));
            const int imgX = orientedMarginLeft + (viewportWidth - iw) / 2;
            const int imgY = orientedMarginTop + (viewportHeight - ih) / 2;
            fitBlock.render(renderer, imgX, imgY);
          }
        };

        // Same display sequence as the manga reader: one FAST BW pass, then the grayscale
        // planes. The image was decoded with 4-level Bayer dithering, and a plain BW display
        // renders gray levels 0-1 as solid black -- a mid-dark cover half showed as one black
        // blob until the grayscale planes lift the dark tones. (No blank-white intermediate
        // pass: it read as a distracting flash on full-page images.)
        drawImagePage();
        renderStatusBar();
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);

        if (renderer.supportsStripGrayscale()) {
          constexpr int STRIP_ROWS = 80;
          const int gh = renderer.getDisplayHeight();
          const int gwBytes = renderer.getDisplayWidthBytes();
          auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
          if (!scratch) {
            LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); image stays BW this page", gwBytes * STRIP_ROWS);
          } else {
            renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
            for (int y = 0; y < gh; y += STRIP_ROWS) {
              const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
              renderer.beginStripTarget(scratch.get(), y, rows);
              renderer.clearScreen(0x00);
              drawImagePage();
              renderer.endStripTarget();
              renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
            }
            renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
            for (int y = 0; y < gh; y += STRIP_ROWS) {
              const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
              renderer.beginStripTarget(scratch.get(), y, rows);
              renderer.clearScreen(0x00);
              drawImagePage();
              renderer.endStripTarget();
              renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
            }
            renderer.setRenderMode(GfxRenderer::BW);
            renderer.displayGrayBuffer();
            renderer.cleanupGrayscaleWithFrameBuffer();
          }
        }
        // Gray charge in the image region needs the HALF ghost-cleanup on the next page.
        pagesUntilFullRefresh = 1;
        imagePageDisplayed = true;
      } else {
        renderVerticalPageBody(*vpage, /*glyphsAlreadyWarm=*/prewarmedVPage_ == verticalSection->currentPage);
      }
      LOG_DBG("ERS", "Rendered vertical page in %dms", millis() - start);
    }

    if (!imagePageDisplayed) {  // image pages already displayed (double-fast + grayscale planes)
      renderStatusBar();
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    }
    // Kindle-class turns: load the NEXT page's glyphs while the reader looks at this one,
    // so the next forward turn renders from a warm cache instead of paying the full
    // per-page SD bulk load at button time. The mini-font cache holds exactly one page per
    // style; the page on screen no longer needs its glyphs (pixels are in the framebuffer).
    // getPage(next) also leaves the next page in the section's single-page read cache, so
    // the turn skips the SD page read too. Backward turns miss the warm cache and take the
    // classic prewarm path.
    prewarmedVPage_ = -1;
    const int warmTarget = lastTurnForward_.load(std::memory_order_relaxed) ? verticalSection->currentPage + 1
                                                                            : verticalSection->currentPage - 1;
    if (warmTarget >= 0 && warmTarget < verticalSection->pageCount) {
      if (const VerticalPage* np = verticalSection->getPage(warmTarget); np && !np->isImagePage()) {
        if (prewarmVerticalPageGlyphs(*np)) prewarmedVPage_ = warmTarget;
      }
    }
    // Pre-build the next chapter's cache while this page is on screen. This call was
    // missing from the vertical path (lost in a refactor -- silentIndexNextChapterIfNeeded
    // has had a vertical branch all along), so in tategaki books every chapter transition
    // showed the Indexing popup. JP books make it worse: each full-page illustration is its
    // own one-page spine item, so a text->image->text sequence popped Indexing twice. Now
    // the image chapter builds while the last text pages are read, and the next text
    // chapter builds while the reader looks at the illustration.
    silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
    saveProgress(currentSpineIndex, verticalSection->currentPage, verticalSection->pageCount, verticalOverride,
                 furiganaOverride);

    showPendingSyncSaveError();

    if (pendingScreenshot) {
      pendingScreenshot = false;
      ScreenshotUtil::takeScreenshot(renderer);
    }

    if (showBookmarkMessage) {
      GUI.drawPopup(renderer, tr(STR_BOOKMARK_ADDED));
    }

    // Last: warm the NEXT page's image pixel cache while this page is on screen, so landing on
    // a full-page illustration is a cache read + FAST pass instead of a multi-second decode.
    // Cancellable per decode block the moment any input or queued render arrives.
    warmNextPageImageCache(viewportWidth, viewportHeight);
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
    prewarmedHPage_ = -1;  // fresh section: any page warmed for the previous one is stale
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));

    if (!section->loadSectionFile(effectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.bookCssMargins)) {
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

      if (!section->createSectionFile(effectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.bookCssMargins,
                                      popupFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        failedSectionSpineIndex = currentSpineIndex;
        section.reset();
        showPendingSyncSaveError();
        return;
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    section->loadSectionFootnotes(sectionFootnotes);
    if (!sectionFootnotes.empty()) {
      LOG_DBG("ERS", "Chapter footnotes: %u", static_cast<unsigned>(sectionFootnotes.size()));
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
    if (!currentPageFootnotes.empty()) {
      LOG_DBG("ERS", "Page footnotes: %u (first '%s')", static_cast<unsigned>(currentPageFootnotes.size()),
              currentPageFootnotes[0].number);
    }

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft,
                   /*glyphsAlreadyWarm=*/prewarmedHPage_ == section->currentPage);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }

  // Idle next-page prewarm (Kindle-class turns): while the reader looks at this page, scan the
  // page they'll turn to next (direction-adaptive) and warm its glyphs into the font cache, so
  // the next turn renders warm (~30ms) instead of paying the scan + SD bulk load at button
  // time. The scan pass draws nothing (GfxRenderer skips drawing while scanning), so it never
  // touches the displayed framebuffer. Heap-gated: warming loads an extra page transiently, so
  // skip it when the largest free block is tight and take the classic cold turn instead.
  prewarmedHPage_ = -1;
  constexpr uint32_t IDLE_WARM_MIN_ALLOC = 32 * 1024;
  if (auto* fcm = renderer.getFontCacheManager(); fcm && ESP.getMaxAllocHeap() >= IDLE_WARM_MIN_ALLOC) {
    const int warmTarget =
        lastTurnForward_.load(std::memory_order_relaxed) ? section->currentPage + 1 : section->currentPage - 1;
    if (warmTarget >= 0 && warmTarget < section->pageCount) {
      if (auto np = section->loadPageFromSectionFile(warmTarget)) {
        auto scope = fcm->createPrewarmScope();
        np->render(renderer, effectiveReaderFontId(), orientedMarginLeft, orientedMarginTop, !useFurigana());
        scope.endScanAndPrewarm();
        scope.release();  // keep the warm resident for the upcoming turn
        prewarmedHPage_ = warmTarget;
      }
    }
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

  // Last: warm the NEXT page's image pixel cache while this page is on screen (see the
  // identical call at the vertical path's tail).
  warmNextPageImageCache(viewportWidth, viewportHeight);
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (useVerticalText()) {
    // Fire on the last two pages, and on single-page chapters (image-only illustration
    // chapters are one page each -- with the old penultimate-page-only trigger a run of
    // them showed the Indexing popup on every page turn).
    if (!epub || !verticalSection || verticalSection->pageCount < 1) return;
    if (verticalSection->currentPage < verticalSection->pageCount - 2) return;

    const int nextSpineIndex = currentSpineIndex + 1;
    if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) return;

    VerticalSection nextVSection(epub, nextSpineIndex, renderer);
    const int fontId = effectiveReaderFontId();
    if (nextVSection.loadSectionFile(fontId, viewportWidth, viewportHeight)) return;

    // The vertical build is the most memory-intensive step in the reader, and this
    // silent path runs it at the worst heap moment: right after a page render, with
    // the glyph slab fully warmed AND the current chapter still resident. Hand the
    // build the font memory first, like the mainline vertical build does. Costs
    // nothing here: every vertical page render clearCache()s and re-prewarms anyway.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
    }

    LOG_DBG("ERS", "Silently indexing next vertical chapter: %d (maxAlloc=%u)", nextSpineIndex, ESP.getMaxAllocHeap());
    if (!nextVSection.createSectionFile(fontId, viewportWidth, viewportHeight)) {
      LOG_ERR("ERS", "Failed silent indexing for vertical chapter: %d", nextSpineIndex);
    }
    return;
  }

  if (!epub || !section || section->pageCount < 1) {
    return;
  }

  // Build the next chapter cache while the last two pages are on screen. Also fires for
  // single-page chapters (image-only illustration chapters are one page each -- with the
  // old penultimate-page-only trigger a run of them showed Indexing on every page turn).
  if (section->currentPage < section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(effectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                  SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.bookCssMargins)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(effectiveReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                     SETTINGS.imageRendering, SETTINGS.focusReadingEnabled, SETTINGS.bookCssMargins)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::imageWarmShouldCancel(const void* ctx) {
  const auto* self = static_cast<const EpubReaderActivity*>(ctx);
  if (self->imageWarmInputStamp_.load(std::memory_order_relaxed) != self->imageWarmStampSnapshot_) {
    return true;
  }
  // A queued render (page turn already requested before the warm started, subactivity render,
  // requestUpdateAndWait from another task) is a pending task notification on the render task.
  // The warm runs ON the render task, so read our own notification VALUE without side effects:
  // ulTaskNotifyValueClear with zero bits to clear is a pure read. (xTaskNotifyAndQuery is NOT
  // -- even with eNoAction it is still a notify and stamps the notification state.)
  return ulTaskNotifyValueClear(nullptr, 0) > 0;
}

void EpubReaderActivity::warmNextPageImageCache(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  imageWarmStampSnapshot_ = imageWarmInputStamp_.load(std::memory_order_relaxed);
  if (imageWarmShouldCancel(this)) {
    LOG_DBG("IWARM", "skip: render already queued");  // TEMP diagnostics (slow-books hunt)
    return;                                           // another render is already queued -- stay out of its way
  }
  if (ESP.getMaxAllocHeap() < IMAGE_WARM_MIN_ALLOC) {
    LOG_DBG("IWARM", "skip: maxAlloc %u < %u floor", ESP.getMaxAllocHeap(), IMAGE_WARM_MIN_ALLOC);  // TEMP
    return;
  }

  const int fontId = effectiveReaderFontId();
  // Returns false to stop iterating (cancelled). The MOST RECENT failed target is remembered
  // (single path, not a list): the warm targets one page at a time, so one slot is enough to
  // stop the common retry churn of re-attempting the same broken image on every render tail.
  const auto warmBlock = [this](const ImageBlock& block) -> bool {
    if (block.getImagePath() == imageWarmFailedPath_) {
      return true;
    }
    const auto res = block.warmCache(renderer, &imageWarmShouldCancel, this);
    if (res == ImageBlock::WarmResult::Failed) {
      imageWarmFailedPath_ = block.getImagePath();
    }
    return res != ImageBlock::WarmResult::Cancelled;
  };

  if (useVerticalText()) {
    if (!verticalSection || verticalSection->pageCount == 0) {
      return;
    }
    // Constructed only on the spine-boundary branch (its ctor builds a path string -- avoidable
    // churn on the common within-chapter turn), but declared at this scope because it must
    // outlive vp: getPage() hands out a pointer into the section's page cache.
    std::optional<VerticalSection> nextV;
    const VerticalPage* vp = nullptr;
    const int nextPage = verticalSection->currentPage + 1;
    if (nextPage < verticalSection->pageCount) {
      vp = verticalSection->getPage(nextPage);
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      // Last page of the chapter: the next page lives in the next spine item. In JP books each
      // full-page illustration is its own one-page spine item, so this cross-boundary peek is
      // the common case -- silentIndexNextChapterIfNeeded has already built the section file.
      nextV.emplace(epub, currentSpineIndex + 1, renderer);
      if (nextV->loadSectionFile(fontId, viewportWidth, viewportHeight) && nextV->pageCount > 0) {
        vp = nextV->getPage(0);
      } else {
        LOG_DBG("IWARM", "boundary peek failed: spine %d section not loadable", currentSpineIndex + 1);  // TEMP
      }
    }
    if (!vp) {
      LOG_DBG("IWARM", "skip: no next vertical page (page %d/%d)", verticalSection->currentPage,
              verticalSection->pageCount);  // TEMP
      return;
    }
    if (!vp->isImagePage()) {
      LOG_DBG("IWARM", "skip: next vertical page is text");  // TEMP
      return;
    }
    if (vp->imageRotated) {
      const int reserve = UITheme::getInstance().getStatusBarHeight() +
                          UITheme::getInstance().getMetrics().statusBarVerticalMargin + SETTINGS.screenMargin;
      ImageBlock block(vp->imagePath, vp->imageWidth, vp->imageHeight);
      block.setRotated(true, static_cast<int16_t>(reserve));
      warmBlock(block);
    } else {
      // Same fit the render path computes -- shared helper keeps the cache dims identical.
      int iw = vp->imageWidth;
      int ih = vp->imageHeight;
      ImageBlock::fitWithin(viewportWidth, viewportHeight, iw, ih);
      warmBlock(ImageBlock(vp->imagePath, static_cast<int16_t>(iw), static_cast<int16_t>(ih)));
    }
    return;
  }

  if (!section || section->pageCount == 0) {
    return;
  }
  std::unique_ptr<Page> peeked;
  const int nextPage = section->currentPage + 1;
  if (nextPage < section->pageCount) {
    peeked = section->loadPageFromSectionFile(nextPage);
  } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
    Section nextSection(epub, currentSpineIndex + 1, renderer);
    if (nextSection.loadSectionFile(fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                    SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                    SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                    SETTINGS.focusReadingEnabled, SETTINGS.bookCssMargins) &&
        nextSection.pageCount > 0) {
      peeked = nextSection.loadPageFromSectionFile(0);
    }
  }
  if (!peeked) {
    LOG_DBG("IWARM", "skip: no next horizontal page peeked (page %d/%d)", section->currentPage,
            section->pageCount);  // TEMP
    return;
  }
  for (const auto& el : peeked->elements) {
    if (el->getTag() != TAG_PageImage) {
      continue;
    }
    if (!warmBlock(static_cast<const PageImage&>(*el).getImageBlock())) {
      break;  // cancelled -- input or a queued render wants the CPU/SD
    }
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount, int8_t vertOverride,
                                      int8_t furiOverride) {
  const uint8_t percent = static_cast<uint8_t>(pageBasedPercent(spineIndex, currentPage + 1));
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, vertOverride, furiOverride, percent);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft, const bool glyphsAlreadyWarm) {
  const auto t0 = millis();
  const int fontId = effectiveReaderFontId();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render. Skipped when the
  // idle next-page warm already loaded this page's glyphs (prewarmedHPage_) -- the cache is
  // warm, so we go straight to the real render (~30ms) instead of paying the scan + SD bulk
  // load again at button time.
  auto* fcm = renderer.getFontCacheManager();
  if (!glyphsAlreadyWarm) {
    auto scope = fcm->createPrewarmScope();
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, !useFurigana());  // scan pass
    scope.endScanAndPrewarm();
  }
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

void EpubReaderActivity::updateChapterPageSpan(const uint16_t viewportWidth, const uint16_t viewportHeight) const {
  const int livePages = verticalSection ? verticalSection->pageCount : (section ? section->pageCount : 0);
  const bool vertical = useVerticalText();
  if (chapterSpanSpine == currentSpineIndex && chapterSpanLivePages == livePages && chapterSpanVertical == vertical) {
    return;
  }
  const bool spanModeChanged = chapterSpanVertical != vertical;
  chapterSpanSpine = currentSpineIndex;
  chapterSpanLivePages = livePages;
  chapterSpanVertical = vertical;
  chapterPagesBefore = 0;
  chapterPagesTotal = std::max(1, livePages);
  bookPagesBefore = 0;
  bookPagesTotal = 0;
  spinePagesEffective.clear();

  if (!epub) return;
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) return;
  if (static_cast<int>(spinePagesReal.size()) != spineCount) spinePagesReal.assign(spineCount, 0);
  // A mode switch invalidates every cached count AND every remembered probe failure -- the two
  // modes use entirely different section files.
  if (spanModeChanged) spinePagesReal.assign(spineCount, 0);

  // Collect real page counts: the live section plus a cheap header-only cache peek for every
  // spine not seen yet this session (a missing cache is a fast failed open).
  const int fontId = effectiveReaderFontId();
  size_t knownBytes = 0;
  uint32_t knownPages = 0;
  for (int i = 0; i < spineCount; i++) {
    if (i == currentSpineIndex && livePages > 0) {
      spinePagesReal[i] = static_cast<uint16_t>(std::min(livePages, 0xFFFF));
    } else if (spinePagesReal[i] == 0) {
      // A failed probe is remembered for the session (kSpineProbeFailed): without this, every
      // menu open / status-bar refresh after a cache-version bump re-opened (and re-discarded)
      // all N stale section files -- a visible seconds-long delay on a 39-spine book.
      bool probed = false;
      if (vertical) {
        VerticalSection sibling(epub, i, renderer);
        if (sibling.loadSectionFile(fontId, viewportWidth, viewportHeight)) {
          spinePagesReal[i] = sibling.pageCount;
          probed = true;
        }
      } else {
        Section sibling(epub, i, renderer);
        if (sibling.loadSectionFile(fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                    SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                    SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                    SETTINGS.focusReadingEnabled, SETTINGS.bookCssMargins)) {
          spinePagesReal[i] = sibling.pageCount;
          probed = true;
        }
      }
      if (!probed) spinePagesReal[i] = kSpineProbeFailed;
    }
    if (spinePagesReal[i] > 0 && spinePagesReal[i] != kSpineProbeFailed) {
      knownPages += spinePagesReal[i];
      const size_t prev = (i >= 1) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      knownBytes += epub->getCumulativeSpineItemSize(i) - prev;
    }
  }

  // Effective counts: real where known, byte-share estimate (against the known chapters'
  // pages-per-byte ratio) where not.
  spinePagesEffective.assign(spineCount, 1);
  for (int i = 0; i < spineCount; i++) {
    if (spinePagesReal[i] > 0 && spinePagesReal[i] != kSpineProbeFailed) {
      spinePagesEffective[i] = spinePagesReal[i];
      continue;
    }
    if (knownBytes > 0 && knownPages > 0) {
      const size_t prev = (i >= 1) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      const size_t sz = epub->getCumulativeSpineItemSize(i) - prev;
      const uint64_t est = (static_cast<uint64_t>(sz) * knownPages + knownBytes / 2) / knownBytes;
      spinePagesEffective[i] = static_cast<uint16_t>(std::clamp<uint64_t>(est, 1, 0xFFFF));
    }
  }
  for (int i = 0; i < spineCount; i++) {
    if (i < currentSpineIndex) bookPagesBefore += spinePagesEffective[i];
    bookPagesTotal += spinePagesEffective[i];
  }

  // ToC-chapter span for the page X/Y display (spines without their own ToC entry inherit
  // the previous entry's tocIndex).
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIdx < 0) return;  // no ToC entry covers this spine -- keep per-file numbering
  int start = currentSpineIndex;
  while (start > 0 && epub->getTocIndexForSpineIndex(start - 1) == tocIdx) start--;
  int end = currentSpineIndex;
  while (end + 1 < spineCount && epub->getTocIndexForSpineIndex(end + 1) == tocIdx) end++;
  if (start == end) return;

  int before = 0;
  int total = 0;
  for (int i = start; i <= end; i++) {
    if (i < currentSpineIndex) before += spinePagesEffective[i];
    total += spinePagesEffective[i];
  }
  chapterPagesBefore = before;
  chapterPagesTotal = std::max(1, total);
}

int EpubReaderActivity::pageBasedPercent(const int spineIndex, const int sectionPage) const {
  if (bookPagesTotal <= 0 || spinePagesEffective.empty()) {
    // Model unavailable (e.g. no section loaded yet) -- fall back to byte weighting.
    if (!epub || epub->getBookSize() == 0) return 0;
    return clampPercent(static_cast<int>(epub->calculateProgress(spineIndex, 0.0f) * 100.0f + 0.5f));
  }
  int before = 0;
  for (int i = 0; i < spineIndex && i < static_cast<int>(spinePagesEffective.size()); i++) {
    before += spinePagesEffective[i];
  }
  const int read = before + std::max(1, sectionPage);
  return clampPercent((read * 100 + bookPagesTotal / 2) / bookPagesTotal);
}

bool EpubReaderActivity::prewarmVerticalPageGlyphs(const VerticalPage& vpage) {
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
  // Prewarm gated on heap: its page-text string and cache-slot claims are bare allocations,
  // and this body also runs mid-build (early first render / build-time page turns) where an
  // OOM aborts under -fno-exceptions. Without prewarm the page still renders correctly via
  // the per-glyph on-demand path -- slower, but never fatal.
  // 20K: normal post-build reading always clears this (heap 30K+), so warm turns are
  // unaffected. The gate only bites during a mid-build serve, and there it MUST stay high:
  // dropping it to 14K let the prewarm's resident footprint coincide with a dense ruby
  // page's layout dip and the build dropped glyphs (need 11736 @ 9716 free) -> stale cache.
  // Build-phase serves rendering on-demand (~2s) is a drop-free, one-time-per-book cost;
  // content integrity outranks shaving that.
  constexpr uint32_t PREWARM_MIN_ALLOC = 20 * 1024;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm || ESP.getMaxAllocHeap() < PREWARM_MIN_ALLOC) return false;
  fcm->clearCache();
  uint8_t styleMask =
      std::accumulate(vpage.glyphs.begin(), vpage.glyphs.end(), uint8_t{0},
                      [](uint8_t m, const auto& g) { return static_cast<uint8_t>(m | (1u << (g.style & 0x03))); });
  if (styleMask == 0) styleMask = 1 << EpdFontFamily::REGULAR;
  const std::string pageText = PageTextExtractor::fromVerticalPage(vpage);
  fcm->prewarmCache(effectiveReaderFontId(), pageText.c_str(), styleMask);
  return true;
}

void EpubReaderActivity::renderVerticalPageBody(const VerticalPage& vpage, const bool glyphsAlreadyWarm) {
  if (!glyphsAlreadyWarm) prewarmVerticalPageGlyphs(vpage);
  // Same origin derivation as render(): vertical text only needs the top-left corner.
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  VerticalTextBlock block(vpage);
  if (useFurigana()) {
    block.render(renderer, effectiveReaderFontId(), SETTINGS.getRubyFontId(), marginLeft, marginTop, true);
  } else {
    block.render(renderer, effectiveReaderFontId(), marginLeft, marginTop, true);
  }
}

void EpubReaderActivity::earlyRenderVerticalPageThunk(void* ctx, const VerticalPage& page, const int pageIndex) {
  static_cast<EpubReaderActivity*>(ctx)->earlyRenderVerticalPage(page, pageIndex);
}

void EpubReaderActivity::earlyRenderVerticalPage(const VerticalPage& page, const int pageIndex) {
  const auto start = millis();
  // Update the shown-page index BEFORE drawing (the render takes 0.7-3s): a button press
  // during the render should target the page AFTER this one, not re-request this one
  // (observed on device as the same page rendering twice back-to-back).
  earlyDisplayedPage_.store(pageIndex, std::memory_order_relaxed);
  renderer.clearScreen();
  renderVerticalPageBody(page);
  // No status bar here: it derives page counts from a section that is still mid-build. The
  // normal post-build render adds it on top of the identical page content.
  renderer.displayBuffer();
  // The build resumes the moment this returns and needs its headroom back: the prewarm above
  // re-claimed font page slots and the decompressor glyph slab that the build path explicitly
  // released before starting. Deliberately NOT releaseAllFontMemory(): that would also drop
  // the SD fonts' advance tables, which the build's measurement is actively using -- their
  // mid-build 16KB rebuild allocation fails under build pressure (observed: a stream of
  // buildAdvanceTable OOM errors and deeper maxAlloc dips that dropped glyphs). clearCache()
  // frees what the render claimed while leaving the measurement caches intact.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->clearCache();
    if (auto* d = fcm->getDecompressor()) d->freeGlyphSlab();
  }
  LOG_DBG("ERS", "Early first render of page %d in %dms", pageIndex, millis() - start);
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int rawCurrentPage = verticalSection ? verticalSection->currentPage : section ? section->currentPage : 0;
  const int rawPageCount = verticalSection ? verticalSection->pageCount : section ? section->pageCount : 0;

  // Keep status bar sane on empty chapters: show a single skippable page (1/1)
  // instead of sentinel/underflow values like 65536/0.
  const int sectionPage = (rawPageCount > 0) ? std::clamp(rawCurrentPage + 1, 1, rawPageCount) : 1;
  const int sectionPageCount = (rawPageCount > 0) ? rawPageCount : 1;

  // Display page numbering spans the ToC chapter, not just this spine file; book progress is
  // page-based (pages read / total pages) with byte weighting only as a bootstrap fallback.
  updateChapterPageSpan(lastViewportWidth, lastViewportHeight);
  const int currentPage = chapterPagesBefore + sectionPage;
  const int pageCount = std::max(chapterPagesTotal, currentPage);

  float bookProgress;
  if (bookPagesTotal > 0) {
    bookProgress = 100.0f * static_cast<float>(bookPagesBefore + sectionPage) / static_cast<float>(bookPagesTotal);
  } else {
    const float sectionChapterProg = (rawPageCount > 0) ? (static_cast<float>(sectionPage) / sectionPageCount) : 0.0f;
    bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;
  }

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

int EpubReaderActivity::effectiveReaderFontId() const {
  const int companion = sdFontSystem.companionFontId();
  if (companion != 0) {
    const bool jpBook = isJapaneseBook() || useVerticalText();
    const bool coversPrimary = sdFontSystem.selectedFontCovers(jpBook ? 0x3042 : 'a');
    if (!coversPrimary) {
      LOG_DBG("ERS", "Effective font: companion %d (jp=%d, selected lacks primary script)", companion, jpBook);
      return companion;
    }
  }
  return SETTINGS.getReaderFontId();
}

void EpubReaderActivity::openWordLookupPanel() {
  if (!epub || !DictIndex::isAvailable()) return;
  // The scan-result cache path lets a re-open of the same page skip the dictionary scan.
  const std::string scanCachePath = epub->getCachePath() + "/wlscan.bin";
  if (verticalSection) {
    const VerticalPage* page = verticalSection->getPage();
    if (page) {
      startActivityForResult(std::make_unique<EpubReaderWordLookupActivity>(
                                 renderer, mappedInput, *page, scanCachePath, static_cast<uint16_t>(currentSpineIndex),
                                 static_cast<uint16_t>(verticalSection->currentPage)),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
  } else if (section) {
    auto page = section->loadPageFromSectionFile();
    if (page) {
      startActivityForResult(std::make_unique<EpubReaderWordLookupActivity>(
                                 renderer, mappedInput, *page, scanCachePath, static_cast<uint16_t>(currentSpineIndex),
                                 static_cast<uint16_t>(section->currentPage)),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
  }
}

void EpubReaderActivity::openFootnotesPanel() {
  // Prefer the chapter-wide list (section file v32+); fall back to the current page's own
  // footnotes for sections built by older firmware. The panel opens at the footnote nearest
  // the current page: the first one ON the page, else the most recently passed one.
  footnotePanelEntries.clear();
  int startIndex = 0;
  if (!sectionFootnotes.empty()) {
    const int curPage = section ? section->currentPage : 0;
    int lastBefore = -1;
    int onPage = -1;
    footnotePanelEntries.reserve(sectionFootnotes.size());
    for (size_t i = 0; i < sectionFootnotes.size(); i++) {
      const auto& [pageIdx, fn] = sectionFootnotes[i];
      if (onPage < 0 && pageIdx == curPage) onPage = static_cast<int>(i);
      if (pageIdx < curPage) lastBefore = static_cast<int>(i);
      footnotePanelEntries.push_back(fn);
    }
    startIndex = onPage >= 0 ? onPage : (lastBefore >= 0 ? lastBefore : 0);
  } else if (!currentPageFootnotes.empty()) {
    footnotePanelEntries = currentPageFootnotes;
  }
  if (footnotePanelEntries.empty()) return;

  startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, footnotePanelEntries,
                                                                       epub.get(), currentSpineIndex, startIndex),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                             navigateToHref(footnoteResult.href, true);
                           }
                           requestUpdate();
                         });
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && (section || verticalSection) && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    const int curPage = verticalSection ? verticalSection->currentPage : section ? section->currentPage : 0;
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
  const int curPage = verticalSection ? verticalSection->currentPage : section ? section->currentPage : -1;
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
