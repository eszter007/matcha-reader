#include "EpubReaderActivity.h"

#include <DictIndex.h>
#include <Epub/Page.h>
#include <Epub/PageTextExtractor.h>
#include <Epub/VerticalSection.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/blocks/VerticalTextBlock.h>
#include <Epub/converters/BmpToFramebufferConverter.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
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

#include "../../util/BookmarkFile.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
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
#include "util/DictionaryRegistry.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
// Below this largest-contiguous-block, the render path first reclaims font memory (see
// render()). Chosen above the word-lookup self-heal floor (28K) so the reader hands off a
// coalesced heap BEFORE the dictionary activity opens on top of it.
constexpr uint32_t RESUME_HEAP_FLOOR = 40 * 1024;
// Largest-block floor below which the per-page bulk glyph prewarm gives up (see
// prewarmVerticalPageGlyphs). Also the point at which reclaiming font memory becomes worth its
// rebuild cost: below this, every page falls back to 40-70 individual on-demand SD glyph loads.
constexpr uint32_t PREWARM_MIN_ALLOC_READ = 12 * 1024;
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;
// Bump when ReaderPrefs gains/changes fields; stale files fall back to globals.
// v2: reader font size is a point size, not a size-enum slot (upstream 1.5.0). A v1 file's
// enum value would read as an absurdly small point size, so old per-book prefs are dropped.
constexpr uint8_t READER_PREFS_VERSION = 2;
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
  p.fontPointSize = SETTINGS.fontPointSize;
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
  SETTINGS.fontPointSize = prefs.fontPointSize;
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

bool EpubReaderActivity::loadBook() {
  if (ESP.getMaxAllocHeap() < 64 * 1024) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      LOG_INF("READER", "Low heap before book load (maxAlloc=%u); releasing font memory", ESP.getMaxAllocHeap());
      fcm->releaseAllFontMemory();
      LOG_INF("READER", "After font release: maxAlloc=%u", ESP.getMaxAllocHeap());
    }
  }

  epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = epub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (loaded) return true;

  if (!epub->getAccessError().empty()) {
    LOG_ERR("READER", "book unavailable: %s", epub->getAccessError().c_str());
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_BOOK_NOT_READABLE));
    delay(2500);
  } else {
    LOG_ERR("READER", "Failed to load epub");
  }
  epub.reset();
  return false;
}

void EpubReaderActivity::onReaderEnter() {
  // Some entry screens open on Confirm press, others on release. Swallow only
  // a release that is still pending; otherwise the first real reader click vanished.
  ignoreNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  ImageBlock::clearSessionRenderFailures();
  // Lazy image extraction: section builds only header-probe images, so the first
  // render of an image page pulls the file out of the EPUB through this hook.
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

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
    // Fork layout: [0-5] spine/page/count, [6] vertical, [7] furigana, [8] percent,
    // [9-12] optional visible-text offset (upstream #2805, appended after the fork bytes).
    uint8_t data[13];
    const int dataSize = f.read(data, sizeof(data));
    if (dataSize >= 8) {
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
      // Drop a forced-vertical flag stored against a non-Japanese book (set before
      // vertical was restricted to Japanese books, or on a book whose language was
      // later corrected). useVerticalText() already ignores it; clearing it here
      // keeps the stored state honest, so the reader menu shows what is in effect.
      // Rewritten to progress.bin by the next save.
      if (verticalOverride == 1 && !isJapaneseBook()) {
        LOG_INF("ERS", "Clearing forced-vertical flag on a non-Japanese book");
        verticalOverride = -1;
      }
      if (dataSize >= 13) {
        cachedVisibleTextOffset = static_cast<uint32_t>(data[9]) | (static_cast<uint32_t>(data[10]) << 8) |
                                  (static_cast<uint32_t>(data[11]) << 16) | (static_cast<uint32_t>(data[12]) << 24);
      }
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
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Japanese books (or forced vertical text) need the proper-size JP fallback font; plain
  // Latin books must not pay its SD load / RAM (user-reported).
  sdFontSystem.setJpFallbackNeeded(renderer, isJapaneseBook() || useVerticalText());

  loadCachedBookmarks();
}

void EpubReaderActivity::onReaderExit() {
  // The extractor holds a raw pointer to this activity's epub; drop it before
  // the activity (and the shared_ptr) goes away.
  ImageBlock::setExtractor(nullptr, nullptr);

  // Record book completion if exiting at end-of-book (only once per book).
  const bool atEnd = currentSpineIndex > 0 && epub && currentSpineIndex >= epub->getSpineItemsCount();
  if (atEnd) {
    READING_STATS_STORE.loadFromFile();
    READING_STATS_STORE.markBookFinished(epub->getPath());
    READING_STATS_STORE.saveToFile();
  }

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

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  // Below the floors: just wait. The tick is deferrable — page-turn transients
  // free up between turns and the tick retries every loop pass. Track the
  // paused state so skipLoopDelay() stops pinning the CPU at full speed while
  // no build work is actually happening (the gate can stay closed for a long
  // stretch if the retained build context itself holds the heap down).
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup() {
  // Mid-build indexing popup: only during onEnter's blocking build-to-target phase
  // (buildPopupPending), at most once, and only when the framebuffer isn't on loan.
  // If it fires while the loan is active (e.g. the parser's size-based call during
  // startBuild), pending stays set and the deadline check retries after the loan.
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts.
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect(const bool pageOnScreen) {
  if (isJapaneseBook()) {
    openWordLookupPanel(pageOnScreen);
    return;
  }
  std::string dictionaryFolder;
  DictionaryRegistry::folderForLanguageOrFallback(epub ? epub->getLanguage() : std::string{}, SETTINGS.dictionaryName,
                                                  dictionaryFolder);
  if (dictionaryFolder.empty()) {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  // Word geometry must match render(): viewable-area margins plus screen margin.
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  // A lookup ends back on the page no matter how it was opened (menu or
  // long-press): the user is mid-reading, not mid-menu.
  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(
                             renderer, mappedInput, std::move(page), orientedMarginLeft, orientedMarginTop,
                             std::move(dictionaryFolder), effectiveReaderFontId()),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void EpubReaderActivity::readerLoop() {
  // Cancel any in-flight background image warm the moment the user touches a button -- BEFORE
  // any handler below can request a render, push a subactivity, or pop this activity (push/pop
  // block on the RenderLock the warm's render() call is still holding; the warm polls this
  // stamp per decode block, so the wait stays in the milliseconds).
  if (mappedInput.wasAnyPressed()) {
    imageWarmInputStamp_.fetch_add(1, std::memory_order_relaxed);
    pendingHorizontalImageRefine_.store(NO_IMAGE_REFINE, std::memory_order_relaxed);
  }

  // A horizontal image is shown immediately in BW; refine it only after the reader
  // leaves the page idle. The render lock keeps this behind the foreground render,
  // and any input above cancels the pending refinement before it can be queued.
  constexpr unsigned long IMAGE_REFINE_IDLE_MS = 150;
  uint32_t pendingRefine = pendingHorizontalImageRefine_.load(std::memory_order_relaxed);
  if (pendingRefine != NO_IMAGE_REFINE && section && lastRenderCompleteMs != 0 &&
      millis() - lastRenderCompleteMs >= IMAGE_REFINE_IDLE_MS && !RenderLock::peek()) {
    const uint32_t currentKey =
        (static_cast<uint32_t>(currentSpineIndex) << 16) | static_cast<uint16_t>(section->currentPage);
    if (pendingRefine == currentKey && pendingHorizontalImageRefine_.compare_exchange_strong(
                                           pendingRefine, NO_IMAGE_REFINE, std::memory_order_relaxed)) {
      requestedHorizontalImageRefine_.store(currentKey, std::memory_order_relaxed);
      requestUpdate();
      return;
    }
  }
  // Idle glyph prewarm for the likely next page (currentPage + 1). The scan
  // pass draws nothing (FCM scan mode suppresses pixels), so the displayed
  // framebuffer is untouched; endScanAndPrewarm loads only glyphs not already
  // cached. Debounced past rapid page-flipping, one attempt per position, and
  // deferred while a render/build owns the CPU or the heap is at the render
  // floor. Cross-chapter prewarm is deliberately out of scope (next spine's
  // section isn't loaded).
  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  if (section && !section->isBuilding() && !RenderLock::peek() && renderer.hasFrameBuffer() &&
      lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
      ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock;  // the page table must not change under the scan
    // Re-check under the lock: peek() and acquisition are not atomic, so the render
    // task may have reset/replaced the section or moved the page in between.
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            // MUST be the effective font, not the raw setting: render() recomputes and stores
            // each line's word x-positions, so scanning with a different font rewrites the
            // page's layout. With a CJK-only family selected (UDDigiKyokasho) every Latin
            // glyph and the space measure zero, which collapsed all word gaps on the page.
            // Same suppressRuby the real render will use: the point of this scan is to warm
            // exactly the glyphs the next page turn draws, and with furigana off the annotation
            // glyphs are never drawn -- scanning them spends SD reads and cache RAM on nothing.
            p->render(renderer, effectiveReaderFontId(), 0, 0, !useFurigana());  // scan only, no pixels
            scope.endScanAndPrewarm();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          }
        }
      }
    }
  }

  // Lazily resume a partial's extension build once the reader nears its watermark. Far from
  // it the rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this
  // session, so reopening a partial deliberately does NOT start it (see the deferral in
  // render()); crossing this margin is the signal that the reader will actually need pages
  // past the watermark soon. Uses the last render's viewport so pagination matches the
  // partial being extended.
  if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
      !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock;
    // Reuse the last render's viewport so the extension paginates identically to the partial.
    const ReaderRenderSpec buildSpec = readerSpec(buildViewportWidth, buildViewportHeight);
    if (!section->startBuild(buildSpec)) {
      // Not fatal: the partial keeps serving its pages; crossing the watermark falls back to
      // the blocking extension in render(). Don't retry every tick.
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial (rebuild from a previous session), pageCount is pinned at the
  // partial's watermark until the build catches up, so the window check would wrongly read
  // "far enough ahead" and stall the build at 0 pages -- then the first turn past the
  // watermark re-parses the whole chapter synchronously. Keep ticking until it finalizes.
  //
  // No window condition any more. Stopping once the build was BUILD_WINDOW_AHEAD pages past the
  // reader left isBuilding() true for the rest of the session, so the chapter never finalized:
  // the total stayed an estimate and the status bar kept its "~" forever. Vertical has always
  // built the whole chapter and shown an exact count, so horizontal was the odd one out.
  // Finishing the chapter is what partials already did ("Keep ticking until it finalizes"), and
  // it stays cheap: two pages per loop tick, only when the render lock is free and the heap gate
  // is open.
  if (section && section->isBuilding() && !RenderLock::peek() && buildTickHeapGate()) {
    RenderLock lock;
    // Re-check under the lock: render() (which also holds the RenderLock) may have finalized the
    // build between the outer isBuilding() check and acquiring the lock here, in which case
    // buildSomeMore() would fail and wrongly reset the section. The heap gate must be re-read
    // too: a render that won the lock race can expand retained glyph buffers, invalidating the
    // pre-lock heap reading. cppcheck can't see the cross-task mutation, so it flags this as
    // always true.
    // cppcheck-suppress knownConditionTrueFalse
    if (section->isBuilding() && buildTickHeapGate()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        // The chapter re-paginated since the saved progress (settings changed): we now know the
        // real page count, so re-render at the remapped page. No-op for an unchanged resume.
        requestUpdate();
      }
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  clearEndOfBookOptionsIfNeeded();

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

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
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

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  // While the end screen suggestion menu is showing it owns Confirm/Back/navigation
  // input. Anything it doesn't handle (e.g. long-press Back to the file browser) falls
  // through to the regular handlers below; page turns are absorbed by the end-of-book
  // block. A Confirm release after a long-press function (bookmark/sync) fired is left
  // to the regular Confirm handler below, which consumes it via ignoreNextConfirmRelease.
  if (handleEndOfBookMenu(ignoreNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)))
    return;

  // Enter reader menu activity on short-press Confirm, the board's menu edge-swipe, or a
  // middle-third tap (see ReaderUtils::isTouchMenuGesture). A long-press
  // that fired a bound function (bookmark or KOReader sync) sets ignoreNextConfirmRelease so the release
  // following the hold does not also open the menu.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
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
      case CrossPointSettings::LP_MENU_DICTIONARY:
        // Hold ~0.4s starts dictionary word selection on the current page.
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS && !showDictionaryMessage) {
          ignoreNextConfirmRelease = true;  // Prevent menu open on the release that follows
          openDictionaryWordSelect(/*pageOnScreen=*/true);
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_READER_MENU:
        // Confirm already opens the menu on release. This option exists for
        // boards whose capacitive Home key supplies the long-press action.
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Home-key boards have no front Confirm button, so a Home-key hold runs the
  // same user-selected long-press action. The SDK emits this event once per
  // hold and suppresses the short Home tap for the same contact.
  if (mappedInput.wasHomeKeyHold()) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case CrossPointSettings::LP_MENU_KOSYNC:
        launchKOReaderSync();
        return;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (!showDictionaryMessage) {
          // The page is on screen for a Home-key hold exactly as it is for a Confirm hold, so
          // word selection starts on it rather than re-rendering (upstream calls this with no
          // argument; this fork's overload takes the flag).
          openDictionaryWordSelect(/*pageOnScreen=*/true);
        }
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
        openReaderMenu();
        return;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Short press Back restores position when viewing a footnote (takes priority over navigation)
  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) return;

  // auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Handle short power button press for footnotes
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    // Inside a footnote the click is REPURPOSED: instead of opening the panel again it jumps back
    // to the reference it was read from. That repurposing is the whole of what
    // pwrBtnFootnoteBack ("Quick-return from footnotes") names, so it is what the setting gates --
    // switched off, the click keeps one meaning everywhere and opens the panel, and Back (handled
    // above) remains the way out of a footnote.
    if (footnoteDepth > 0 && SETTINGS.pwrBtnFootnoteBack) {
      restoreSavedPosition();
    } else {
      openFootnotesPanel();
    }
    return;
  }

  // Manual turns can't outrun the panel, so the guard below refuses to start a
  // turn while a render is in flight or inside a short post-turn gap. But the
  // press itself shouldn't be lost: it's latched into pendingManualTurn and
  // executed here, on the first idle tick after the guard clears.
  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section) {
      // The section was dropped after the latch (re-layout, build failure,
      // bookmark jump): the queued turn no longer names a page.
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    pageTurn(forward);
    return;
  }

  // Handle short power button press for word lookup
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::WORD_LOOKUP &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openDictionaryWordSelect(/*pageOnScreen=*/true);
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) return;

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs > ReaderUtils::SKIP_HOLD_MS;

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

  // Refuse to START a turn while a render is in flight, OR within a short window
  // of the last turn. render() runs on its own task (renderTaskLoop) concurrently
  // with input; a slow AA/image page display lags behind fast taps, and a second
  // turn firing before the first commits its differential baseline writes the
  // panel twice -> two overlapping page segments. RenderLock::peek() catches a
  // render that has already taken the lock (mirrors the automatic-turn guard),
  // but there is a brief window between requesting a turn and the render task
  // acquiring the lock where peek() is still false — a mashed second tap slips
  // through there, which is what still triggered after slow image pages. The
  // lastPageTurnTime gap bridges that startup latency; after it, peek() takes
  // over for the rest of the (variable-length) render. The press is latched, not
  // dropped: the consume block above runs it on the first idle tick, so one
  // eager tap during a slow render still turns the page. Latching (assign, not
  // increment) means mashing collapses to a single queued turn.
  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
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
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
    verticalSection.reset();
  }
}

void EpubReaderActivity::openReaderMenu() {
  const auto span = sectionPageSpan();
  const int sectionPage = span.page;
  const int sectionPageCount = span.count;
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
  bool hasPageText = false;
  if (verticalSection) {
    // getPage() faults the page into the section's SINGLE shared page slot -- the same slot the
    // render task re-faults from render()'s prewarm and image-warm tail, which runs for seconds
    // while holding the render mutex. Faulting it from this (main) task unlocked runs
    // readPage() on one std::vector<VerticalGlyph> from two tasks: clear() in one while the
    // other is mid-push_back frees the buffer under it, and the damage detonates on the NEXT
    // clear() as a free() of a non-heap pointer (crash report: heap_caps_free assert inside
    // getPage -> ~basic_string). The lock must also span every use of the returned pointer.
    RenderLock lock(*this);
    const VerticalPage* page = verticalSection->getPage();
    hasPageText = page && !PageTextExtractor::fromVerticalPage(*page).empty();
  } else if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    hasPageText = !section->getTextFromSectionFile().empty();
  }
  refreshSectionFootnotesIfBuilt();
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
          !sectionFootnotes.empty() || !currentPageFootnotes.empty(), !cachedBookmarks.empty(), hasWordLookup,
          useVerticalText(), useFurigana(), hasPageText, /*imageReaderMinimal=*/false, /*mangaMode=*/false,
          /*hideGenericLookup=*/isJapaneseBook(), /*showPanelsOnlyToggle=*/false, /*panelsOnlyEnabled=*/false,
          /*scrubOnEnter=*/shownPageHasImages_),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        toggleAutoPageTurn(menu.pageTurnOption);
        applyVerticalFuriganaOverride(menu.verticalOverride, menu.furiganaOverride);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
        requestUpdate();
      });
}

// Shared by the reader menu's own result (menu.verticalOverride/furiganaOverride, always the
// unchanged state it was opened with -- these toggles no longer live there, see
// EpubReaderMenuActivity::buildMenuItems) and Reader Settings' result (where they DO change,
// via SettingInfo::DynamicToggle). -1 means "unset/no book context for this toggle" and is a
// no-op; each override only applies, and only resets the section, when it actually differs from
// the book's current effective state.
void EpubReaderActivity::applyVerticalFuriganaOverride(const int8_t verticalOverrideIn,
                                                       const int8_t furiganaOverrideIn) {
  if (verticalOverrideIn >= 0 && verticalOverrideIn != (useVerticalText() ? 1 : 0)) {
    verticalOverride = verticalOverrideIn;
    {
      // Every other section reset in this file takes the render lock: the render task can
      // still be inside its (multi-second, section-touching) warm tail when this result
      // lands, and freeing the section under it is a use-after-free.
      RenderLock lock(*this);
      // Carry the reading position across the mode change; the rebuild on the other side has
      // nothing else to resume from. A page NUMBER does not survive: the two modes paginate the
      // same chapter differently. Two things that do are recorded here, in order of preference:
      //   - the content offset, an exact anchor, since both layouts count source positions
      //     identically (VerticalPage::visibleTextOffset);
      //   - page + chapter total, whose ratio the rebuild remaps proportionally when no offset
      //     can be read (vertical: the cachedChapterTotalPageCount block in render();
      //     horizontal: applyDeferredReposition()).
      cachedVisibleTextOffset.reset();
      if (verticalSection) {
        cachedSpineIndex = currentSpineIndex;
        nextPageNumber = verticalSection->currentPage;
        cachedChapterTotalPageCount = verticalSection->pageCount;
        cachedVisibleTextOffset = verticalSection->getVisibleTextOffsetForPage(verticalSection->currentPage);
      } else if (section) {
        cachedSpineIndex = currentSpineIndex;
        nextPageNumber = section->currentPage;
        // estimatedTotalPages(), not pageCount: mid-build the watermark sits barely ahead of
        // currentPage, so the ratio would be ~1.0 and land the reader at the chapter's end.
        cachedChapterTotalPageCount = section->estimatedTotalPages();
        cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
      }
      section.reset();
      verticalSection.reset();
    }
    // Forcing vertical text on a non-ja book is the same signal isJapaneseBook() covers at
    // open: JP fallback follows it.
    sdFontSystem.setJpFallbackNeeded(renderer, isJapaneseBook() || useVerticalText());
  }
  if (furiganaOverrideIn >= 0 && furiganaOverrideIn != (useFurigana() ? 1 : 0)) {
    furiganaOverride = furiganaOverrideIn;
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  // Menu-launched sub-screens return one level to the reader menu when
  // cancelled (Back button or back gesture, identical on touch and button
  // devices), instead of dropping to the reading surface. Home-gesture exits
  // never run these handlers (ActivityManager replaces the stack), so they
  // cannot re-open the menu.
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      // Preferred path: a bookmark carrying an exact content offset. It is immune to
      // re-pagination, so resolve by content instead of trusting a page number saved under
      // possibly-different settings.
      if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
        RenderLock lock(*this);
        clearDeferredReposition();
        if (section && currentSpineIndex == sync.spineIndex) {
          // Already in this chapter and laid out: resolve straight away, no reload.
          const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
          section->currentPage = page.value_or(std::max(0, sync.page));
        } else {
          // Different chapter: reload and let render() build to the offset before drawing.
          currentSpineIndex = sync.spineIndex;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);  // hint until the offset resolves
          section.reset();
        }
        return;
      }

      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = verticalSection ? verticalSection->pageCount
                                   : section       ? section->estimatedTotalPages()
                                                   : 0;
      const bool cachedPageMatchesActiveSection = (section || verticalSection) && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = activeTotalPages > 0 ? activeTotalPages : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      // Any explicit selection supersedes the session-start resume/reflow anchor,
      // including a selection that is already active.
      RenderLock lock(*this);
      clearDeferredReposition();

      if (currentSpineIndex != targetSpineIndex) {
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
        verticalSection.reset();
      } else if (verticalSection && verticalSection->currentPage != targetPage) {
        RenderLock lock(*this);
        verticalSection->currentPage = std::max(0, targetPage);
      } else if (section && section->currentPage != targetPage) {
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section && !verticalSection) {
        nextPageNumber = targetPage;
      }
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::READER_SETTINGS: {
      // Push settings opened on the Reader tab; Back pops straight back into the book. Font or
      // margin changes are picked up on return: the SD font system reloads to the new selection
      // and the next render's section-cache parameter check rebuilds the layout if needed.
      // Vertical Text / Furigana (moved here from the reader's own quick menu -- see
      // EpubReaderMenuActivity::buildMenuItems) come back the same way the quick menu used to
      // carry them: a MenuResult, read via get_if since SettingsActivity only sets one when
      // showVerticalToggle() was true here, so std::monostate elsewhere is expected.
      startActivityForResult(std::make_unique<SettingsActivity>(renderer, mappedInput, /*initialCategory=*/1,
                                                                /*finishOnBack=*/true, isJapaneseBook(),
                                                                epub ? epub->getLanguage() : std::string{},
                                                                showVerticalToggle(), useVerticalText(), useFurigana(),
                                                                /*mangaMode=*/false,
                                                                /*hideMangaOnlySettings=*/true),
                             [this](const ActivityResult& result) {
                               if (const auto* menu = std::get_if<MenuResult>(&result.data)) {
                                 applyVerticalFuriganaOverride(menu->verticalOverride, menu->furiganaOverride);
                               }
                               sdFontSystem.ensureLoaded(renderer);
                               sdFontSystem.setJpFallbackNeeded(renderer, isJapaneseBook() || useVerticalText());
                               // Reading Orientation now only changes via this screen (removed from the reader's
                               // own quick menu, which used to apply it straight from the popup via
                               // applyOrientation(menu.orientation)): pick up a change the same way onEnter()
                               // does, or it would silently wait for the book's next full open to take effect.
                               ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
                               // Return to the reader MENU (where the user came from), not the page.
                               openReaderMenu();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      // Release the section while the chapter list is up (mirrors the
      // TEXT_SETTINGS path): picking a chapter resets it anyway, and its
      // tens-of-KB footprint is the difference between the chapter list
      // holding its CJK glyph arena (RAM-only repaints) and re-reading
      // glyphs from SD on every row step. Cancel restores via the same
      // cached-position rebuild TEXT_SETTINGS uses.
      {
        RenderLock lock;
        if (section) {
          rememberCurrentContentOffset();
          cachedSpineIndex = currentSpineIndex;
          cachedChapterTotalPageCount = section->pageCount;
          nextPageNumber = section->currentPage;
        }
        section.reset();
      }
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, spineIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock(*this);

            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;

            // If anchor is not empty, it will be used later to calculate the page number.
            pendingAnchor = chapterResult.anchor;

            // Otherwise page 0 will be used.
            nextPageNumber = 0;

            section.reset();
            verticalSection.reset();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      openFootnotesPanel();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
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
      startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent,
                                                                                  /*scrubOnEnter=*/shownPageHasImages_),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 openReaderMenu();
                               } else {
                                 jumpToPercent(std::get<PercentResult>(result.data).percent);
                               }
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      // The menu was the last thing drawn, so the panel has to paint the page itself.
      openDictionaryWordSelect(/*pageOnScreen=*/false);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      std::string pageText;
      if (verticalSection) {
        RenderLock lock(*this);  // shared page slot -- see openReaderMenu()
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
      startActivityForResult(std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath(),
                                                                           /*scrubOnEnter=*/shownPageHasImages_),
                             progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::WORD_LOOKUP: {
      openWordLookupPanel(/*pageOnScreen=*/false);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TRANSLATE_PAGE: {
      std::string pageText;
      if (verticalSection) {
        RenderLock lock(*this);  // shared page slot -- see openReaderMenu()
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
        {
          RenderLock lock(*this);  // the render task may still be in its warm tail
          section.reset();
          verticalSection.reset();
          if (auto* fcm = renderer.getFontCacheManager()) {
            fcm->releaseAllFontMemory();
          }
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
                         : section       ? section->estimatedTotalPages()
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
    // The image extractor holds a raw pointer into this epub (see onEnter);
    // clear it before the early release, mirroring onExit(), or a later image
    // render would call through a dangling context.
    ImageBlock::setExtractor(nullptr, nullptr);
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
      rememberCurrentContentOffset();
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
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
    verticalSection.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  // A page turn is authoritative: do not let a resume/reflow position captured
  // at session start snap the reader back after the incremental build completes.
  {
    RenderLock lock(*this);
    clearDeferredReposition();
  }
  // Tilt- and auto-page-turn calls reach here without a button press, so the loop()-side stamp
  // bump didn't fire -- cancel a running image warm before the chapter-boundary branches below
  // block on the RenderLock it holds.
  imageWarmInputStamp_.fetch_add(1, std::memory_order_relaxed);
  pendingHorizontalImageRefine_.store(NO_IMAGE_REFINE, std::memory_order_relaxed);
  lastTurnForward_.store(isForwardTurn, std::memory_order_relaxed);

  // A vertical chapter is still building on the render task: pageCount is 0 until the build
  // ends, so the normal path below would misread every press as "past the last page", block
  // on the RenderLock for the rest of the build, and then jump to the NEXT SPINE (observed on
  // device: one press during the build teleported the reader to the end of the book). Route
  // the turn to the build's page-request hook instead -- the page shows as soon as it exists.
  if (verticalBuildInProgress_.load(std::memory_order_relaxed)) {
    const int shown = earlyDisplayedPage_.load(std::memory_order_relaxed);
    if (shown >= 0 && verticalSection) {
      if (isForwardTurn) {
        // Ahead of the build frontier, so it costs nothing: the page is served straight from
        // RAM as it is laid out. Reading forward through a building chapter works normally.
        verticalSection->requestPageDuringBuild(shown + 1);
      } else {
        // Backward needs a page already written, which means a cache read-back the build's own
        // working set cannot fund. Say so instead of ignoring the press.
        verticalSection->noteBackTurnDuringBuild();
      }
    }
    lastPageTurnTime = millis();
    return;
  }

  const int curPage = verticalSection ? verticalSection->currentPage : (section ? section->currentPage : 0);
  const int pgCount = verticalSection ? verticalSection->pageCount : (section ? section->pageCount : 0);

  if (isForwardTurn) {
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (curPage < pgCount - 1 || (section && !verticalSection && section->isBuilding())) {
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

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
  nextPageNumber = 0;
  pendingPageJump = std::numeric_limits<uint16_t>::max();
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

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
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
    renderEndOfBook("ERS");
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

  orientedMarginBottom += readerBottomReserve(useVerticalText());

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  lastViewportWidth = viewportWidth;
  lastViewportHeight = viewportHeight;
  // Capture for loop()'s lazy partial-extension start (must match this render's layout params).
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = readerSpec(viewportWidth, viewportHeight);

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
                               static_cast<bool>(SETTINGS.bookCssMargins),
                               SETTINGS.lineSpacing,
                               useFurigana()};
    if ((section || verticalSection) && currentSig != sectionLayoutSig) {
      LOG_DBG("ERS", "Layout params changed; reflowing section in place");
      // Anchor the position by content before the sections go: a reflow re-paginates, so the
      // page number about to be recorded below is only the fallback. Vertical can answer this
      // now too, so a font or spacing change no longer costs vertical readers their place.
      rememberCurrentContentOffset();
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
      // The relayout repaints THIS page with its lines shifted under whatever the panel
      // already shows. A mid-cycle FAST erases the old glyphs with the weak DU transition
      // only, leaving the previous layout crisply superimposed (photographed on device
      // after a cache-version bump relaid the book out). Force the absolute HALF pass.
      pagesUntilFullRefresh = 1;
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
    // Release only on evidence that the heap is inadequate, not merely low: a render that served
    // every glyph proves this level works, and releasing anyway hands the next render a cold font
    // cache. forceFontReleaseCheck_ keeps the unconditional release for the first render after
    // entering the activity -- the resume path the floor exists for, where a fragmented boot heap
    // otherwise loses glyph chunks mid-render.
    //
    // ...or once the heap has decayed past the point where the bulk prewarm can run. maxAlloc decays
    // monotonically while reading and never recovers on its own; this release is the only thing that
    // coalesces it. Below the prewarm floor every page costs ~40 on-demand SD glyph loads (~300ms)
    // instead of one prewarm (~90-150ms), so one rebuild buys many warm turns. Do not widen this back
    // to a bare 40K floor: that fires on nearly every turn for no gain.
    bool starvedSinceLastRender = forceFontReleaseCheck_;
    if (auto* fd = fcm->getDecompressor()) {
      const uint32_t starvedNow = fd->getStarvedGlyphCount();
      starvedSinceLastRender = starvedSinceLastRender || starvedNow != starvedGlyphsAtLastRender_;
      starvedGlyphsAtLastRender_ = starvedNow;
    }
    forceFontReleaseCheck_ = false;
    const bool cannotPrewarm = maxAlloc < PREWARM_MIN_ALLOC_READ;
    if (maxAlloc < RESUME_HEAP_FLOOR && (starvedSinceLastRender || cannotPrewarm)) {
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
      // makeUniqueNoThrow, not bare new: with -fno-exceptions a failed new aborts the firmware
      // instead of returning null, and this allocation can land on a badly fragmented heap.
      verticalSection = makeUniqueNoThrow<VerticalSection>(epub, currentSpineIndex, renderer);
      if (!verticalSection) {
        LOG_ERR("ERS", "OOM allocating VerticalSection");
        // Mark this spine failed so render() stops retrying the same allocation every frame --
        // the same guard the build-failure path uses to avoid an infinite indexing/error loop.
        failedVerticalSpineIndex = currentSpineIndex;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderStatusBar();
        renderer.displayBuffer();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }
      sectionFootnotes.clear();  // vertical sections don't collect footnotes

      const int fontId = effectiveReaderFontId();
      if (!verticalSection->loadSectionFile(fontId, viewportWidth, viewportHeight, SETTINGS.lineSpacing,
                                            useFurigana())) {
        LOG_DBG("ERS", "Vertical cache not found, building...");
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        // Same force every horizontal Indexing-popup site applies: the popup paints FAST, and a
        // rebuilt layout replaces it with shifted content -- HALF-clear or both ghost under the page.
        pagesUntilFullRefresh = 1;

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
        // Seed earlyDisplayedPage_ with the target page (not -1) BEFORE the build starts, so a
        // page turn pressed in the first seconds -- before the early-render hook has fired --
        // still has a valid baseline to offset from and records its request, instead of being
        // silently dropped. Percent jumps set no hook (they need the final pageCount), so they
        // keep -1 and mid-build turns stay inert there, matching the classic wait path.
        int earlyTarget = -1;
        if (!pendingPercentJump) {
          earlyTarget = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber > 0 ? nextPageNumber : 0);
          verticalSection->setEarlyRenderHook(this, &EpubReaderActivity::earlyRenderVerticalPageThunk, earlyTarget);
          verticalSection->setBuildNoticeHook(this, &EpubReaderActivity::buildNoticeThunk);
        }

        earlyPageActuallyDisplayed_ = false;
        earlyDisplayedPage_.store(earlyTarget, std::memory_order_relaxed);
        verticalBuildInProgress_.store(true, std::memory_order_relaxed);
        const bool built = verticalSection->createSectionFile(fontId, viewportWidth, viewportHeight,
                                                              SETTINGS.lineSpacing, useFurigana());
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

      // An explicit jump (bookmark, chapter list, percent) is a deliberate navigation and must
      // outrank the position being carried across a re-pagination. Recorded before the branch
      // below consumes it.
      const bool hadExplicitPageJump = pendingPageJump.has_value();
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
        // Left UNCLAMPED on purpose: nextPageNumber is numbered in whatever pagination saved it
        // (a horizontal layout, or this one before a settings change), and vertical pagination is
        // typically much shorter. Clamping first would collapse "page 204 of 213" onto the last
        // vertical page and the remap below would then read that as ~96% of the OLD count -- the
        // position drifted backwards on every switch. Clamp once, after the remap.
        verticalSection->currentPage = nextPageNumber;
      }
      pendingAnchor.clear();

      // Content anchor first, page fraction only as the fallback. The offset names an exact
      // character and is immune to re-pagination; the fraction is a guess that lands the reader
      // up to a page away and drifts a little further on every switch.
      bool resolvedByOffset = false;
      if (currentSpineIndex == cachedSpineIndex && cachedVisibleTextOffset.has_value() && !hadExplicitPageJump &&
          !pendingPercentJump) {
        if (const auto page = verticalSection->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
          verticalSection->currentPage = *page;
          resolvedByOffset = true;
        }
      }
      cachedVisibleTextOffset.reset();
      if (cachedChapterTotalPageCount > 0) {
        if (!resolvedByOffset && currentSpineIndex == cachedSpineIndex &&
            verticalSection->pageCount != cachedChapterTotalPageCount) {
          const float progress =
              static_cast<float>(verticalSection->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
          verticalSection->currentPage = static_cast<int>(progress * verticalSection->pageCount);
        }
        cachedChapterTotalPageCount = 0;
      }

      if (verticalSection->pageCount == 0 || verticalSection->currentPage < 0) {
        verticalSection->currentPage = 0;
      } else if (verticalSection->currentPage >= verticalSection->pageCount) {
        verticalSection->currentPage = verticalSection->pageCount - 1;
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

    if (earlyPageActuallyDisplayed_ &&
        earlyDisplayedPage_.exchange(-1, std::memory_order_relaxed) == verticalSection->currentPage) {
      earlyPageActuallyDisplayed_ = false;
      lastRenderedVPage_ = verticalSection->currentPage;
      saveProgress(currentSpineIndex, verticalSection->currentPage, verticalSection->pageCount, verticalOverride,
                   furiganaOverride);
      LOG_DBG("ERS", "Keeping early-rendered page %d; skipping duplicate refresh", verticalSection->currentPage);
      showPendingSyncSaveError();
      return;
    }
    earlyPageActuallyDisplayed_ = false;
    earlyDisplayedPage_.store(-1, std::memory_order_relaxed);

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
        if (verticalSection->lastReadHeapRefused()) {
          // Transient low heap, NOT corruption: the on-disk cache is valid. Clearing it here
          // would force an expensive rebuild (which needs far more heap and would fail too).
          // Reclaim the font memory to recover headroom and re-render; the retry then fits.
          LOG_ERR("ERS", "Vertical page read refused on low heap; keeping cache and retrying");
          if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
          prewarmedVPage_ = -1;
          requestUpdate();
          automaticPageTurnActive = false;
          showPendingSyncSaveError();
          return;
        }
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
        const int reserve = readerBottomReserve(/*verticalMode=*/false);
        // Release ImageBlock's pixel-cache RAM slot on every exit from this branch. renderContents()
        // (the horizontal path) has always had this guard; vertical never did, so any time the slot
        // DID engage here its 6x16KB stayed resident for good. Observed on device: the heap fell
        // 132KB -> 30KB across a single image page and never recovered, after which maxAlloc sat at
        // 8692 and everything downstream failed -- "Page record needs 8424 bytes, refusing read"
        // spinning every 30ms, and image re-extraction unable to obtain its 32KB inflate window.
        // Latent for as long as the slot's heap gate kept declining; a leak the moment it stopped.
        struct PxcSlotGuard {
          ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
        } pxcSlotGuard;

        // NOT releasing font memory here: measured, it returns ~2.7KB (and 0 when the caches are
        // already cold), nowhere near the ~65KB the slot's gate needs, while costing a glyph and
        // kern-table reload on the next text page. The gate is cleared by not leaking, not by this.
        const auto drawImagePage = [&]() {
          // Rotated image pages hand render() the natural dims and (0, 0): it switches to the
          // adjacent orientation, fits and centres there. Disabled for one commit (0d4d1aa0) while
          // render() had no rotation support and silently drew nothing; restored with it.
          if (vpage->imageRotated) {
            ImageBlock imgBlock(vpage->imagePath, vpage->imageSrcPath, vpage->imageWidth, vpage->imageHeight);
            imgBlock.setRotated(true, static_cast<int16_t>(reserve));
            imgBlock.render(renderer, 0, 0);
          } else {
            int iw = vpage->imageWidth;
            int ih = vpage->imageHeight;
            // Shared with warmNextPageImageCache so a background-warmed cache has EXACTLY the
            // dimensions this render asks for.
            ImageBlock::fitWithin(viewportWidth, viewportHeight, iw, ih);
            ImageBlock fitBlock(vpage->imagePath, vpage->imageSrcPath, static_cast<int16_t>(iw),
                                static_cast<int16_t>(ih));
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

        // The grayscale refine below re-reads the pixel cache several times (~1s+) and the BW
        // image is ALREADY a valid picture on the persistent e-ink. Snapshot the input stamp so
        // the refine can be ABANDONED the instant the reader turns the page: flipping through
        // illustrations then feels instant (BW shows fast, the next turn is honoured immediately)
        // while dwelling on a page still refines all the way to 4-level grayscale.
        imageWarmStampSnapshot_ = imageWarmInputStamp_.load(std::memory_order_relaxed);

        // A 1-bit BMP has no gray tones for the planes to lift, and the BMP decoder writes no
        // .pxc cache, so each strip pass below would be a full SD re-decode of an image the BW
        // pass already displayed completely -- measured 1587ms per turn (3 decodes) plus the
        // ~1.7s gray waveform for zero visual change. Converter-produced books ship exactly
        // these mono BMPs. One header read decides it.
        const bool monoBmp = FsHelpers::hasBmpExtension(vpage->imagePath) &&
                             BmpToFramebufferConverter::isMonochromeStatic(vpage->imagePath);

        if (!monoBmp && renderer.supportsStripGrayscale() && !imageWarmShouldCancel(this)) {
          const int gh = renderer.getDisplayHeight();
          const int gwBytes = renderer.getDisplayWidthBytes();
          // Each grayscale strip re-reads the WHOLE pixel cache (the .pxc is row-major in logical
          // image space, so a physical band can't seek to just its rows), so the read cost scales
          // with the strip COUNT. Taller strips = fewer strips = fewer whole-cache re-reads: at
          // 160 rows a 480px page is 3 strips instead of 6, roughly halving the ~13 reads that made
          // image page turns slow. The taller scratch is 16KB vs 8KB; if that doesn't fit on a
          // fragmented (X3) heap, fall back to the original 80-row strip -- never worse than before,
          // and the BW-only path below still catches a total allocation failure.
          int stripRows = 160;
          auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * stripRows);
          if (!scratch) {
            stripRows = 80;
            scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * stripRows);
          }
          if (!scratch) {
            LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); image stays BW this page", gwBytes * stripRows);
          } else {
            bool cancelled = false;
            renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
            for (int y = 0; y < gh && !cancelled; y += stripRows) {
              if (imageWarmShouldCancel(this)) {
                cancelled = true;
                break;
              }
              const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
              renderer.beginStripTarget(scratch.get(), y, rows);
              renderer.clearScreen(0x00);
              drawImagePage();
              renderer.endStripTarget();
              renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
            }
            renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
            for (int y = 0; y < gh && !cancelled; y += stripRows) {
              if (imageWarmShouldCancel(this)) {
                cancelled = true;
                break;
              }
              const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
              renderer.beginStripTarget(scratch.get(), y, rows);
              renderer.clearScreen(0x00);
              drawImagePage();
              renderer.endStripTarget();
              renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
            }
            renderer.setRenderMode(GfxRenderer::BW);
            // On cancel, don't show the half-built grayscale -- leave the BW image already on the
            // e-ink. Either way reset the controller's grayscale planes; the next page turn does a
            // full clear+render+display, so the rebase content is transient.
            if (!cancelled) renderer.displayGrayBuffer();
            renderer.cleanupGrayscaleWithFrameBuffer();
          }
        }
        // Gray charge in the image region needs the HALF ghost-cleanup on the next page. A mono
        // BMP skipped the gray pass entirely, so it left no charge -- normal refresh cadence.
        if (!monoBmp) pagesUntilFullRefresh = 1;
        imagePageDisplayed = true;
      } else {
        renderedVPage_ = verticalSection->currentPage;  // see the post-render warm block below
        const bool vGlyphsWarm = prewarmedVPage_ == verticalSection->currentPage;
        renderVerticalPageBody(*vpage, vGlyphsWarm);
        // Re-assert the claim for the page the body just prewarmed (it cleared it above).
        if (!vGlyphsWarm) prewarmedVPage_ = verticalSection->currentPage;
      }
      LOG_DBG("ERS", "Rendered vertical page in %dms", millis() - start);
    }

    // Async: start the waveform and return, so runPostRenderTail() below runs DURING the panel's
    // ~500ms refresh rather than after it. Worth ~450ms per turn.
    //
    // Contract: nothing between here and waitRefreshComplete() may touch the framebuffer. The tail
    // is safe (its glyph warm renders in scan mode, which draws nothing); popups, screenshots and
    // the image warm are not, and run after the wait.
    shownPageHasImages_ = imagePageDisplayed;
    const bool overlapRefresh = !imagePageDisplayed && renderer.supportsAsyncRefresh();
    if (!imagePageDisplayed) {  // image pages already displayed (double-fast + grayscale planes)
      renderStatusBar();
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
    }
    runPostRenderTail(viewportWidth, viewportHeight, /*vertical=*/true, 0, 0);

    // End of the overlap window. Everything past this point may draw: the popups below, the
    // screenshot's framebuffer read, and the image warm's cache decode.
    if (overlapRefresh) renderer.waitRefreshComplete();

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
    // makeUniqueNoThrow, not bare new: with -fno-exceptions a failed new aborts the firmware
    // instead of returning null, and this allocation can land on a badly fragmented heap.
    section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer);
    if (!section) {
      LOG_ERR("ERS", "OOM allocating Section");
      // Mark this spine failed so render() stops retrying the same allocation every frame --
      // the same guard the build-failure path uses to avoid an infinite indexing/error loop.
      failedSectionSpineIndex = currentSpineIndex;
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    // Fresh section, fresh chance: a failed lazy extension start in a previous
    // section must not suppress watermark-triggered rebuilds for this one.
    partialRebuildStartFailed = false;

    // A finalized cache serves every page as-is. A partial cache (suspended build from a
    // previous session) serves its pages instantly too, but a build must still run to lay
    // out the rest -- it re-parses from the top in the background (HTML already cached,
    // pages are deterministic) and finalizes, so the partial machinery retires itself.
    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      // Matching render params means identical pagination, so the saved page number is valid
      // as-is: consume any pending settings-change reposition. Without this, a chapter total
      // saved while the section was still building (i.e. a watermark, not the real count)
      // would remap the resume page against the finalized count and teleport the reader.
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    // Land this render by content offset when one applies. An explicit bookmark jump
    // (pendingOffsetJump) always wins -- it is a deliberate navigation to a stored content anchor.
    // Otherwise fall back to the settings-change reposition: read after the cache-hit reset above,
    // a spec match means the saved page number still names the same content so there is nothing to
    // reposition, while a page jump or fragment anchor is a deliberate navigation that outranks it.
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      // Jumps that need the final pagination or the anchor map -- explicit page jumps,
      // fragment anchors, percent jumps, and cross-setting progress repositioning -- can't
      // resolve their landing page until the whole chapter is laid out, so they take the full
      // (blocking) build with the indexing popup. Everything else -- plain forward reads, resume,
      // and explicit page jumps -- only needs a specific page, so it builds incrementally to that
      // page and finishes the rest in loop(). The settings-change reposition (cachedChapterTotal*)
      // is NOT a full-build trigger: it's deferred to applyDeferredReposition() once the real page
      // count is known, so it never blocks the first page.
      // Only a percent jump truly needs the whole chapter up front (percent -> page needs the final
      // page count). Anchor jumps (TOC / chapter select / footnotes) resolve incrementally below --
      // the anchor is recorded as its page is laid out, so a chapter-top anchor lands on page 0
      // without indexing the whole chapter.
      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        // The popup's own refresh is a plain FAST, so force the page that replaces it onto the HALF
        // ghost-cleanup path -- otherwise the "INDEXING" text ghosts under the rendered page.
        pagesUntilFullRefresh = 1;
        // No popup redraws while the framebuffer is lent to the build below;
        // the panel holds the popup displayed above (e-ink is persistent).
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        // Lend the framebuffer's 48 KB to the blocking full build; restored
        // (white) at scope exit, and the page render below redraws everything.
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();  // restore before anything draws
          showBuildError();
          return;
        }
        loan.end();
      } else {
        // Lay out just enough to show the landing page; loop() builds the rest behind it. Show the
        // indexing popup up front only when the build will actually be slow: a large spine (its
        // whole HTML must be inflated before page 1 can lay out -- the giant single-spine case), or
        // a deep resume/jump that must lay out many pages to reach the landing page. Tiny sections
        // build in a blink and stay popup-free.
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        // Landing well inside a partial: the page (or anchor, via the on-disk map) is already
        // servable, so don't restart the extension build now -- it re-lays out the WHOLE chapter
        // from page 0 (minutes of background CPU + SD writes on a giant spine), pure waste when
        // the reader never nears the watermark this session. loop() starts it lazily once the
        // reader is within PARTIAL_REBUILD_START_MARGIN pages of the watermark.
        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          // Popup only when the build will actually be slow: a big spine whose HTML still needs
          // inflating (the multi-second cost), or a deep page target. A reopen with cached HTML builds
          // fast, so no popup -- that's what made an already-indexed book look like it was reindexing.
          // A partial cache that already covers the target page shows it instantly: never popup.
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            // An anchor jump's cost is bounded by the anchor's page, not `target`. An anchor already
            // in the on-disk map (partial or finalized cache) lands instantly: no popup. Otherwise it
            // lies beyond the indexed watermark and the build may lay out the whole spine to find it,
            // so gate on spine size alone -- laying out a big spine takes seconds even with cached
            // HTML. Ordinary chapter-top TOC jumps resolve on page 0 and stay popup-free.
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            // HALF-clear the popup when the page replaces it, else "INDEXING" ghosts under the page.
            pagesUntilFullRefresh = 1;
          }
          // Mid-build popup surfacing for slow builds the predictive gates can't
          // see (image extraction/probing inside a single page, or any chunk
          // overrunning the deadline). The parser fires the callback before the
          // first image probe; buildPopupPending gates it to this blocking phase
          // so a background build in loop() can never draw over a displayed page.
          buildPopupPending = !showPopup;
          // Building can need a large single contiguous allocation (e.g. the zip inflate window) --
          // free the font decompressor's buffers (hot group + glyph slab) first to hand that
          // headroom to the build, same rationale as the identical call on the vertical-mode build
          // path above.
          if (auto* fcm = renderer.getFontCacheManager()) {
            fcm->releaseAllFontMemory();
          }

          const unsigned long buildStartMs = millis();
          bool started;
          {
            // Lend the framebuffer's 48 KB to startBuild only (the spine HTML
            // inflation peak). The chunk loop below runs without it so the popup
            // can draw mid-build; background chunks never had the loan either.
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            failedSectionSpineIndex = currentSpineIndex;
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            // Anchor jump: build until the anchor's page is laid out (usually page 0), checking a
            // partial's on-disk anchor map too so an already-indexed anchor resolves immediately.
            // Re-pagination: build until the content the reader was on has been laid out. Costs the
            // same parse work as the old page target did -- it is the same content -- but it stops
            // at the right place, so the landing page is known before anything is drawn.
            // Otherwise: build until the target page exists. loop() builds the rest behind it.
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              // The predictive gates guessed fast but the build blew the silent budget.
              showBuildPopup();
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              failedSectionSpineIndex = currentSpineIndex;
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    section->loadSectionFootnotes(sectionFootnotes);
    if (!sectionFootnotes.empty()) {
      LOG_DBG("ERS", "Chapter footnotes: %u", static_cast<unsigned>(sectionFootnotes.size()));
    }

    if (pendingPageJump.has_value()) {
      // A jump past the watermark of a still-building section is legitimate -- render()'s
      // build pump lays out the rest. Only a finished section can clamp.
      if (section->isBuilding()) {
        section->currentPage = *pendingPageJump;
      } else if (section->pageCount == 0) {
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
      }
    }

    // The chapter re-paginated, so nextPageNumber above named the old pagination's page.
    // The build loop stopped once this offset was laid out, so resolve it now, before the
    // first draw. Leaving it to applyDeferredReposition() is what made the stale page paint
    // first and then jump when the background build finished the chapter.
    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        // This anchor has now established the landing page. It must not be
        // applied again by applyDeferredReposition() after a background build
        // finishes, or it would undo page turns made during that build.
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      // An explicit bookmark/sync target supersedes any stale session-start
      // resume anchor even when its offset cannot be resolved (for example an
      // empty chapter).
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();  // one-shot explicit jump: consumed on this render

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
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

  // Extend the build to the requested page if needed (for partials and in-progress builds).
  // This runs every render, so it covers both the first page and any forward turn that gets
  // ahead of the background builder; pages already built do no work here.
  //
  // Crossing a partial's watermark before the extension rebuild has caught up means a
  // synchronous wait spanning the remaining prefix re-layout -- potentially tens of
  // seconds on a giant spine. Show the indexing popup so it isn't a silent freeze
  // (the page that replaces it takes the HALF ghost-cleanup path). Ordinary window
  // catch-ups on a non-partial build are a page or two and stay popup-free.
  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    // Start a build to extend a partial toward the requested page.
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    // Extend until either the target page exists or the build completes.
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  // For an in-progress incremental build, make sure the page we're about to show has been laid out.
  if (section->isBuilding()) {
    // Same reasoning as the partial extension above: a catch-up of a page or two is quick and
    // stays silent, but a long one is a multi-second freeze on a blank screen and needs to say
    // what it is doing. Toggling a book between vertical and horizontal is exactly that case:
    // the two modes use different viewport heights, so the cache fails its parameter check and
    // the rebuild starts at page 0 while the reader is deep in the chapter. That path showed no
    // indexing popup at all.
    constexpr int kSilentCatchUpPages = 4;
    if (section->currentPage - static_cast<int>(section->pageCount) > kSilentCatchUpPages) {
      GUI.drawPopup(renderer, tr(STR_INDEXING));
      // The popup refreshes FAST, so force the page replacing it onto the HALF ghost-cleanup
      // path or "INDEXING" ghosts under the text.
      pagesUntilFullRefresh = 1;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  // The requested page is now as built as it will get. If it still lands past the end,
  // clamp to the last real page: the UINT16_MAX "last page" sentinel from backward chapter
  // navigation, an explicit jump beyond a finished chapter, or a stale saved position.
  // Guarded on !isBuilding() because a still-building section's pageCount is only the current
  // watermark (not the final count) and has already been driven far enough by the loops above.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  // Apply a deferred settings-change reposition now that the real page count is known (a no-op for
  // a plain resume / unchanged pagination). If still building, this defers to loop() on completion.
  applyDeferredReposition();

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
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      // Retrying rebuilds a transiently corrupt section and usually recovers, but a page that keeps
      // failing would loop forever on a blank screen, so bound the retries before giving up.
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      // Abandon (not suspend) any active build BEFORE clearing: clearCache deletes the files,
      // and the destructor's suspend would otherwise commit tables into a deleted handle.
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;  // Reset so a later user-initiated navigation can try afresh
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();  // Try again after clearing cache
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;  // Reset the retry counter once a page loads cleanly

    // A page that loads without error but draws nothing is indistinguishable from a render
    // fault on screen, so name it. Only the fault is logged -- this runs on every page turn.
    if (p->elements.empty()) {
      LOG_ERR("ERS", "Page %d of %u loaded with 0 elements (building=%d partial=%d complete=%d)", section->currentPage,
              static_cast<unsigned>(section->pageCount), section->isBuilding() ? 1 : 0, section->isPartial() ? 1 : 0,
              section->isBuildComplete() ? 1 : 0);
    }

    // Cache this page's content offset (read alongside the page, no extra file open) so
    // saveProgress and addBookmark can use it without reopening section.bin.
    currentPageVisibleOffset = p->visibleTextOffset;

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);
    if (!currentPageFootnotes.empty()) {
      LOG_DBG("ERS", "Page footnotes: %u (first '%s')", static_cast<unsigned>(currentPageFootnotes.size()),
              currentPageFootnotes[0].number);
    }

    const auto start = millis();
    const uint32_t currentKey =
        (static_cast<uint32_t>(currentSpineIndex) << 16) | static_cast<uint16_t>(section->currentPage);
    const bool grayscaleRefineOnly =
        requestedHorizontalImageRefine_.exchange(NO_IMAGE_REFINE, std::memory_order_relaxed) == currentKey;
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft,
                   /*glyphsAlreadyWarm=*/prewarmedHPage_ == section->currentPage, grayscaleRefineOnly);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
  }
  runPostRenderTail(viewportWidth, viewportHeight, /*vertical=*/false, orientedMarginLeft, orientedMarginTop);

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

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (useVerticalText()) {
    // Fire over the last few pages, and on short chapters (image-only illustration chapters are
    // one page each -- with the old penultimate-page-only trigger a run of them showed the
    // Indexing popup on every page turn).
    //
    // The window must be wide enough for SEVERAL attempts, not one. The heap gate below rejects
    // on transient fragmentation (observed: maxAlloc 69620 on one turn, 114676 two turns later),
    // so a single-attempt window loses the chapter to that sampling noise and the transition pays
    // a multi-second foreground build. Only the first attempt to pass does any work.
    constexpr int SILENT_INDEX_WINDOW_PAGES = 5;
    if (!epub || !verticalSection || verticalSection->pageCount < 1) return;
    if (verticalSection->currentPage < verticalSection->pageCount - SILENT_INDEX_WINDOW_PAGES) return;

    const int nextSpineIndex = currentSpineIndex + 1;
    if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) return;

    // A skip below set a backoff: retrying every tick releases the font caches each time
    // (cold glyphs on the next turn) while the heap plateau that caused the skip rarely
    // moves within a second. One attempt per backoff window is plenty.
    if (silentIndexBackoffUntilMs_ != 0 && millis() < silentIndexBackoffUntilMs_) return;

    VerticalSection nextVSection(epub, nextSpineIndex, renderer);
    const int fontId = effectiveReaderFontId();
    if (nextVSection.loadSectionFile(fontId, viewportWidth, viewportHeight, SETTINGS.lineSpacing, useFurigana()))
      return;

    constexpr uint32_t SILENT_VBUILD_MIN_ALLOC = 96 * 1024;

    // Do NOT add a heap pre-gate above the release below. Free heap while reading sits near this
    // floor (~95K) and the release is worth ~40-50K, so any pre-release check rejects attempts
    // that would have passed. The post-release gate is the only meaningful reading. Skipping a
    // release costs ~250ms; the foreground build a missed index causes costs 5-13s.
    //
    // The vertical build is the most memory-intensive step in the reader, and this
    // silent path runs it at the worst heap moment: right after a page render, with
    // the glyph slab fully warmed AND the current chapter still resident. Hand the
    // build the font memory first, like the mainline vertical build does. This is why the call
    // runs BEFORE the idle glyph warm: the release empties the mini-font cache, so warming first
    // would throw that work away and leave the next turn cold.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
      // The release just emptied the mini-font cache, so the idle warm's page is no longer
      // warm. Without this the NEXT render trusts prewarmedVPage_, skips prewarmVerticalPageGlyphs
      // entirely, and resolves every glyph one at a time through the on-demand miss path.
      prewarmedVPage_ = -1;
      prewarmedHPage_ = -1;
    }

    // Post-release gate: if the largest block is STILL small, this build would run the whole
    // gauntlet degraded -- observed at maxAlloc=63476: styled blocks skipped, glyphs dropped,
    // and the section stamped stale THE MOMENT it was written. That is throwaway work that
    // also leaves short pages on screen if the reader pages into it this session. Leave the
    // section unbuilt instead: a roomier later tick retries, and the foreground open path
    // (which frees more up front and early-renders) builds it properly on arrival.
    if (ESP.getMaxAllocHeap() < SILENT_VBUILD_MIN_ALLOC) {
      LOG_DBG("ERS", "Silent vertical index skipped, heap too tight (maxAlloc=%u)", ESP.getMaxAllocHeap());
      // Backoff must stay well under WINDOW * turn duration (~600ms/turn), or one rejection
      // consumes the whole window. A rejected attempt costs the font rebuild the release above
      // forces (~250-400ms on the next render) -- cheap against the foreground build it avoids.
      silentIndexBackoffUntilMs_ = millis() + 1500;
      return;
    }
    silentIndexBackoffUntilMs_ = 0;

    LOG_DBG("ERS", "Silently indexing next vertical chapter: %d (maxAlloc=%u)", nextSpineIndex, ESP.getMaxAllocHeap());
    if (!nextVSection.createSectionFile(fontId, viewportWidth, viewportHeight, SETTINGS.lineSpacing, useFurigana())) {
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
  // Never while this chapter is still building: the silent build would fight it for the
  // heap and the RenderLock, and loop() has more of this chapter to lay out first.
  if (section->isBuilding()) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  const ReaderRenderSpec spec = readerSpec(viewportWidth, viewportHeight);
  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(spec) && !nextSection.isPartial()) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  if (!nextSection.createSectionFile(spec)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  // Re-derive the page from the saved content offset after a settings reflow.
  // Older 4/6-byte progress files retain the page-fraction fallback.
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
    // Repaginated: even an unmoved page NUMBER now holds shifted content, and the panel still
    // shows the old layout. A mid-cycle FAST's weak DU erase leaves that old layout crisply
    // superimposed under the new one (photographed on device); take the absolute HALF pass.
    pagesUntilFullRefresh = 1;
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

// Speculative + bookkeeping work that runs while the finished page is on screen. Order and gating
// here are load-bearing; see nextTurnAlreadyRequested() in the header for the rule.
void EpubReaderActivity::runPostRenderTail(const uint16_t viewportWidth, const uint16_t viewportHeight,
                                           const bool vertical, const int marginLeft, const int marginTop) {
  const bool skimming = nextTurnAlreadyRequested();

  // FIRST, and never gated. Builds the next chapter's section cache while the last pages of this
  // one are on screen; without it a chapter transition pays a multi-second foreground build with
  // an Indexing popup. It only does work on a chapter's closing pages -- which are exactly the
  // pages a reader skims through, so gating it on `skimming` suppressed it whenever it mattered.
  //
  // Must precede the warm below: it calls releaseAllFontMemory() before building, which empties
  // the mini-font cache and would discard a warm done first.
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);

  // Warm the neighbouring page's glyphs so the next turn renders from RAM instead of paying the
  // scan plus SD bulk load at button time. Direction-adaptive: the next page after a forward
  // turn, the previous after a backward one, so sustained paging either way stays warm. The
  // mini-font cache holds one page per style, so only the turn after a direction REVERSAL is
  // cold; that is inherent, not a defect.
  //
  // Gated on `skimming`: this prepares a page a rapidly-paging reader will skip past, and it runs
  // on the render task, so it delays the turn being waited on (~430ms, mostly the mini-kern
  // build).
  //
  // Runs only after a REAL page change -- warming the neighbour of a re-rendered page evicts
  // glyphs that page still needs.
  const bool forward = lastTurnForward_.load(std::memory_order_relaxed);
  if (vertical) {
    // renderedVPage_, never currentPage: a render takes 100-700ms and a button press during it
    // advances currentPage, so the target would overshoot by one -- loading a page the reader is
    // not going to next and evicting the one they are.
    const bool pageChanged = renderedVPage_ != lastRenderedVPage_;
    lastRenderedVPage_ = renderedVPage_;
    const int warmTarget = forward ? renderedVPage_ + 1 : renderedVPage_ - 1;
    if (!skimming && pageChanged && warmTarget >= 0 && warmTarget < verticalSection->pageCount) {
      prewarmedVPage_ = -1;
      // getPage() also leaves the page in the section's single-page read cache, so the turn skips
      // the SD page read as well.
      if (const VerticalPage* np = verticalSection->getPage(warmTarget); np && !np->isImagePage()) {
        if (prewarmVerticalPageGlyphs(*np)) prewarmedVPage_ = warmTarget;
      }
    }
  } else {
    prewarmedHPage_ = -1;
    // Warming loads an extra page transiently, so take the classic cold turn when the largest
    // free block is tight.
    constexpr uint32_t IDLE_WARM_MIN_ALLOC = 32 * 1024;
    const int warmTarget = forward ? section->currentPage + 1 : section->currentPage - 1;
    if (auto* fcm = renderer.getFontCacheManager(); fcm && !skimming && ESP.getMaxAllocHeap() >= IDLE_WARM_MIN_ALLOC &&
                                                    warmTarget >= 0 && warmTarget < section->pageCount) {
      if (auto np = section->loadPageAt(warmTarget)) {
        // createPrewarmScope() clears the cache in its constructor, freeing the MAX_PAGE_SLOTS
        // page-buffer slots, so each warm REPLACES the previous page's glyphs -- slots do not
        // accumulate across turns even though release() keeps the result. The scan pass draws
        // nothing (GfxRenderer skips drawing while scanning), so the displayed framebuffer is
        // untouched and this is safe inside an async refresh window.
        auto scope = fcm->createPrewarmScope();
        np->render(renderer, effectiveReaderFontId(), marginLeft, marginTop, !useFurigana());
        scope.endScanAndPrewarm();
        scope.release();  // keep the warm resident for the upcoming turn
        prewarmedHPage_ = warmTarget;
      }
    }
  }

  // Never gated on `skimming`. The saved record is not write-only -- render()'s progress-sync path
  // restores position from it, so leaving it stale through a skim lets a sync snap the reader back
  // to the last page actually written.
  //
  // It IS gated on the position having changed: writeAtomic() is several FAT ops for 6 bytes, and
  // render() also runs for menu, bookmark and screenshot re-renders, which move nothing. Every
  // real page turn changes `page`, so durability is unaffected.
  //
  // Horizontal reports estimatedTotalPages(), not pageCount: during a partial build the estimate
  // is the meaningful total, and it moving is itself worth persisting. Compare against the same
  // value that gets stored, or the guard never settles.
  // Gated on footnoteDepth == 0: currentSpineIndex points at the footnote/endnote target
  // while footnoteDepth > 0 (see navigateToHref), not the book position the reader should
  // resume at. onExit() already special-cases this by saving the pre-footnote origin instead
  // -- but this per-render autosave runs on every page turn, including turns taken while
  // reading a multi-page endnote, and would otherwise overwrite that origin with the
  // in-footnote position before onExit ever runs. If the device then crashes or loses power
  // while still inside the footnote, the saved resume point is left pointing into the notes
  // file instead of the book.
  const int page = vertical ? verticalSection->currentPage : section->currentPage;
  const int pageCount = vertical ? verticalSection->pageCount : section->estimatedTotalPages();
  if (footnoteDepth == 0 &&
      (currentSpineIndex != lastSavedSpineIndex || page != lastSavedPage || pageCount != lastSavedPageCount)) {
    if (saveProgress(currentSpineIndex, page, pageCount, verticalOverride, furiganaOverride)) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = page;
      lastSavedPageCount = pageCount;
    }
  }
}

bool EpubReaderActivity::nextTurnAlreadyRequested() const {
  // Reading this task's own notification VALUE without side effects: ulTaskNotifyValueClear with
  // zero bits to clear is a pure read (xTaskNotifyAndQuery is NOT -- even with eNoAction it
  // stamps the notification state). Same signal imageWarmShouldCancel() uses, minus its input
  // stamp, which is only meaningful once warmNextPageImageCache() has taken its snapshot.
  return mappedInput.anyButtonDownRaw() || ulTaskNotifyValueClear(nullptr, 0) > 0;
}

bool EpubReaderActivity::imageWarmShouldCancel(const void* ctx) {
  const auto* self = static_cast<const EpubReaderActivity*>(ctx);
  if (self->mappedInput.anyButtonDownRaw()) return true;
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
    return;  // another render is already queued -- stay out of its way
  }
  if (ESP.getMaxAllocHeap() < IMAGE_WARM_MIN_ALLOC) {
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
      if (nextV->loadSectionFile(fontId, viewportWidth, viewportHeight, SETTINGS.lineSpacing, useFurigana()) &&
          nextV->pageCount > 0) {
        vp = nextV->getPage(0);
      } else {
        // Kept: this line means the next chapter has no section file, i.e. the silent index did
        // not build it and the reader is one turn from a multi-second foreground build.
        LOG_DBG("IWARM", "boundary peek failed: spine %d section not loadable", currentSpineIndex + 1);
      }
    }
    // Warm the image on the next page and, if that one is already cached, keep looking ahead
    // within this chapter. Building a 464x717 cache takes ~4.4s on device, so meeting an
    // illustration with a cold cache stalls the turn; spending otherwise idle time on the ones
    // further ahead makes every later image page open immediately. Cheap to repeat: warmCache()
    // returns AlreadyWarm after a 4-byte header read once the cache exists.
    constexpr int IMAGE_WARM_LOOKAHEAD_PAGES = 8;
    const auto warmVerticalPage = [&](const VerticalPage& page) -> bool {
      if (!page.isImagePage()) return true;  // keep scanning
      if (page.imageRotated) {
        const int reserve = readerBottomReserve(/*verticalMode=*/false);
        ImageBlock block(page.imagePath, page.imageSrcPath, page.imageWidth, page.imageHeight);
        block.setRotated(true, static_cast<int16_t>(reserve));
        return warmBlock(block);
      }
      // Same fit the render path computes -- shared helper keeps the cache dims identical.
      int iw = page.imageWidth;
      int ih = page.imageHeight;
      ImageBlock::fitWithin(viewportWidth, viewportHeight, iw, ih);
      return warmBlock(
          ImageBlock(page.imagePath, page.imageSrcPath, static_cast<int16_t>(iw), static_cast<int16_t>(ih)));
    };

    if (vp && !warmVerticalPage(*vp)) return;  // cancelled: the reader wants the render task back

    for (int ahead = 2; ahead <= IMAGE_WARM_LOOKAHEAD_PAGES; ahead++) {
      const int page = verticalSection->currentPage + ahead;
      if (page >= verticalSection->pageCount) break;
      // Re-check the heap per page: getPage() may pull a page in from the section file.
      if (ESP.getMaxAllocHeap() < IMAGE_WARM_MIN_ALLOC || imageWarmShouldCancel(this)) return;
      const VerticalPage* aheadPage = verticalSection->getPage(page);
      if (!aheadPage) break;
      if (!warmVerticalPage(*aheadPage)) return;
    }
    return;
  }

  if (!section || section->pageCount == 0) {
    return;
  }
  constexpr int IMAGE_WARM_LOOKAHEAD_PAGES = 8;
  const auto warmHorizontalPage = [&](const Page& page) -> bool {
    for (const auto& el : page.elements) {
      if (el->getTag() == TAG_PageImage && !warmBlock(static_cast<const PageImage&>(*el).getImageBlock())) {
        return false;
      }
    }
    return true;
  };

  const bool forward = lastTurnForward_.load(std::memory_order_relaxed);
  const int direction = forward ? 1 : -1;
  int warmedAhead = 0;
  for (int pageIndex = section->currentPage + direction;
       pageIndex >= 0 && pageIndex < section->pageCount && warmedAhead < IMAGE_WARM_LOOKAHEAD_PAGES;
       pageIndex += direction, warmedAhead++) {
    if (ESP.getMaxAllocHeap() < IMAGE_WARM_MIN_ALLOC || imageWarmShouldCancel(this)) return;
    auto page = section->loadPageAt(pageIndex);
    if (!page || !warmHorizontalPage(*page)) return;
  }

  // Continue the same short lookahead across the chapter boundary in the
  // reader's actual turn direction.
  const int adjacentSpine = currentSpineIndex + direction;
  if (warmedAhead < IMAGE_WARM_LOOKAHEAD_PAGES && adjacentSpine >= 0 && adjacentSpine < epub->getSpineItemsCount()) {
    Section adjacentSection(epub, adjacentSpine, renderer);
    if (adjacentSection.loadSectionFile(readerSpec(viewportWidth, viewportHeight)) && adjacentSection.pageCount > 0) {
      for (int pageIndex = forward ? 0 : adjacentSection.pageCount - 1;
           pageIndex >= 0 && pageIndex < adjacentSection.pageCount && warmedAhead < IMAGE_WARM_LOOKAHEAD_PAGES;
           pageIndex += direction, warmedAhead++) {
        if (ESP.getMaxAllocHeap() < IMAGE_WARM_MIN_ALLOC || imageWarmShouldCancel(this)) return;
        auto page = adjacentSection.loadPageAt(pageIndex);
        if (!page || !warmHorizontalPage(*page)) return;
      }
    }
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount, int8_t vertOverride,
                                      int8_t furiOverride) {
  const uint8_t percent = static_cast<uint8_t>(pageBasedPercent(spineIndex, currentPage + 1));
  std::optional<uint32_t> offset;
  if (verticalSection && spineIndex == currentSpineIndex && currentPage >= 0 &&
      currentPage < verticalSection->pageCount) {
    // Vertical records the same content offset horizontal does, so progress saved while
    // reading vertically resolves exactly when the book is next opened horizontally (and the
    // other way round). Without this the two modes could only meet through page proportions.
    offset = verticalSection->getVisibleTextOffsetForPage(currentPage);
  } else if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    // The on-screen page's offset was captured at load; reuse it to avoid a fresh section-file
    // open on every page turn. Any other page (rare) falls back to a direct lookup.
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, vertOverride, furiOverride, percent,
                                       offset);
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (verticalSection && verticalSection->currentPage >= 0 &&
      verticalSection->currentPage < verticalSection->pageCount) {
    cachedVisibleTextOffset = verticalSection->getVisibleTextOffsetForPage(verticalSection->currentPage);
  } else if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft, const bool glyphsAlreadyWarm,
                                        const bool grayscaleRefineOnly) {
  const auto t0 = millis();
  // Reuse the image-warm input generation, which is bumped on every input/page turn.
  const uint32_t inputStamp = imageWarmInputStamp_.load(std::memory_order_relaxed);
  const int fontId = effectiveReaderFontId();

  // The image pixel-cache RAM slot lives for exactly one page render (it feeds
  // the BW double-refresh and every grayscale band pass); release it on every
  // exit so nothing stays resident across page turns.
  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  // Font prewarm: scan pass accumulates text, then prewarm, then real render. Skipped when the
  // idle next-page warm already loaded this page's glyphs (prewarmedHPage_) -- the cache is
  // warm, so we go straight to the real render instead of paying the scan + SD bulk load again
  // at button time. The scope must stay alive for the WHOLE function so its warm survives the
  // real render below and clears only at function exit (via the optional's destructor);
  // scoping it to just the prewarm block would clear the cache before the real render and
  // defeat the prewarm. In the already-warm case no scope is created, so the idle warm's cache
  // persists (the next idle warm clears and rebuilds it).
  auto* fcm = renderer.getFontCacheManager();
  std::optional<FontCacheManager::PrewarmScope> prewarm;
  if (!grayscaleRefineOnly && !glyphsAlreadyWarm && fcm) {
    prewarm.emplace(fcm->createPrewarmScope());
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, !useFurigana());  // scan pass
    // Scan the status bar too: a CJK book/chapter title redirected to the SD
    // fallback font joins the page's single batch prewarm instead of triggering
    // its own SD pass after the scope ends.
    renderStatusBar();
    prewarm->endScanAndPrewarm();
  }
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  shownPageHasImages_ = pageHasImages;
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = !grayscaleRefineOnly && forcedRefreshPending;
  if (!grayscaleRefineOnly) forcedRefreshPending = false;
  // The reader starts with zero here, which means the normal refresh cycle
  // would use a HALF refresh for its first page. Keep that same clean base for
  // image pages: a FAST refresh otherwise runs directly over the
  // retained frame after a silent restart (for example, when returning from
  // KOReader sync), leaving the old UI mixed with the image.
  const bool cleanImageBasePending = !grayscaleRefineOnly && (manualRefreshPending || pagesUntilFullRefresh == 0);
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase = tiledGrayscale && !pageHasImages && renderer.combinesGrayscaleBase();
  // Whole-plane buffering only pays when the BW refresh genuinely runs async
  // underneath it; on blocking panels (X3) it would just spend ~50 KB for the
  // identical serial timing. Image pages take the blocking double-FAST path
  // below (no async refresh is ever started), so they'd spend the buffers with
  // nothing in flight to overlap.
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, !useFurigana());
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  if (!grayscaleRefineOnly && pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  auto tBwRender = tPrewarm;
  auto tDisplay = tBwRender;
  if (!grayscaleRefineOnly) {
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, !useFurigana());
    renderStatusBar();
    tBwRender = millis();
  }

  if (!grayscaleRefineOnly && pageHasImages) {
    // Put the final image on the panel in one pass. The old selective-blank
    // double refresh showed a white frame and delayed the picture; grayscale
    // now refines separately after the page stays idle.
    renderer.displayBuffer(cleanImageBasePending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else if (!grayscaleRefineOnly && combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
  } else if (!grayscaleRefineOnly) {
    // Async form: start the waveform and return so the grayscale plane rendering
    // below overlaps the panel's refresh time instead of following it.
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  tDisplay = millis();

  if (!grayscaleRefineOnly && pageHasImages) {
    const uint32_t currentKey =
        (static_cast<uint32_t>(currentSpineIndex) << 16) | static_cast<uint16_t>(section->currentPage);
    pendingHorizontalImageRefine_.store(currentKey, std::memory_order_relaxed);
    LOG_DBG("ERS", "Page render (image BW): prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
            tBwRender - tPrewarm, tDisplay - tBwRender, tDisplay - t0);
    return;
  }

  if (grayscaleRefineOnly) {
    imageWarmStampSnapshot_ = imageWarmInputStamp_.load(std::memory_order_relaxed);
  }

  // Tiled grayscale: render each plane band-by-band, leaving the BW
  // framebuffer intact so no full-frame storeBwBuffer is needed; controller
  // RAM is re-synced from the live framebuffer afterward. The page is
  // re-rendered ceil(H/STRIP_ROWS) times per plane, but renderCharImpl culls
  // out-of-band glyphs before decode so the cost stays close to one render.
  // Both text (drawPixel) and images (DirectPixelWriter) honor the active
  // strip target. When the BW refresh above went out async, the plane
  // rendering below overlaps the panel's refresh time; only the controller
  // RAM writes wait for BUSY.
  if (tiledGrayscale) {
    // Image rendering re-reads the whole row-major pixel cache for every
    // physical strip. Match the vertical image path's 160-row fast tier,
    // falling back to 80 rows below if the larger scratch does not fit.
    int stripRows = pageHasImages ? 160 : 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;
    const auto shouldCancel = [&] {
      return pageHasImages ? imageWarmShouldCancel(this)
                           : imageWarmInputStamp_.load(std::memory_order_relaxed) != inputStamp;
    };

    // Render one plane band-by-band into a whole-plane buffer without touching
    // the controller, so it can run while the refresh is still in flight.
    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += stripRows) {
        if (shouldCancel()) return false;
        const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
      return true;
    };

    // Tiered on heap pressure: two plane buffers hide both plane renders
    // inside the refresh wait; one hides the LSB render (its buffer is reused
    // for MSB after streaming); none falls back to the strip-scratch flow with
    // no overlap. Each buffer is only attempted when it leaves ~60 KB free so
    // the pass never starves concurrent allocations: the next page re-render
    // allocates through throwing std::string paths that abort() on OOM under
    // -fno-exceptions, so a plane buffer that "fits" but eats the render
    // headroom is worse than the strip fallback. Blocking panels skip the
    // buffers entirely (nothing to overlap).
    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    // Free-heap alone ignores fragmentation: taking the largest block for a
    // plane can leave only slivers behind even when total headroom looks fine.
    // Require the block to fit the plane with 16 KB contiguous to spare, which
    // also keeps the advance-table batch scratch viable mid-render (same
    // rationale as BACKGROUND_BUILD_MIN_MAX_ALLOC).
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      bool cancelled = !renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (!cancelled && msbPlaneBuf) cancelled = !renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      if (!cancelled) cancelled = shouldCancel();
      if (!cancelled) {
        renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
        if (msbPlaneBuf) {
          cancelled = shouldCancel();
        } else {
          cancelled = !renderPlaneToBuffer(false, lsbPlaneBuf.get());
        }
        if (!cancelled) {
          renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf ? msbPlaneBuf.get() : lsbPlaneBuf.get(), 0, gh);
        }
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      if (!cancelled && !shouldCancel()) {
        renderer.displayGrayBuffer();
      } else {
        cancelled = true;
      }
      const auto tGrayDisplay = millis();

      // BW framebuffer is intact; re-sync controller RAM for the next
      // differential page turn directly from it.
      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums "
              "(planes buffered: %d) cancelled=%d",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1,
              cancelled);
    } else {
      // Per-strip scratch tier: blocking panels (X3) and the OOM fallback.
      // The strip writes below need the panel idle, so wait out any pending
      // async refresh first (no-op on blocking panels).
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * stripRows);
      if (!scratch && pageHasImages) {
        stripRows = 80;
        scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * stripRows);
      }
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * stripRows);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        // Bands may be streamed in any order: X4 windows each via setRamArea,
        // X3 via PTL.
        bool cancelled = false;
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh && !cancelled; y += stripRows) {
          if (shouldCancel()) {
            cancelled = true;
            break;
          }
          const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        // MSB plane.
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh && !cancelled; y += stripRows) {
          if (shouldCancel()) {
            cancelled = true;
            break;
          }
          const int rows = (gh - y < stripRows) ? (gh - y) : stripRows;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        if (!cancelled) renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        // BW framebuffer is intact; re-sync controller RAM for the next
        // differential page turn directly from it.
        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums cancelled=%d",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0, cancelled);
      }
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
  // The probe loop below opens one section cache file per spine. While a chapter is still
  // building, every one of those opens queues behind the build's own SD traffic on the shared
  // storage mutex, so anything that calls this -- opening the reader menu, drawing the status
  // bar -- stalls for as long as the build holds the card (reported on device: buttons feel
  // unresponsive during indexing).
  //
  // Only the probes are expensive, so only they are skipped. Bailing out of the whole function
  // used to leave the numbers at the per-section fallback for as long as the build ran, which
  // after a mode switch is the entire rebuild: the status bar showed "203/203" where the other
  // mode showed "228/258". The live chapter's own count costs nothing, and the byte-share
  // estimate for the rest is arithmetic, so the chapter-wide numbers are available immediately.
  const bool buildActive =
      verticalBuildInProgress_.load(std::memory_order_relaxed) || (section && section->isBuilding());

  // Memo key: the raw watermark, which advances as the build lays pages out.
  const int livePages = verticalSection ? verticalSection->pageCount : (section ? section->pageCount : 0);
  // Model input: the chapter's estimated total, the same number sectionPageSpan() shows. The
  // watermark makes a half-built chapter look only as long as the pages laid out so far, which
  // shrinks the X/Y span and, through spinePagesReal, the whole-book percentage with it. Vertical
  // builds in one pass, so its pageCount is already the total.
  const int liveTotalPages = verticalSection ? verticalSection->pageCount
                             : section       ? section->estimatedTotalPages()
                                             : 0;
  const bool vertical = useVerticalText();
  if (chapterSpanSpine == currentSpineIndex && chapterSpanLivePages == livePages && chapterSpanVertical == vertical &&
      chapterSpanBuildActive == buildActive) {
    return;
  }
  const bool spanModeChanged = chapterSpanVertical != vertical;
  chapterSpanSpine = currentSpineIndex;
  chapterSpanLivePages = livePages;
  chapterSpanVertical = vertical;
  chapterSpanBuildActive = buildActive;
  chapterPagesBefore = 0;
  chapterPagesTotal = std::max(1, liveTotalPages);
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
    if (i == currentSpineIndex && liveTotalPages > 0) {
      spinePagesReal[i] = static_cast<uint16_t>(std::min(liveTotalPages, 0xFFFF));
    } else if (spinePagesReal[i] == 0 && !buildActive) {
      // A failed probe is remembered for the session (kSpineProbeFailed): without this, every
      // menu open / status-bar refresh after a cache-version bump re-opened (and re-discarded)
      // all N stale section files -- a visible seconds-long delay on a 39-spine book.
      // Skipped entirely while a build holds the card; those spines fall to the byte estimate
      // and are probed for real once the build finishes.
      bool probed = false;
      if (vertical) {
        VerticalSection sibling(epub, i, renderer);
        if (sibling.loadSectionFile(fontId, viewportWidth, viewportHeight, SETTINGS.lineSpacing, useFurigana())) {
          spinePagesReal[i] = sibling.pageCount;
          probed = true;
        }
      } else {
        Section sibling(epub, i, renderer);
        if (sibling.loadSectionFile(readerSpec(viewportWidth, viewportHeight))) {
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

  // Nothing real to build a model from. A mode switch drops every cached count AND the live
  // section, and no cache probe for the new mode can succeed until its first chapter is built,
  // so for that window every spine would fall back to the one-page placeholder below. That is
  // what produced "~228/228 7600%": book progress is (pages before + page) / total, and total
  // was 3 for a 3-spine book. Leave the model empty instead, so callers fall back to byte
  // weighting and per-section numbering until real counts arrive.
  if (knownPages == 0) {
    LOG_DBG("ERS", "No real page counts yet (spine %d, %s), using byte-weighted progress", currentSpineIndex,
            vertical ? "vertical" : "horizontal");
    return;
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

EpubReaderActivity::SectionPageSpan EpubReaderActivity::sectionPageSpan() const {
  const int rawPage = verticalSection ? verticalSection->currentPage : section ? section->currentPage : 0;
  // estimatedTotalPages() rather than pageCount: while a giant spine builds, pageCount is only
  // the watermark reached so far, so "page X of Y" would count against a total that keeps moving.
  const int rawCount = verticalSection ? verticalSection->pageCount : section ? section->estimatedTotalPages() : 0;
  if (rawCount <= 0) {
    // No pages laid out yet -- a chapter mid-rebuild, typically right after a vertical/horizontal
    // switch. Publishing {1, 1} here is what made the status bar read "1/1 0%" for a reader who
    // was 95% through the chapter a moment earlier. Hold the position being carried into the
    // rebuild instead; the real span replaces it as soon as the section has pages.
    if (cachedChapterTotalPageCount > 0 && currentSpineIndex == cachedSpineIndex) {
      return {std::clamp(nextPageNumber + 1, 1, cachedChapterTotalPageCount), cachedChapterTotalPageCount};
    }
    return {1, 1};  // empty chapter: one skippable page beats 65536/0
  }

  // Loud rather than silently clamped. An out-of-range page here is a real bug somewhere in the
  // positioning path, and the clamp below is what hides it from the screen.
  if (rawPage < 0 || rawPage >= rawCount) {
    LOG_ERR("ERS", "section page out of range: %d of %d (spine %d, %s)", rawPage, rawCount, currentSpineIndex,
            verticalSection ? "vertical" : "horizontal");
  }
  return {std::clamp(rawPage + 1, 1, rawCount), rawCount};
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
  // Bulk-load every glyph this page needs before drawing any of them. Otherwise each uncached
  // codepoint falls through to the on-demand path -- for an SD font, a file open plus two seeks and
  // two reads per miss into an 8-slot ring -- which for a page of hundreds of distinct kanji means
  // hundreds of SD round-trips, and 1.5-2s of render time.
  //
  // Three constraints, each one a page-blanking bug if broken:
  //
  //   clearCache() first is REQUIRED. FontDecompressor::prewarmCache() claims one of only
  //   MAX_PAGE_SLOTS (4) page-buffer slots per call and never self-evicts ("the caller must call
  //   freePageBuffer/clearCache to reset", FontDecompressor.h). Without it every page turn claims
  //   another slot until all 4 are stuck and no glyph resolves.
  //
  //   styleMask must list only the styles PRESENT on this page. FontCacheManager::prewarmCache()
  //   claims a slot per requested style plus one per style for the family's fallback font -- a
  //   blanket "all 4" asks for up to 8 slots against the 4 that exist.
  //
  //   The heap floor differs by caller. The page-text string and slot claims are bare allocations,
  //   and this also runs mid-build, where an OOM aborts under -fno-exceptions. Keep 20K there: at
  //   14K the prewarm's footprint coincided with a dense ruby page's layout dip and the build
  //   dropped glyphs (needed 11736, 9716 free), staling the cache. Rendering a build-phase page
  //   on-demand costs ~2s once per book; content integrity outranks that. Reading uses the lower
  //   floor -- there is no layout left to starve, and the fallback is worse for the heap as well
  //   as the clock (40-74 on-demand loads per page at ~7ms each, against maxAlloc that sits at
  //   12-32K, so a 20K floor rejects every prewarm).
  constexpr uint32_t PREWARM_MIN_ALLOC_BUILD = 20 * 1024;
  const uint32_t floorBytes = duringEarlyBuildRender_ ? PREWARM_MIN_ALLOC_BUILD : PREWARM_MIN_ALLOC_READ;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm || ESP.getMaxAllocHeap() < floorBytes) return false;
  // The mini-font cache holds one page per style and clearCache() below empties it. Retract the
  // claim BEFORE destroying what backs it: a prewarmedVPage_ still naming a page this call evicts
  // makes the next render trust a cache that no longer holds it and pay a full on-demand page.
  // Callers that know which page they warmed re-assert it on success.
  prewarmedVPage_ = -1;
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
  // Same origin derivation as render(): vertical text only needs the top-left corner, but
  // getOrientedViewableTRBL fills all four edges -- right/bottom are intentionally unused here.
  int marginTop, marginLeft;
  [[maybe_unused]] int marginRight, marginBottom;
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

void EpubReaderActivity::buildNoticeThunk(void* ctx) {
  auto* self = static_cast<EpubReaderActivity*>(ctx);
  // The framebuffer is lent away for the duration of the chapter extraction; drawing then would
  // scribble on the bytes the inflate is using. Skipping simply drops the notice -- the press it
  // answers is already lost either way.
  if (!self->renderer.hasFrameBuffer()) return;
  GUI.drawPopup(self->renderer, tr(STR_INDEXING));
  // The page render that follows repaints the whole screen over this, and a plain FAST leaves
  // the popup's glyphs ghosting underneath; take the absolute pass.
  self->pagesUntilFullRefresh = 1;
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
  duringEarlyBuildRender_ = true;  // the build is paused mid-layout; prewarm keeps its high floor
  renderVerticalPageBody(page);
  duringEarlyBuildRender_ = false;
  // Into the SAME buffer as the page and before the single displayBuffer() below, so it costs no extra
  // refresh. Safe mid-build: renderStatusBar() reads estimatedTotalPages() and marks an estimate with
  // "~". Without it the bar is absent for a vertical chapter's first pages, until the build ends and
  // ordinary renders take over (horizontal has no early-render path).
  renderStatusBar();
  renderer.displayBuffer();
  earlyPageActuallyDisplayed_ = true;
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
  const auto span = sectionPageSpan();
  const int rawPageCount = span.count;
  const int sectionPage = span.page;
  const int sectionPageCount = span.count;

  // Display page numbering spans the ToC chapter, not just this spine file; book progress is
  // page-based (pages read / total pages) with byte weighting only as a bootstrap fallback.
  updateChapterPageSpan(lastViewportWidth, lastViewportHeight);
  const int currentPage = chapterPagesBefore + sectionPage;
  const int pageCount = std::max(chapterPagesTotal, currentPage);

  float bookProgress;
  if (bookPagesTotal > 0) {
    bookProgress = 100.0f * static_cast<float>(bookPagesBefore + sectionPage) / static_cast<float>(bookPagesTotal);
  } else {
    // sectionPage is 1-based, so the fraction is of the pages BEHIND you: page 1 of 10 is 0.0
    // through the chapter, not 0.1. Using it raw also read 100% through the chapter in the one
    // case this branch exists for, a chapter with no pages laid out yet, where the span falls
    // back to {1, 1}. Switching a book to vertical then reported 99% of the book, since spine 1
    // of 3 ends there. The reader menu has always computed it this way.
    const float sectionChapterProg =
        (rawPageCount > 0) ? (static_cast<float>(sectionPage - 1) / sectionPageCount) : 0.0f;
    bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;
  }

  std::string title;

  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  // section is null in vertical mode (verticalSection owns the chapter there), and null again
  // between chapters -- upstream can't hit either case, so its call was unguarded.
  const bool building = section && section->isBuilding();
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    building);
}

ReaderRenderSpec EpubReaderActivity::readerSpec(const uint16_t viewportWidth, const uint16_t viewportHeight) const {
  ReaderRenderSpec spec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);
  spec.fontId = effectiveReaderFontId();
  spec.furiganaEnabled = useFurigana();
  return spec;
}

int EpubReaderActivity::effectiveReaderFontId() const {
  const bool jpBook = isJapaneseBook() || useVerticalText();
  const bool coversPrimary = sdFontSystem.selectedFontCovers(jpBook ? 0x3042 : 'a');
  if (!coversPrimary) {
    // The selected family cannot carry this book's primary script. Substitute a
    // font that can -- which one depends on the direction of the miss:
    //  - Japanese book, selected font has no CJK -> the companion, which
    //    ensureJpFallback() picked as the Noto Serif JP family.
    //  - Latin book, selected font has no Latin (a CJK-only family such as
    //    UDDigiKyokasho) -> the BUILT-IN Noto Serif, not the companion.
    //    The companion is chosen for Japanese, so using it here renders an
    //    English book in a Japanese typeface.
    if (jpBook) {
      const int companion = sdFontSystem.companionFontId();
      if (companion != 0) {
        LOG_DBG("ERS", "Effective font: companion %d (jp book, selected lacks CJK)", companion);
        return companion;
      }
    } else {
      const int builtin = SETTINGS.getBuiltinSerifReaderFontId();
      LOG_DBG("ERS", "Effective font: built-in %d (latin book, selected lacks Latin)", builtin);
      return builtin;
    }
  }
  return SETTINGS.getReaderFontId();
}

bool EpubReaderActivity::repaintVerticalPageForPanelThunk(void* ctx) {
  return static_cast<EpubReaderActivity*>(ctx)->repaintVerticalPageForPanel();
}

bool EpubReaderActivity::repaintVerticalPageForPanel() {
  if (!verticalSection) return false;
  const VerticalPage* page = verticalSection->getPage();
  if (!page) {
    // The slot could not be re-faulted (low heap / read error). Report the failure so the panel
    // retries rather than keeping a cursor on a blank screen.
    LOG_ERR("ERS", "Word lookup: no page to repaint under the word cursor");
    return false;
  }
  renderVerticalPageBody(*page);
  renderStatusBar();
  return true;
}

void EpubReaderActivity::openWordLookupPanel(const bool pageOnScreen) {
  if (!epub || !DictIndex::isAvailable()) return;
  // The scan-result cache path lets a re-open of the same page skip the dictionary scan.
  const std::string scanCachePath = epub->getCachePath() + "/wlscan.bin";
  if (verticalSection) {
    // Page geometry for the panel's word cursor, captured BEFORE the font release below:
    // verticalCellPx probes the reference glyph out of the reader font, and a released SD font
    // answers with the line height instead -- a cell that disagrees with the pixels on screen.
    VerticalSelectContext selectCtx;
    int viewableRight = 0;
    int viewableBottom = 0;
    renderer.getOrientedViewableTRBL(&selectCtx.marginTop, &viewableRight, &viewableBottom, &selectCtx.marginLeft);
    selectCtx.marginTop += SETTINGS.screenMargin;
    selectCtx.marginLeft += SETTINGS.screenMargin;
    selectCtx.cellPx = verticalCellPx(renderer, effectiveReaderFontId());
    selectCtx.repaintPage = &EpubReaderActivity::repaintVerticalPageForPanelThunk;
    selectCtx.repaintCtx = this;
    selectCtx.pageOnScreen = pageOnScreen;

    // Built under the render lock, started after it: the constructor's scan copies every glyph
    // out of *page, and the pointer is only valid while nothing else re-faults the section's
    // single page slot -- which the render task's warm tail does, for seconds at a time. This
    // exact race is what corrupted the glyph vector before the reported heap_caps_free panic
    // (see openReaderMenu()). startActivityForResult() only queues, so it must not be called
    // with the lock held (ActivityManager takes it again to perform the push).
    // Same reclaim as the horizontal branch below, for the same reason: the scan aborts on a
    // failed allocation (pushGlyphSafe -> scanTruncated) and shows whatever it had, which
    // surfaces as INCOMPLETE matches rather than none. Vertical has no incremental build to
    // suspend, but the glyph caches are the bulk of it and reload lazily on the next page.
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->releaseAllFontMemory();
      prewarmedVPage_ = -1;  // the release emptied the mini font cache
      prewarmedHPage_ = -1;
    }
    LOG_DBG("ERS", "Word lookup (vertical): maxAlloc after reclaim = %u", ESP.getMaxAllocHeap());

    std::unique_ptr<Activity> panel;
    {
      RenderLock lock(*this);
      if (const VerticalPage* page = verticalSection->getPage()) {
        panel = makeUniqueNoThrow<EpubReaderWordLookupActivity>(
            renderer, mappedInput, *page, scanCachePath, static_cast<uint16_t>(currentSpineIndex),
            static_cast<uint16_t>(verticalSection->currentPage), selectCtx);
        if (!panel) LOG_ERR("ERS", "OOM: word lookup panel");
      }
    }
    if (panel) {
      startActivityForResult(std::move(panel), [this](const ActivityResult&) { requestUpdate(); });
    }
  } else if (section) {
    // loadPage(), not loadPageAt(): with the incremental build the current page often lives only
    // in the in-progress .part file, and loadPageAt() reads the COMMITTED file -- it returned
    // nullptr and Word Lookup silently did nothing in horizontal mode while vertical (which has
    // no incremental build) worked. loadPage() serves from the active build first.
    auto page = section->loadPage(section->currentPage);
    if (page) {
      // Hand the dictionary a heap it can work in. An incremental build keeps its parser,
      // BuildContext and page LUT resident for as long as the chapter is open -- the one-shot
      // build this replaced freed all of it when it finished -- and the panel arrived with a
      // ~6KB largest block, at which DictIndex refuses EVERY entry it finds ("Skipping entry
      // (43 bytes, maxAlloc=6132)"). Matches were found and could not be loaded, which the UI
      // reported as "no match found". Vertical mode has no incremental build, hence no symptom.
      //
      // Suspend rather than abandon: suspendBuild() persists what was laid out as a partial
      // section file, so the pages already built stay readable and the rebuild resumes from
      // that watermark instead of starting over. The page above is already loaded, so nothing
      // here needs the build to still be live.
      if (section->isBuilding()) {
        RenderLock lock(*this);  // the render task may be inside the build; every other reset here takes it too
        section->suspendBuild();
      }
      if (auto* fcm = renderer.getFontCacheManager()) {
        fcm->releaseAllFontMemory();
        prewarmedVPage_ = -1;  // the release emptied the mini font cache
        prewarmedHPage_ = -1;
      }
      LOG_DBG("ERS", "Word lookup: maxAlloc after reclaim = %u", ESP.getMaxAllocHeap());

      startActivityForResult(std::make_unique<EpubReaderWordLookupActivity>(
                                 renderer, mappedInput, *page, scanCachePath, static_cast<uint16_t>(currentSpineIndex),
                                 static_cast<uint16_t>(section->currentPage)),
                             [this](const ActivityResult&) { requestUpdate(); });
    }
  }
}

void EpubReaderActivity::refreshSectionFootnotesIfBuilt() {
  // Nothing to re-read while the build is still running (the table is written last), and a
  // vertical section never has one. An empty list on a finished section is also the normal
  // "chapter has no notes" case -- the retry costs one small read on menu/panel open.
  if (!section || section->isBuilding() || !sectionFootnotes.empty()) return;
  section->loadSectionFootnotes(sectionFootnotes);
}

void EpubReaderActivity::openFootnotesPanel() {
  refreshSectionFootnotesIfBuilt();
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
    clearDeferredReposition();
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
    clearDeferredReposition();
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

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
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
    pageCount = verticalSection ? verticalSection->pageCount : section->estimatedTotalPages();
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
    // Record the exact content offset so the bookmark lands correctly after any re-pagination.
    // The two layouts own different section objects; never touch `section` while a vertical page
    // is active (it is null in that mode).
    std::optional<uint32_t> offset;
    if (verticalSection && currentPage >= 0 && currentPage < verticalSection->pageCount) {
      offset = verticalSection->getVisibleTextOffsetForPage(currentPage);
    } else if (section && currentPage >= 0 && currentPage < section->pageCount) {
      // currentPageVisibleOffset was captured for this very page at its last render.
      offset = currentPageVisibleOffset.has_value()
                   ? currentPageVisibleOffset
                   : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
    }
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if ((!section && !verticalSection) || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int page = verticalSection ? verticalSection->currentPage : section->currentPage;
  const int pageCount = verticalSection ? verticalSection->pageCount : section->estimatedTotalPages();
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
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
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
                         : section       ? section->estimatedTotalPages()
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
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
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

// Whether Vertical Text / Furigana are meaningful for this book at all -- shared by opening
// Reader Settings (to decide whether to show the two toggles there) and this menu's own
// MenuResult (kept in sync even though nothing here can change them anymore).
//
// verticalOverride is persisted per book in progress.bin, so a book whose metadata isn't
// dc:language=ja but that has vertical forced on reopens in tategaki -- vertical columns, CJK
// breaking, no word spaces. Gating on isJapaneseBook() alone would then hide the only control
// that turns it back off, leaving the book permanently unreadable.
bool EpubReaderActivity::showVerticalToggle() const { return isJapaneseBook() || verticalOverride == 1; }

bool EpubReaderActivity::useVerticalText() const {
  // Vertical (tategaki) is a Japanese typesetting mode: it stacks characters in
  // columns and breaks per-character, so a Latin book laid out this way loses its
  // word spaces and is unreadable. Never apply it to a non-Japanese book, not even
  // with the per-book override forced on -- that override is persisted in
  // progress.bin, so once set it would follow the book forever.
  if (!isJapaneseBook()) return false;
  if (verticalOverride == 0) return false;
  if (verticalOverride == 1) return true;
  return true;  // auto: isJapaneseBook() is true here
}

uint8_t EpubReaderActivity::readerBottomReserve(const bool verticalMode) const {
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // reserves space for automatic page turn indicator when no status bar or progress bar only
  const bool needsPageTurnLane =
      automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight());
  // Everything actually drawn at the bottom of the screen: the status bar, plus a text lane for
  // the page-turn indicator when nothing else is using one.
  const auto drawnHeight = static_cast<uint8_t>(
      statusBarHeight + (needsPageTurnLane ? UITheme::getInstance().getMetrics().statusBarVerticalMargin : 0));
  return verticalMode ? static_cast<uint8_t>(drawnHeight + SETTINGS.screenMargin)
                      : std::max(SETTINGS.screenMargin, drawnHeight);
}

bool EpubReaderActivity::useFurigana() const {
  if (furiganaOverride == 0) return false;
  if (furiganaOverride == 1) return true;
  return true;
}
