#include "ReaderActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <MangaPanel.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "MangaReaderActivity.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

bool ReaderActivity::isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

bool ReaderActivity::isMangaFolder(const std::string& path) { return manga::MangaBook::isMangaFolder(path); }

std::unique_ptr<manga::MangaBook> ReaderActivity::loadManga(const std::string& path) {
  auto book = makeUniqueNoThrow<manga::MangaBook>(path);
  if (!book) {
    LOG_ERR("READER", "Failed to allocate MangaBook");
    return nullptr;
  }
  if (!book->load()) {
    LOG_ERR("READER", "Failed to load manga: %s", path.c_str());
    return nullptr;
  }
  return book;
}

void ReaderActivity::onGoToMangaReader(std::unique_ptr<manga::MangaBook> manga) {
  currentBookPath = manga->getFolder();
  activityManager.replaceActivity(std::make_unique<MangaReaderActivity>(renderer, mappedInput, std::move(manga)));
}

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) const {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  // Coalesce the heap BEFORE Epub::load(): its CSS cache load aborts below 48KB contiguous and
  // a CSS re-parse wants 64KB free. On the resume-from-sleep path the boot screen leaves font
  // caches warm and an X3 arrives here at ~49KB maxAlloc -- just under the threshold -- which
  // cascaded into a per-boot CSS re-parse + section-cache invalidation (reporter log). Fonts
  // reload lazily; the reader re-warms them for the first page render anyway.
  if (ESP.getMaxAllocHeap() < 64 * 1024) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      LOG_INF("READER", "Low heap before book load (maxAlloc=%u); releasing font memory", ESP.getMaxAllocHeap());
      fcm->releaseAllFontMemory();
      LOG_INF("READER", "After font release: maxAlloc=%u", ESP.getMaxAllocHeap());
    }
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  if (epub->load(true, SETTINGS.embeddedStyle == 0)) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(renderer, mappedInput, std::move(epub)));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  sdFontSystem.setJpFallbackNeeded(renderer, false);
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  sdFontSystem.setJpFallbackNeeded(renderer, false);
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  sdFontSystem.ensureLoaded(renderer);

  currentBookPath = initialBookPath;
  if (isMangaFolder(initialBookPath)) {
    // Manga is Japanese content (OCR text overlays, lookup): needs the JP fallback font.
    sdFontSystem.setJpFallbackNeeded(renderer, true);
    auto manga = loadManga(initialBookPath);
    if (!manga) {
      onGoBack();
      return;
    }
    onGoToMangaReader(std::move(manga));
  } else if (isBmpFile(initialBookPath)) {
    onGoToBmpViewer(initialBookPath);
  } else if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}

void ReaderActivity::onGoBack() { finish(); }
