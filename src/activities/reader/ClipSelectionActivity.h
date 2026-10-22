#pragma once

#include <Epub/Page.h>
#include <Epub/VerticalParsedText.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "VerticalClipCells.h"
#include "activities/Activity.h"
#include "clippings/ClippingWordFont.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "util/WordSelectionInput.h"

class ClipSelectionActivity final : public Activity {
 public:
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        std::vector<std::unique_ptr<Page>> pages, int marginLeft, int marginTop, int initialX = -1,
                        int initialY = -1, int fontId = -1);
  // A vertical page: the same selection over character cells (a rotated Latin run is one cell).
  // Rows are columns, so "along the text" is down a column and "across" is between columns.
  // The page is taken by value: the VerticalSection's single page slot is re-faulted by the
  // reader's render task while a child activity runs, so a reference into it goes stale.
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, VerticalPage page, bool furigana,
                        int marginLeft, int marginTop, int fontId, int initialX = -1, int initialY = -1);

  // Vertical pages are repainted by the reader (the same body draw and glyph prewarm it uses for
  // its own page and for the word-lookup panel), so the selection never drifts from the page.
  // The reader's cell size for this page, measured while its font is loaded (see VerticalClipCells).
  void setCellPx(const int px) { cellPx = px; }

  void setRepaintPage(void* ctx, bool (*fn)(void*)) {
    repaintCtx = ctx;
    repaintPage = fn;
  }

  // Hands the selected text range to the reader, whose page repaint paints it under the text like
  // a saved clipping -- the same look as a horizontal selection.
  void setLiveSelection(void* ctx, void (*fn)(void*, uint32_t, uint32_t)) {
    liveSelectionCtx = ctx;
    liveSelection = fn;
  }

  // Opened from a lookup panel's handle with the finger still down: the selection starts as the
  // initial word and follows that finger as a handle drag, from the point it pressed.
  void continueDragFrom(const int touchX, const int touchY) {
    dragFromX = touchX;
    dragFromY = touchY;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleHomeGesture() override;

  // Heap the vertical selection allocates on entry, for the reader's pre-check: one WordBox per
  // cell plus five text bytes each.
  static size_t verticalCellBytes();

 private:
  struct WordBox {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    uint16_t row = 0;
    uint8_t pageOffset = 0;
    ClippingWordFont font;
    uint16_t pageWordIndex = 0;
    uint32_t startOffset = UINT32_MAX;
    uint32_t endOffset = UINT32_MAX;
    const char* text = nullptr;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool paragraphStart = false;
    bool isRtl = false;
    bool discretionaryHyphen = false;
  };

  static constexpr size_t MAX_SELECTABLE_WORDS = 240;
  // A vertical page is one page of character cells (a dense one holds ~450); 512 WordBoxes stay
  // under 24KB on the C3, and the nothrow allocation cancels the selection when even that is short.
  static constexpr size_t MAX_SELECTABLE_CELLS = 512;

  bool extractWords();
  bool extractVerticalCells();
  void prewarmVerticalPage();
  bool vertical() const { return verticalPage != nullptr; }
  size_t pageCount() const { return vertical() ? 1 : pages.size(); }
  void invertWord(const WordBox& word, int offsetX, int offset) const;
  void drawPage(int offsetX, int offsetY);
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  int dragWordAt(int x, int y) const;
  bool selectionContains(int x, int y) const;
  int nextPageStartIndexForTouchDrag() const;
  bool isWithinCurrentPageEndDwellSlop(int x, int y) const;
  void moveVertical(int direction);
  void selectIndex(int index);
  void moveToPage(int pageOffset);
  void confirmSelection(ClippingResult::Action action = ClippingResult::Action::Clip);
  void loopButtons();
  bool handleButtons(uint8_t buttons);
  Rect handleRect(int index, bool start) const;
  Rect actionRect() const;
  int selectionTop() const;
  int textOffset() const;
  int textXOffset() const;
  void cancel();
  bool buildSelectedText(int first, int last, std::string& text) const;
  void drawSelection() const;
  // Mid-drag range on a vertical page: one inverted bar per column, from the first cell's top to
  // the last cell's bottom, so the bar is unbroken whatever the cells' own heights. Updates invert
  // the old spans and then the new ones: the part in both flips twice and stays put.
  void invertColumnSpans(int first, int last, int offsetX, int offset) const;
  bool renderIncremental();
  // Touch: the refresh is fired deferred, so the loop keeps reading the finger while the panel
  // runs its waveform instead of waiting ~500 ms behind the render lock for every drag sample.
  // settleRefresh() waits it out and reseeds the diff baseline before anything draws again.
  void pushRefresh(HalDisplay::RefreshMode mode);
  void settleRefresh();
  bool refreshPending = false;
  void flipWord(int index, int offsetX, int offset, const Rect& safe) const;
  void invertGapAfter(int index, int offsetX, int offset, const Rect& safe) const;
  void invertClipped(int x, int y, int width, int height, const Rect& safe) const;
  void invertSelectionChange(int oldFirst, int oldLast, int newFirst, int newLast, int offsetX, int offset) const;

  std::vector<std::unique_ptr<Page>> pages;
  void* liveSelectionCtx = nullptr;
  void (*liveSelection)(void*, uint32_t, uint32_t) = nullptr;
  void publishLiveSelection() const;
  // A vertical range is redrawn by the reader, clipped to the cells a step changed: their union
  // collects here and flushDirtyCells() repaints it once, before the refresh.
  // A finger drag in progress. The range is then drawn as inverted cells by this activity and only
  // the cells that change are flipped per sample: a page repaint through the reader (vertical) or
  // the handles and action bar would cost a full render per hop on top of the panel's refresh.
  bool dragging() const { return touchDragSelecting && touchDragHasMoved; }
  bool lastRenderedDragging = false;
  bool rangeRepaintedByReader() const {
    return vertical() && rangeStart >= 0 && liveSelection && repaintPage && !dragging();
  }
  void markDirty(const WordBox& word) const;
  void flushDirtyCells();
  mutable int dirtyLeft = 0;
  mutable int dirtyTop = 0;
  mutable int dirtyRight = 0;
  mutable int dirtyBottom = 0;
  void* repaintCtx = nullptr;
  bool (*repaintPage)(void*) = nullptr;
  VerticalPage ownedVerticalPage;
  const VerticalPage* verticalPage = nullptr;  // &ownedVerticalPage on a vertical page
  const bool furigana = false;
  // NUL-terminated UTF-8 of each single-character vertical cell; WordBox::text points in here.
  std::unique_ptr<char[]> verticalTextPool;
  const int marginLeft;
  const int marginTop;
  const int initialX;
  const int initialY;
  OptionPopup actionPopup;
  // ponytail: retain 16 button frames during refresh; increase only if overflow is observed.
  std::array<uint8_t, 16> pendingButtons{};
  size_t pendingButtonCount = 0;
  WordSelectionInput selectionInput;
  int fontId = 0;
  int cellPx = 0;
  int lineHeight = 0;
  std::unique_ptr<WordBox[]> words;
  size_t wordCount = 0;
  int selected = 0;
  int rangeStart = -1;
  uint8_t currentPageOffset = 0;
  uint16_t rowCount = 0;
  bool touchDragSelecting = false;
  bool ignoreInitialTouch = false;
  int dragFromX = -1;
  int dragFromY = -1;
  int dragOffsetX = 0;
  int dragOffsetY = 0;
  bool touchDragHasMoved = false;
  int touchDragStartX = 0;
  int touchDragStartY = 0;
  int touchDragPageEndIndex = -1;
  unsigned long touchDragPageEndHeldSince = 0;
  int lastRenderedSelected = -1;
  int lastRenderedRangeStart = -1;
  int lastRenderedPageOffset = -1;
  int lastRenderedTextOffset = 0;
  int lastRenderedTextXOffset = 0;
};
