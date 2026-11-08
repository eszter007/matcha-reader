#pragma once

#include <MangaPanel.h>

#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/UiListActivity.h"

// Chapter list of a page-based book (XTC, manga). Each entry is a title and the chapter's first
// page; picking one returns that page as a PageResult.
class PageChapterSelectionActivity final : public UiListActivity {
 public:
  explicit PageChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::vector<manga::TocEntry> tocEntries, uint32_t currentPage);

  void onEnter() override;

 private:
  std::vector<manga::TocEntry> tocEntries;
  // fui::ListItem points at label storage that must outlive the build, and a
  // page-based book has a handful of chapters rather than the hundreds an EPUB
  // can carry -- so the whole list is materialised rather than windowed.
  std::vector<freeink::ui::ListItem> items;
  int initialSelected = 0;  // chapter holding the page the reader is on; applied in onEnter()

  int listCount() const override { return static_cast<int>(tocEntries.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void drawChrome() override;
};
