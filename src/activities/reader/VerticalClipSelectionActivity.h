#pragma once

#include <Epub/VerticalParsedText.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "VerticalClipCells.h"
#include "activities/Activity.h"
#include "activities/ActivityResult.h"
#include "components/OptionPopup.h"
#include "util/WordSelectionInput.h"

// Text selection on a vertical page for clippings: the counterpart of ClipSelectionActivity for
// pages laid out in columns. The unit is the character cell (a rotated Latin run is one cell).
// `page` is the VerticalSection's loaded page and must outlive this activity: the reader hands
// it over while no build runs, and it does not fault in another page while this one is on top.
class VerticalClipSelectionActivity final : public Activity {
 public:
  VerticalClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const VerticalPage& page,
                                int fontId, bool furigana, int marginLeft, int marginTop, int initialX = -1,
                                int initialY = -1);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleHomeGesture() override;

 private:
  // A dense page holds ~500 cells; 640 x 24 bytes keeps the table under 16KB.
  static constexpr size_t MAX_CELLS = 640;

  bool extractCells();
  int cellAt(int x, int y) const;
  void moveCell(int direction);
  void jumpColumn(int direction);
  bool handleButtons(uint8_t buttons);
  void openActions();
  void confirmSelection(ClippingResult::Action action);
  void cancel();
  bool buildSelectedText(int first, int last, std::string& text) const;
  void drawPage();
  void invertCell(int index) const;

  const VerticalPage& page;
  const int fontId;
  const bool furigana;
  const int marginLeft;
  const int marginTop;
  const int initialX;
  const int initialY;
  OptionPopup actionPopup;
  WordSelectionInput selectionInput;
  std::unique_ptr<VerticalClipCell[]> cells;
  size_t cellCount = 0;
  int selected = 0;
  int rangeStart = -1;
  // What the framebuffer currently shows, so a move only inverts the cells that changed.
  bool pageDrawn = false;
  int drawnFirst = -1;
  int drawnLast = -1;
  bool ignoreInitialTouch = false;
};
