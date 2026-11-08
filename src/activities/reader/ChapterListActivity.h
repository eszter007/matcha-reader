#pragma once
#include <Epub.h>
#include <MangaPanel.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

// What a chapter list shows and returns. EPUB reads its table of contents from SD row by row;
// page-based books (XTC, manga) pass a small in-memory list.
class ChapterSource {
 public:
  virtual ~ChapterSource() = default;
  virtual int count() const = 0;
  // Row label, indented by nesting level where the book has one.
  virtual std::string label(int index) const = 0;
  // The chapter the reader is in, selected when the list opens.
  virtual int currentIndex() const = 0;
  // Result of picking a row: a ChapterResult (EPUB) or a PageResult (page-based books).
  virtual ActivityResult result(int index) const = 0;
};

class EpubChapterSource final : public ChapterSource {
  std::shared_ptr<Epub> epub;
  int tocIndex;

 public:
  EpubChapterSource(std::shared_ptr<Epub> epub, const int currentTocIndex)
      : epub(std::move(epub)), tocIndex(currentTocIndex) {}
  int count() const override { return epub ? epub->getTocItemsCount() : 0; }
  std::string label(int index) const override;
  int currentIndex() const override { return tocIndex < 0 ? 0 : tocIndex; }
  ActivityResult result(int index) const override;
};

class PageChapterSource final : public ChapterSource {
  std::vector<manga::TocEntry> entries;  // in page order
  uint32_t currentPage;

 public:
  PageChapterSource(std::vector<manga::TocEntry> entries, const uint32_t currentPage)
      : entries(std::move(entries)), currentPage(currentPage) {}
  int count() const override { return static_cast<int>(entries.size()); }
  std::string label(int index) const override;
  int currentIndex() const override;
  ActivityResult result(int index) const override;
};

class ChapterListActivity final : public UiListActivity {
  std::unique_ptr<ChapterSource> source;

  // Windowed row buffers: only the rows around the viewport are materialized. An EPUB TOC is
  // read from SD per row and can run to hundreds of entries (547 in a large collection); built up
  // front it cost ~60KB of labels + ListItems, starving the CJK glyph arena. The window follows
  // nav.top via itemsWindowFirst (see fui::ListProps); refreshing it also batch-prewarms the
  // window's fallback glyphs, so each page of the list pays one bounded SD pass and repaints stay
  // RAM-only.
  static constexpr int TOC_WINDOW = 24;
  std::string windowLabels[TOC_WINDOW];
  freeink::ui::ListItem windowItems[TOC_WINDOW];
  int windowStart = -1;
  int windowCount = 0;
  void refreshTocWindow(int start);

  int listCount() const override { return source ? source->count() : 0; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Back cancels with a result and Confirm activates on RELEASE here.
  bool handleButtons() override;
  // Header is drawn inside the safe area (not full-width like the base).
  void drawChrome() override;

 public:
  explicit ChapterListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               std::unique_ptr<ChapterSource> source);
  void onEnter() override;
};
