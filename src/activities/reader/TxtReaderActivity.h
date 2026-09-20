#pragma once

#include <Txt.h>

#include <vector>

#include "CrossPointSettings.h"
#include "ReaderActivity.h"

class TxtReaderActivity final : public ReaderActivity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;  // File offset for start of each page
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

  bool loadBook() override;
  bool hasBook() const override { return txt != nullptr; }
  std::string getBookTitle() const override;
  void onReaderEnter() override;
  void onReaderExit() override;
  void readerLoop() override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("TxtReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  void render(RenderLock&&) override;
  ScreenshotInfo getScreenshotInfo() const override;
};
