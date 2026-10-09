#include "VerticalClipSelectionActivity.h"

#include <Arduino.h>
#include <Epub/blocks/VerticalTextBlock.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>

#include "ClippingStore.h"
#include "MappedInputManager.h"

using Input = WordSelectionInput;

VerticalClipSelectionActivity::VerticalClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             const VerticalPage& page, const int fontId,
                                                             const bool furigana, const int marginLeft,
                                                             const int marginTop, const int initialX,
                                                             const int initialY)
    : Activity("VerticalClipSelection", renderer, mappedInput),
      page(page),
      fontId(fontId),
      furigana(furigana),
      marginLeft(marginLeft),
      marginTop(marginTop),
      initialX(initialX),
      initialY(initialY) {}

void VerticalClipSelectionActivity::onEnter() {
  Activity::onEnter();
  if (!extractCells() || cellCount == 0) {
    if (cellCount == 0) LOG_ERR("CLIP", "No selectable cells on the vertical page");
    cancel();
    return;
  }
  // Start in the middle of the page, so any cell is at most half a page of presses away.
  selected = static_cast<int>(cellCount / 2);
  if (initialX >= 0) {
    const int hit = cellAt(initialX, initialY);
    if (hit < 0) {
      cancel();
      return;
    }
    selected = rangeStart = hit;
    ignoreInitialTouch = true;
  }
  requestUpdate();
}

bool VerticalClipSelectionActivity::extractCells() {
  cellCount = 0;
  cells = makeUniqueNoThrow<VerticalClipCell[]>(MAX_CELLS);
  if (!cells) {
    LOG_ERR("CLIP", "OOM: selection cells (%u bytes)", static_cast<unsigned>(MAX_CELLS * sizeof(VerticalClipCell)));
    return false;
  }
  forEachVerticalClipCell(renderer, page, fontId, marginLeft, marginTop, this,
                          [](void* ctx, const VerticalClipCell& cell) {
                            auto* self = static_cast<VerticalClipSelectionActivity*>(ctx);
                            if (self->cellCount == MAX_CELLS) {
                              LOG_ERR("CLIP", "Selectable cell cap hit (%u); page truncated",
                                      static_cast<unsigned>(MAX_CELLS));
                              return false;
                            }
                            self->cells[self->cellCount++] = cell;
                            return true;
                          });
  return true;
}

int VerticalClipSelectionActivity::cellAt(const int x, const int y) const {
  for (size_t i = 0; i < cellCount; i++) {
    const VerticalClipCell& c = cells[i];
    if (x >= c.x && x < c.x + c.width && y >= c.y && y < c.y + c.height) return static_cast<int>(i);
  }
  // A fingertip beside a column: the nearest cell within half a cell of the touch.
  int best = -1;
  int bestDistance = 0;
  for (size_t i = 0; i < cellCount; i++) {
    const VerticalClipCell& c = cells[i];
    const int dx = std::max({c.x - x, 0, x - (c.x + c.width)});
    const int dy = std::max({c.y - y, 0, y - (c.y + c.height)});
    const int distance = dx + dy;
    if (distance <= c.width / 2 && (best < 0 || distance < bestDistance)) {
      best = static_cast<int>(i);
      bestDistance = distance;
    }
  }
  return best;
}

void VerticalClipSelectionActivity::moveCell(const int direction) {
  const int next = std::clamp(selected + direction, 0, static_cast<int>(cellCount) - 1);
  if (next == selected) return;
  selected = next;
  requestUpdate();
}

void VerticalClipSelectionActivity::jumpColumn(const int direction) {
  // Columns count from the right, so +1 is the next column in reading order. Land on the cell of
  // the same row when the column has one, else its nearest.
  const VerticalClipCell& current = cells[selected];
  const int targetColumn = static_cast<int>(current.column) + direction;
  int best = -1;
  int bestDistance = 0;
  for (size_t i = 0; i < cellCount; i++) {
    const VerticalClipCell& c = cells[i];
    if (static_cast<int>(c.column) != targetColumn) continue;
    const int distance = std::abs(static_cast<int>(c.row) - static_cast<int>(current.row));
    if (best < 0 || distance < bestDistance) {
      best = static_cast<int>(i);
      bestDistance = distance;
    }
  }
  if (best < 0) {
    // No such column on this page: the last cell forward, the first one backward.
    best = direction > 0 ? static_cast<int>(cellCount) - 1 : 0;
  }
  if (best == selected) return;
  selected = best;
  requestUpdate();
}

void VerticalClipSelectionActivity::openActions() {
  static constexpr StrId OPTIONS[] = {StrId::STR_CLIP, StrId::STR_BOOKMARK_OPTION};
  actionPopup.show(StrId::STR_CLIPPINGS, OPTIONS, 2, 0, [this](const int index) {
    confirmSelection(index == 0 ? ClippingResult::Action::Clip : ClippingResult::Action::Bookmark);
  });
  requestUpdate();
}

bool VerticalClipSelectionActivity::buildSelectedText(const int first, const int last, std::string& text) const {
  text.clear();
  text.reserve(std::min<size_t>(CLIPPING_TEXT_MAX, static_cast<size_t>(last - first + 1) * 3 + 16));
  for (int i = first; i <= last; i++) {
    const VerticalClipCell& cell = cells[i];
    const VerticalGlyph& g = page.glyphs[cell.glyphIndex];
    if (i > first && cell.paragraphStart) text.push_back('\n');
    if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
      text += page.glyphTextStr(g);
    } else {
      utf8AppendCodepoint(g.codepoint, text);
    }
    if (text.size() > CLIPPING_TEXT_MAX) return false;
  }
  return true;
}

void VerticalClipSelectionActivity::confirmSelection(const ClippingResult::Action action) {
  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  ClippingResult result;
  result.action = action;
  const auto heap = HalMemory::getDefaultHeap();
  const bool lowMemory = heap.largestBlockBytes < CLIPPING_TEXT_MAX + 1 || heap.freeBytes < CLIPPING_TEXT_MAX + 2048;
  if (lowMemory || !buildSelectedText(first, last, result.text)) {
    static constexpr StrId OPTIONS[] = {StrId::STR_BACK};
    actionPopup.show(lowMemory ? StrId::STR_MEMORY_ERROR : StrId::STR_CLIPPING_TOO_LONG, OPTIONS, 1, 0, [](int) {});
    requestUpdate();
    return;
  }
  result.startPageOffset = 0;
  result.endPageOffset = 0;
  result.startWordIndex = static_cast<uint16_t>(first);
  result.endWordIndex = static_cast<uint16_t>(last);
  result.wordCount = static_cast<uint16_t>(last - first + 1);
  result.startOffset = cells[first].startOffset;
  result.endOffset = cells[last].endOffset;
  setResult(std::move(result));
  finish();
}

void VerticalClipSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool VerticalClipSelectionActivity::handleHomeGesture() {
  cancel();
  return true;
}

bool VerticalClipSelectionActivity::handleButtons(const uint8_t buttons) {
  if (actionPopup.handleButtons(buttons & Input::INPUT_PREVIOUS, buttons & Input::INPUT_NEXT,
                                buttons & Input::INPUT_CONFIRM, buttons & Input::INPUT_BACK,
                                [this] { requestUpdate(); }))
    return true;

  if (buttons & Input::INPUT_BACK) {
    if (rangeStart >= 0) {
      rangeStart = -1;
      requestUpdate();
    } else {
      cancel();
    }
    return true;
  }
  if (buttons & Input::INPUT_CONFIRM) {
    if (rangeStart < 0) {
      rangeStart = selected;
      requestUpdate();
    } else {
      openActions();
    }
    return true;
  }
  // Same axes as Word Lookup: step along the column, jump between columns (Left runs forward,
  // with the text). The page buttons step too, so a button-only device can crawl the column.
  if (buttons & (Input::INPUT_DOWN | Input::INPUT_NEXT)) {
    moveCell(1);
  } else if (buttons & (Input::INPUT_UP | Input::INPUT_PREVIOUS)) {
    moveCell(-1);
  } else if (buttons & Input::INPUT_LEFT) {
    jumpColumn(1);
  } else if (buttons & Input::INPUT_RIGHT) {
    jumpColumn(-1);
  }
  return false;
}

void VerticalClipSelectionActivity::loop() {
  if (cellCount == 0) return;
  if (!mappedInput.hasTouch()) {
    const uint8_t buttons = selectionInput.pollButtons(mappedInput, millis());
    if (!buttons) return;
    RenderLock lock;
    handleButtons(buttons);
    return;
  }

  RenderLock lock;
  if (actionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  const uint8_t buttons = Input::buttonEdges(mappedInput);
  if (buttons) {
    handleButtons(buttons);
    return;
  }

  int touchX = 0;
  int touchY = 0;
  if (ignoreInitialTouch) {
    if (!mappedInput.isScreenTouchHeld(touchX, touchY)) ignoreInitialTouch = false;
    return;
  }
  if (!mappedInput.wasScreenTapped(touchX, touchY)) return;
  const int hit = cellAt(touchX, touchY);
  if (hit < 0) {
    // Off the text: a tap elsewhere leaves, like the horizontal selection.
    cancel();
    return;
  }
  if (rangeStart < 0) {
    selected = rangeStart = hit;
  } else if (hit == selected) {
    // Tapping the end of the selection again is the "done" of touch.
    openActions();
    return;
  } else {
    selected = hit;
  }
  requestUpdate();
}

void VerticalClipSelectionActivity::drawPage() {
  renderer.clearScreen();
  VerticalTextBlock block(page);
  if (furigana) {
    block.render(renderer, fontId, fontId, marginLeft, marginTop, true);
  } else {
    block.render(renderer, fontId, marginLeft, marginTop, true);
  }
}

void VerticalClipSelectionActivity::invertCell(const int index) const {
  const VerticalClipCell& c = cells[index];
  renderer.invertRect(c.x, c.y, c.width, c.height);
}

void VerticalClipSelectionActivity::render(RenderLock&&) {
  if (actionPopup.processRender(renderer, mappedInput)) {
    pageDrawn = false;  // the popup painted over the page; the next frame redraws it
    return;
  }
  if (!pageDrawn) {
    drawPage();
    pageDrawn = true;
    drawnFirst = drawnLast = -1;
  }
  const int first = rangeStart < 0 ? selected : std::min(rangeStart, selected);
  const int last = rangeStart < 0 ? selected : std::max(rangeStart, selected);
  // Inversion is its own undo: flip the cells that left the range, then those that joined it.
  for (int i = drawnFirst; i >= 0 && i <= drawnLast; i++) {
    if (i < first || i > last) invertCell(i);
  }
  for (int i = first; i <= last; i++) {
    if (drawnFirst < 0 || i < drawnFirst || i > drawnLast) invertCell(i);
  }
  drawnFirst = first;
  drawnLast = last;
  renderer.displayBuffer();
}
