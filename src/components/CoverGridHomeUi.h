#pragma once

#include <array>
#include <string>
#include <vector>

#include "HomeCoverCache.h"
#include "RecentBooksStore.h"
#include "UiAppHost.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"

class CoverGridHomeUi final : public UiAppHost {
 public:
  static constexpr int THUMB_HEIGHT = 400;
  static constexpr int GRID_COLUMNS = 3;
  static constexpr int GRID_ROWS = 2;
  static constexpr int MAX_BOOKS = 1 + GRID_COLUMNS * GRID_ROWS;
  static_assert(MAX_BOOKS <= HomeCoverCache::MAX_COVERS);
  explicit CoverGridHomeUi(GfxRenderer& renderer);
  void begin(const std::vector<RecentBook>& books, bool hasOpds, bool hasContinueReading);
  void refreshCoverPaths();
  void setSelection(int selection) { selected = selection; }
  int selectedAction(const MappedInputManager& input);
  // Book index long-pressed since the last call, or -1. Long press opens that book's stats,
  // matching the Library grid.
  int takeLongPressedBook();
  // Badge percentage for a book, as the grid drew it (-1 when unknown).
  int progressFor(size_t index) const { return index < bookProgress.size() ? bookProgress[index] : -1; }
  // Re-read one book's progress after something changed it. The cached cell holds the badge that
  // was painted with the old number, so it has to go with it.
  void refreshProgress(size_t index);
  // Exact generation height, shared by every slot and recorded during draw.
  // Thumbs must be generated at the drawn size: rescaling a dithered 1-bit
  // image aliases badly.
  int thumbHeightFor() const;
  // False until the first draw has measured a slot; thumbHeightFor() returns the
  // fallback constant before that, which is not a size any card will ask for.
  bool thumbHeightMeasured() const { return thumbHeight > 0; }
  bool takeThumbHeightChanged();

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawHeaderBand();
  void drawEmpty(UiScreen& screen);
  void drawCurrent(UiScreen& screen, freeink::ui::Rect rect, int coverRowHeight);
  void drawGrid(UiScreen& screen);
  freeink::ui::Rect layoutGrid(freeink::ui::Rect rect);
  void drawTabs();
  bool paintFramedCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, size_t index);
  void refreshCoverPath(size_t index);
  void noteThumbHeight(int slotHeight);

  HomeCoverCache coverCache;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  std::array<std::string, MAX_BOOKS> coverPaths;
  std::array<int, MAX_BOOKS> bookProgress{};
  int thumbHeight = 0;
  bool thumbHeightChanged = false;
  int selected = 0;
  int pending = -1;
  int pendingLongPress = -1;
  int progress = -1;
  bool hasOpds = false;
  bool hasContinueReading = false;
  char progressText[12]{};
  // Component styles and interaction tables stay off the render task's stack.
  freeink::ui::BookCardProps card;
  freeink::ui::CoverGridProps grid;
  freeink::ui::Rect gridBounds{};
  // Where the featured cover was last painted, for the long-press test in selectedAction.
  freeink::ui::Rect heroCoverRect{};
};
