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
  void setRepaintPage(void* ctx, bool (*fn)(void*)) {
    repaintCtx = ctx;
    repaintPage = fn;
  }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleHomeGesture() override;

 private:
  struct WordBox {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
    uint16_t row = 0;
    uint8_t pageOffset = 0;
    int8_t characterSpacing = 0;
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

  bool extractWords();
  bool extractVerticalCells();
  void prewarmVerticalPage();
  bool vertical() const { return verticalPage != nullptr; }
  size_t pageCount() const { return vertical() ? 1 : pages.size(); }
  // Vertical cells are highlighted by inversion (their glyphs cannot be redrawn word by word).
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
  bool renderIncremental();
  void drawWordHighlight(int index, int firstSelected, int lastSelected, int offsetX, int offset) const;
  void drawWordClean(int index, int offsetX, int offset) const;
  void ditherGapBetween(const WordBox& a, const WordBox& b, int offsetX, int offset) const;
  void clearGapBetween(const WordBox& a, const WordBox& b, int offsetX, int offset) const;
  void prewarmWord(int index) const;

  std::vector<std::unique_ptr<Page>> pages;
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
  int lineHeight = 0;
  std::unique_ptr<WordBox[]> words;
  size_t wordCount = 0;
  int selected = 0;
  int rangeStart = -1;
  uint8_t currentPageOffset = 0;
  uint16_t rowCount = 0;
  bool touchDragSelecting = false;
  bool ignoreInitialTouch = false;
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
