#pragma once
#include <MangaPanel.h>

#include <string>
#include <vector>

#include "../../BookmarkEntry.h"
#include "../Activity.h"
#include "util/ButtonNavigator.h"

// Manga bookmark list. Unlike EpubReaderBookmarksActivity, a manga bookmark's
// position is a plain page index (computedChapterProgress) -- there's no
// spine/xpath to resolve, so "open" just returns that page number directly.
class MangaBookmarksActivity final : public Activity {
  std::string bookPath;
  std::vector<manga::TocEntry> tocEntries;
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  std::vector<BookmarkEntry> bookmarks;
  int confirmingDelete = 0;  // 0 = hide dialog, 1 = show dialog, 2 = allow confirmation to delete

 public:
  explicit MangaBookmarksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                                  std::vector<manga::TocEntry> tocEntries)
      : Activity("MangaBookmarks", renderer, mappedInput),
        bookPath(std::move(bookPath)),
        tocEntries(std::move(tocEntries)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Scrubs the manga page's gray charge on the first paint; see the .cpp.
  bool firstPaint_ = true;
  // Calculate the vertical space to reserve for button hints based on orientation
  int getGutterBottom(const GfxRenderer& renderer);

  // Calculate the height available for the bookmark list based on orientation
  int getListHeight(const GfxRenderer& renderer);
};
