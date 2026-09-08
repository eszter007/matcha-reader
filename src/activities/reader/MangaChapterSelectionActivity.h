#pragma once

#include <MangaPanel.h>

#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/UiListActivity.h"

// UiListActivity, like the EPUB and XTC chapter pickers: it brings the touch
// routing (row taps, swipe scrolling) the hand-rolled button-only list this
// replaced never had, so the chapter list answers a finger on a touch board.
class MangaChapterSelectionActivity final : public UiListActivity {
 public:
  explicit MangaChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::vector<manga::TocEntry> tocEntries, uint32_t currentPage);

  void onEnter() override;

 private:
  std::vector<manga::TocEntry> tocEntries;
  // fui::ListItem points at label storage that must outlive the build, and a
  // manga TOC is a handful of chapters rather than the hundreds an EPUB can
  // carry -- so the whole list is materialised rather than windowed.
  std::vector<freeink::ui::ListItem> items;

  int listCount() const override { return static_cast<int>(tocEntries.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleButtons() override;
  void drawChrome() override;
};
