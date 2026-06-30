#pragma once

#include <MangaPanel.h>

#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class MangaChapterSelectionActivity final : public Activity {
 public:
  explicit MangaChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         std::vector<manga::TocEntry> tocEntries, const uint32_t currentPage)
      : Activity("MangaChapterSelection", renderer, mappedInput), tocEntries(std::move(tocEntries)) {
    // Pre-select whichever chapter the current page falls within.
    for (size_t i = 0; i < this->tocEntries.size(); i++) {
      if (this->tocEntries[i].pageIndex <= currentPage) {
        selectorIndex = static_cast<int>(i);
      } else {
        break;
      }
    }
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<manga::TocEntry> tocEntries;
  int selectorIndex = 0;
  ButtonNavigator buttonNavigator;
};
