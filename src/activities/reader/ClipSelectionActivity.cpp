#include "ClipSelectionActivity.h"

#include <Arduino.h>
#include <Epub/PageTextExtractor.h>
#include <Epub/blocks/VerticalTextBlock.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <numeric>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/ClippingText.h"
#include "clippings/SelectionGeometry.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "util/WordSelectionInput.h"

using Input = WordSelectionInput;

namespace {

constexpr size_t FONT_PREWARM_TEXT_MAX = 2048;
constexpr int TOUCH_DRAG_MOVEMENT_PX = 4;
constexpr unsigned long TOUCH_PAGE_ADVANCE_HOLD_MS = 1000;
constexpr int TOUCH_PAGE_END_DWELL_SLOP_PX = 8;
// Slot order of the action bar; drawn by BaseTheme::drawSelectionActions in the same order.
constexpr ClippingResult::Action SELECTION_ACTIONS[BaseTheme::SELECTION_ACTION_COUNT] = {
    ClippingResult::Action::Lookup, ClippingResult::Action::Translate, ClippingResult::Action::Clip,
    ClippingResult::Action::Bookmark};

const char* cleanWordStart(const char* text) {
  if (!text) return "";
  if (clippingText::hasEmSpacePrefix(text)) text += 3;
  while (*text != '\0' && (*text == ' ' || *text == '\r' || *text == '\n' || *text == '\t' ||
                           (static_cast<uint8_t>(text[0]) == 0xC2 && static_cast<uint8_t>(text[1]) == 0xA0))) {
    text += static_cast<uint8_t>(text[0]) == 0xC2 ? 2 : 1;
  }
  return text;
}

}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<std::unique_ptr<Page>> pages, const int marginLeft,
                                             const int marginTop, const int initialX, const int initialY,
                                             const int readerFontId)
    : Activity("ClipSelection", renderer, mappedInput),
      pages(std::move(pages)),
      marginLeft(marginLeft),
      marginTop(marginTop),
      initialX(initialX),
      initialY(initialY),
      fontId(readerFontId) {}

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, VerticalPage page,
                                             const bool furigana, const int marginLeft, const int marginTop,
                                             const int readerFontId, const int initialX, const int initialY)
    : Activity("ClipSelection", renderer, mappedInput),
      marginLeft(marginLeft),
      marginTop(marginTop),
      initialX(initialX),
      initialY(initialY),
      ownedVerticalPage(std::move(page)),
      verticalPage(&ownedVerticalPage),
      furigana(furigana),
      fontId(readerFontId) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();
  if (fontId < 0) fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  if (!extractWords() || wordCount == 0) {
    if (wordCount == 0) LOG_ERR("CLIP", "No selectable words on current page");
    cancel();
    return;
  }
  uint16_t firstPageRows = 0;
  for (size_t i = 0; i < wordCount; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != 0) break;
    firstPageRows = std::max<uint16_t>(firstPageRows, static_cast<uint16_t>(word.row + 1));
  }
  const int middle =
      closestInRow(firstPageRows / 2, vertical() ? renderer.getScreenHeight() / 2 : renderer.getScreenWidth() / 2);
  if (middle >= 0) selected = middle;
  if (initialX >= 0) {
    const int hit = wordAt(initialX, initialY);
    if (hit < 0) {
      cancel();
      return;
    }
    selected = rangeStart = hit;
    ignoreInitialTouch = true;
  }
  requestUpdate();
}

bool ClipSelectionActivity::extractWords() {
  if (vertical()) return extractVerticalCells();
  wordCount = 0;
  words = makeUniqueNoThrow<WordBox[]>(MAX_SELECTABLE_WORDS);
  if (!words) {
    LOG_ERR("CLIP", "OOM: selection words (%u bytes)", static_cast<unsigned>(MAX_SELECTABLE_WORDS * sizeof(WordBox)));
    return false;
  }
  rowCount = 0;
  const bool needsFontPrewarm = renderer.isSdCardFont(fontId);
  auto pageText = needsFontPrewarm ? makeUniqueNoThrow<char[]>(FONT_PREWARM_TEXT_MAX) : nullptr;
  size_t pageTextLength = 0;
  if (needsFontPrewarm && !pageText) LOG_DBG("CLIP", "Skipping SD font prewarm: OOM");
  uint8_t styleMask = 0;
  bool paragraphStartPending = false;

  for (size_t pageOffset = 0; pageOffset < pages.size(); ++pageOffset) {
    uint16_t pageWordIndex = 0;
    for (const auto& element : pages[pageOffset]->elements) {
      if (element->getTag() != TAG_PageLine) continue;
      const auto& line = static_cast<const PageLine&>(*element);
      const auto& block = line.getBlock();
      if (!block || !block->valid()) continue;

      const size_t lineStart = wordCount;
      const bool isRtl = block->getBlockStyle().isRtl;
      const size_t remaining = MAX_SELECTABLE_WORDS - lineStart;
      size_t rtlWordCount = 0;
      const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(fontId));
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        if (block->wordStartsParagraph(i)) paragraphStartPending = true;
        const char* text = block->wordText(i);
        if (!clippingText::hasVisibleText(text)) continue;

        const auto style = static_cast<EpdFontFamily::Style>(block->wordStyle(i) & ~EpdFontFamily::UNDERLINE);
        const ClippingWordFont wordFont = clippingWordFont(*block, i, fontId);
        int width = clippingWordWidth(renderer, wordFont, text, style);
        if (width <= 0) continue;
        if (i + 1 < block->wordCount() && block->wordXpos(i + 1) > block->wordXpos(i)) {
          width = std::min(width, static_cast<int>(block->wordXpos(i + 1) - block->wordXpos(i)));
        }

        if (!isRtl && wordCount == MAX_SELECTABLE_WORDS) break;

        WordBox& word = isRtl ? words[lineStart + (rtlWordCount < remaining ? rtlWordCount : rtlWordCount % remaining)]
                              : words[wordCount++];
        word.x = static_cast<int16_t>(marginLeft + line.xPos + block->wordXpos(i));
        word.y = static_cast<int16_t>(marginTop + line.yPos + rubyShift);
        word.width = static_cast<int16_t>(width);
        word.height = static_cast<int16_t>(lineHeight);
        word.row = rowCount;
        word.pageOffset = static_cast<uint8_t>(pageOffset);
        word.pageWordIndex = pageWordIndex++;
        word.startOffset = block->wordSourceRange(i).start;
        word.endOffset = block->wordSourceRange(i).end;
        word.text = text;
        word.style = style;
        word.font = wordFont;
        word.paragraphStart = false;
        word.isRtl = isRtl;
        word.discretionaryHyphen = block->wordHasDiscretionaryHyphen(i);
        if (pageText) {
          for (const char* p = text; *p != '\0' && pageTextLength + 1 < FONT_PREWARM_TEXT_MAX; ++p) {
            pageText[pageTextLength++] = *p;
          }
          if (pageTextLength + 1 < FONT_PREWARM_TEXT_MAX) pageText[pageTextLength++] = ' ';
        }
        styleMask |= static_cast<uint8_t>(1U << (static_cast<uint8_t>(style) & 0x03));
        if (isRtl) ++rtlWordCount;
      }
      if (isRtl) {
        const size_t stored = std::min(remaining, rtlWordCount);
        wordCount = lineStart + stored;
        if (rtlWordCount > remaining) {
          std::rotate(words.get() + lineStart, words.get() + lineStart + rtlWordCount % remaining,
                      words.get() + wordCount);
        }
        std::reverse(words.get() + lineStart, words.get() + wordCount);
      }
      if (wordCount > lineStart && paragraphStartPending) {
        // Whitespace-only tokens are skipped; keep their boundary on the first selectable logical word.
        const auto first =
            std::min_element(words.get() + lineStart, words.get() + wordCount,
                             [](const WordBox& a, const WordBox& b) { return a.startOffset < b.startOffset; });
        first->paragraphStart = true;
        paragraphStartPending = false;
      }
      if (wordCount != lineStart) ++rowCount;
      if (wordCount == MAX_SELECTABLE_WORDS) {
        LOG_ERR("CLIP", "Selectable word cap hit (%u); multi-page selection was truncated",
                static_cast<unsigned>(MAX_SELECTABLE_WORDS));
        break;
      }
    }
    if (wordCount == MAX_SELECTABLE_WORDS) break;
  }

  if (styleMask == 0) styleMask = 0x01;
  if (pageText) {
    pageText[pageTextLength] = '\0';
    renderer.ensureSdCardFontReady(fontId, pageText.get(), styleMask);
  }

  return true;
}

// Bulk-loads the page's glyphs the way the reader does before it draws (prewarmVerticalPageGlyphs);
// otherwise every glyph of the page resolves one at a time through the SD miss path. Runs before
// anything measures text in this font, and again before a redraw. It claims page slots, so it
// needs heap room.
void ClipSelectionActivity::prewarmVerticalPage() {
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm || ESP.getMaxAllocHeap() < 12 * 1024) return;
  fcm->clearCache();
  uint8_t styleMask = std::accumulate(
      verticalPage->glyphs.begin(), verticalPage->glyphs.end(), uint8_t{0},
      [](const uint8_t m, const VerticalGlyph& g) { return static_cast<uint8_t>(m | (1u << (g.style & 0x03))); });
  if (styleMask == 0) styleMask = 1 << EpdFontFamily::REGULAR;
  const std::string pageText = PageTextExtractor::fromVerticalPage(*verticalPage);
  fcm->prewarmCache(fontId, pageText.c_str(), styleMask);
}

bool ClipSelectionActivity::extractVerticalCells() {
  wordCount = 0;
  rowCount = 0;
  prewarmVerticalPage();
  words = makeUniqueNoThrow<WordBox[]>(MAX_SELECTABLE_CELLS);
  // Five bytes per cell is the most a single codepoint and its NUL can take.
  verticalTextPool = makeUniqueNoThrow<char[]>(MAX_SELECTABLE_CELLS * 5);
  if (!words || !verticalTextPool) {
    LOG_ERR("CLIP", "OOM: vertical selection cells");
    return false;
  }
  struct Ctx {
    ClipSelectionActivity* self;
    size_t poolUsed = 0;
  } ctx{this};
  forEachVerticalClipCell(
      renderer, *verticalPage, fontId, marginLeft, marginTop, &ctx, [](void* raw, const VerticalClipCell& cell) {
        auto& ctx = *static_cast<Ctx*>(raw);
        auto& self = *ctx.self;
        if (self.wordCount == MAX_SELECTABLE_CELLS) {
          LOG_ERR("CLIP", "Selectable cell cap hit (%u); page truncated", static_cast<unsigned>(MAX_SELECTABLE_CELLS));
          return false;
        }
        const VerticalGlyph& g = self.verticalPage->glyphs[cell.glyphIndex];
        WordBox& word = self.words[self.wordCount];
        word.x = cell.x;
        word.y = cell.y;
        word.width = cell.width;
        word.height = cell.height;
        word.row = cell.column;
        word.pageOffset = 0;
        word.pageWordIndex = static_cast<uint16_t>(self.wordCount);
        word.startOffset = cell.startOffset;
        word.endOffset = cell.endOffset;
        if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
          word.text = self.verticalPage->glyphText(g);  // the page's text pool outlives this activity
        } else {
          char* out = self.verticalTextPool.get() + ctx.poolUsed;
          const int len = utf8EncodeCodepoint(g.codepoint, out);
          out[len] = '\0';
          ctx.poolUsed += static_cast<size_t>(len) + 1;
          word.text = out;
        }
        word.style = static_cast<EpdFontFamily::Style>(g.style);
        word.font = {self.fontId, TextBlock::WORD_SCALE_ONE, 0};
        word.paragraphStart = cell.paragraphStart;
        word.isRtl = false;
        word.discretionaryHyphen = false;
        self.wordCount++;
        self.rowCount = std::max<uint16_t>(self.rowCount, static_cast<uint16_t>(cell.column + 1));
        return true;
      });
  return true;
}

int ClipSelectionActivity::closestInRow(const uint16_t row, const int center) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].row != row) continue;
    const int distance = vertical() ? std::abs(words[i].y + words[i].height / 2 - center)
                                    : std::abs(words[i].x + words[i].width / 2 - center);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

int ClipSelectionActivity::wordAt(const int x, int y) const {
  y -= textOffset();
  constexpr int SLOP = 4;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (x >= word.x - SLOP && x < word.x + word.width + SLOP && y >= word.y - SLOP && y < word.y + word.height + SLOP) {
      return i;
    }
  }
  return -1;
}

int ClipSelectionActivity::dragWordAt(const int x, int y) const {
  y -= textOffset();
  int best = -1;
  Rect nearest{};
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    // nearerWord ranks by row distance first; a vertical page's rows are columns, so transpose.
    const Rect candidate =
        vertical() ? Rect{word.y, word.x, word.height, word.width} : Rect{word.x, word.y, word.width, word.height};
    if (best < 0 || (vertical() ? selectionGeometry::nearerWord(candidate, nearest, y, x)
                                : selectionGeometry::nearerWord(candidate, nearest, x, y))) {
      nearest = candidate;
      best = i;
    }
  }
  return best;
}

bool ClipSelectionActivity::selectionContains(const int x, const int y) const {
  const int offset = textOffset();
  const WordBox* previous = nullptr;
  for (int i = std::min(rangeStart, selected); i <= std::max(rangeStart, selected); ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (vertical()) {
      int top = word.y;
      int bottom = word.y + word.height;
      if (previous && previous->row == word.row) {
        top = std::min(top, static_cast<int>(previous->y));
        bottom = std::max(bottom, previous->y + previous->height);
      }
      if (selectionGeometry::contains(Rect{word.x, top + offset, word.width, bottom - top}, x, y)) return true;
      previous = &word;
      continue;
    }
    int left = word.x;
    int right = word.x + word.width;
    if (previous && previous->row == word.row) {
      left = std::min(left, static_cast<int>(previous->x));
      right = std::max(right, previous->x + previous->width);
    }
    if (selectionGeometry::contains(Rect{left, word.y + offset, right - left, word.height}, x, y)) return true;
    previous = &word;
  }
  return false;
}

int ClipSelectionActivity::nextPageStartIndexForTouchDrag() const {
  if (!touchDragHasMoved || rangeStart < 0 || selected < rangeStart) return -1;

  const uint8_t currentPage = words[selected].pageOffset;
  for (int i = selected + 1; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset > currentPage) return i;
  }
  return -1;
}

bool ClipSelectionActivity::isWithinCurrentPageEndDwellSlop(const int x, const int y) const {
  if (selected < 0 || selected >= static_cast<int>(wordCount)) return false;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  if (selectionGeometry::atBottomEdge(safe, lineHeight, x, y)) return true;
  if (selected + 1 < static_cast<int>(wordCount) && words[selected + 1].pageOffset == currentPageOffset) return false;
  const WordBox& word = words[selected];
  const int wordY = word.y + textOffset();
  return word.pageOffset == currentPageOffset && x >= word.x - TOUCH_PAGE_END_DWELL_SLOP_PX &&
         x < word.x + word.width + TOUCH_PAGE_END_DWELL_SLOP_PX && y >= wordY - TOUCH_PAGE_END_DWELL_SLOP_PX &&
         y < wordY + word.height + TOUCH_PAGE_END_DWELL_SLOP_PX;
}

void ClipSelectionActivity::moveVertical(const int direction) {
  const int targetRow = static_cast<int>(words[selected].row) + direction;
  if (targetRow < 0 || targetRow >= rowCount) return;
  const WordBox& cursor = words[selected];
  const int next = closestInRow(static_cast<uint16_t>(targetRow),
                                vertical() ? cursor.y + cursor.height / 2 : cursor.x + cursor.width / 2);
  if (next >= 0 && next != selected) {
    selectIndex(next);
  }
}

void ClipSelectionActivity::selectIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(wordCount) || index == selected) return;
  selected = index;
  currentPageOffset = words[selected].pageOffset;
  requestUpdate();
}

void ClipSelectionActivity::moveToPage(const int pageOffset) {
  if (pageOffset < 0 || pageOffset >= static_cast<int>(pageCount()) || pageOffset == currentPageOffset) return;
  for (int i = 0; i < static_cast<int>(wordCount); ++i) {
    if (words[i].pageOffset == pageOffset) {
      selectIndex(i);
      return;
    }
  }
}

bool ClipSelectionActivity::buildSelectedText(const int first, const int last, std::string& text) const {
  text.clear();
  text.reserve(CLIPPING_TEXT_MAX);
  const size_t count = last - first + 1;
  auto order = makeUniqueNoThrow<uint16_t[]>(count);
  if (!order) {
    LOG_ERR("CLIP", "OOM: selection export order");
    return false;
  }
  bool allSourcesKnown = true;
  for (size_t i = 0; i < count; ++i) {
    order[i] = static_cast<uint16_t>(first + i);
    allSourcesKnown &= words[order[i]].startOffset != UINT32_MAX && words[order[i]].endOffset != UINT32_MAX;
  }
  // Keep navigation in visual order and export in logical source order.
  if (allSourcesKnown) {
    std::sort(order.get(), order.get() + count, [this](const uint16_t a, const uint16_t b) {
      if (words[a].startOffset == words[b].startOffset) return a < b;
      return words[a].startOffset < words[b].startOffset;
    });
  }
  for (size_t i = 0; i < count; ++i) {
    const WordBox& current = words[order[i]];
    const char* word = cleanWordStart(current.text);
    char separator = '\0';
    if (i > 0) {
      const WordBox& previous = words[order[i - 1]];
      const bool sourceKnown = previous.endOffset != UINT32_MAX && current.startOffset != UINT32_MAX;
      const bool separated = sourceKnown ? current.startOffset > previous.endOffset
                                         : current.row != previous.row || current.x > previous.x + previous.width + 2;
      if (current.paragraphStart) {
        separator = '\n';
      } else if (separated) {
        separator = ' ';
      }
    }
    if (!clippingText::append(text, word, separator, CLIPPING_TEXT_MAX, current.discretionaryHyphen)) {
      return false;
    }
  }
  return true;
}

void ClipSelectionActivity::confirmSelection(const ClippingResult::Action action) {
  if (rangeStart < 0) {
    rangeStart = selected;
    requestUpdate();
    return;
  }

  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  ClippingResult result;
  result.action = action;
  const auto heap = HalMemory::getDefaultHeap();
  // reserve() aborts on OOM; leave room for the sort order and error popup too.
  const size_t requiredHeap = CLIPPING_TEXT_MAX + 1 + (last - first + 1) * sizeof(uint16_t) + 1024;
  const bool lowMemory = heap.largestBlockBytes < CLIPPING_TEXT_MAX + 1 || heap.freeBytes < requiredHeap;
  if (lowMemory || !buildSelectedText(first, last, result.text)) {
    static constexpr StrId OPTIONS[] = {StrId::STR_BACK};
    actionPopup.show(lowMemory ? StrId::STR_MEMORY_ERROR : StrId::STR_CLIPPING_TOO_LONG, OPTIONS, 1, 0, [](int) {});
    requestUpdate();
    return;
  }
  result.startPageOffset = words[first].pageOffset;
  result.endPageOffset = words[last].pageOffset;
  result.startWordIndex = words[first].pageWordIndex;
  result.endWordIndex = words[last].pageWordIndex;
  result.wordCount = static_cast<uint16_t>(last - first + 1);
  result.startOffset = UINT32_MAX;
  result.endOffset = 0;
  for (int i = first; i <= last; ++i) {
    if (words[i].startOffset == UINT32_MAX || words[i].endOffset == UINT32_MAX) {
      result.startOffset = result.endOffset = UINT32_MAX;
      break;
    }
    result.startOffset = std::min(result.startOffset, words[i].startOffset);
    result.endOffset = std::max(result.endOffset, words[i].endOffset);
  }
  setResult(std::move(result));
  finish();
}

void ClipSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool ClipSelectionActivity::handleHomeGesture() {
  cancel();
  return true;
}

void ClipSelectionActivity::loopButtons() {
  const uint8_t buttons = selectionInput.pollButtons(mappedInput, millis());
  if (buttons) {
    if (pendingButtonCount < pendingButtons.size()) {
      pendingButtons[pendingButtonCount++] = buttons;
    } else {
      LOG_ERR("CLIP", "Selection button queue full");
    }
  }

  RenderLock lock(RenderLock::Try{});
  if (!lock.held()) return;
  size_t processed = 0;
  while (processed < pendingButtonCount) {
    if (handleButtons(pendingButtons[processed++])) break;
  }
  std::move(pendingButtons.begin() + processed, pendingButtons.begin() + pendingButtonCount, pendingButtons.begin());
  pendingButtonCount -= processed;
}

void ClipSelectionActivity::loop() {
  if (wordCount == 0) return;
  if (!mappedInput.hasTouch()) {
    loopButtons();
    return;
  }

  RenderLock lock;
  if (actionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  int touchX = 0;
  int touchY = 0;
  if (ignoreInitialTouch) {
    if (!mappedInput.isScreenTouchHeld(touchX, touchY)) ignoreInitialTouch = false;
    return;
  }
  if (!touchDragSelecting && mappedInput.wasScreenTapped(touchX, touchY)) {
    if (rangeStart >= 0) {
      const int action = selectionGeometry::actionAt(actionRect(), BaseTheme::SELECTION_ACTION_PAD_PX, touchX, touchY);
      if (action >= 0) {
        confirmSelection(SELECTION_ACTIONS[action]);
        return;
      }
      const int first = std::min(rangeStart, selected);
      const int last = std::max(rangeStart, selected);
      const bool onStart = words[first].pageOffset == currentPageOffset &&
                           selectionGeometry::contains(handleRect(first, true), touchX, touchY);
      const bool onEnd = words[last].pageOffset == currentPageOffset &&
                         selectionGeometry::contains(handleRect(last, false), touchX, touchY);
      if (!onStart && !onEnd && !selectionContains(touchX, touchY)) cancel();
    } else {
      const int hit = wordAt(touchX, touchY);
      if (hit < 0) {
        cancel();
      } else {
        selected = rangeStart = hit;
        requestUpdate();
      }
    }
    return;
  }
  if (touchDragSelecting) {
    if (mappedInput.isScreenTouchHeld(touchX, touchY)) {
      const int deltaX = touchX - touchDragStartX;
      const int deltaY = touchY - touchDragStartY;
      touchDragHasMoved = touchDragHasMoved || deltaX >= TOUCH_DRAG_MOVEMENT_PX || deltaX <= -TOUCH_DRAG_MOVEMENT_PX ||
                          deltaY >= TOUCH_DRAG_MOVEMENT_PX || deltaY <= -TOUCH_DRAG_MOVEMENT_PX;

      const int hit = dragWordAt(touchX + dragOffsetX, touchY + dragOffsetY);
      if (hit >= 0) {
        selectIndex(hit);
      }

      // Hold at the bottom edge or on the final word to extend onto the next page.
      const int nextPageStart = nextPageStartIndexForTouchDrag();
      if (nextPageStart >= 0 && (isWithinCurrentPageEndDwellSlop(touchX, touchY) ||
                                 isWithinCurrentPageEndDwellSlop(touchX + dragOffsetX, touchY + dragOffsetY))) {
        const unsigned long now = millis();
        if (touchDragPageEndIndex != nextPageStart) {
          touchDragPageEndIndex = nextPageStart;
          touchDragPageEndHeldSince = now;
        } else if (now - touchDragPageEndHeldSince >= TOUCH_PAGE_ADVANCE_HOLD_MS) {
          touchDragPageEndIndex = -1;
          selectIndex(nextPageStart);
          dragOffsetX = dragOffsetY = 0;
        }
      } else {
        touchDragPageEndIndex = -1;
      }
      return;
    }
    touchDragSelecting = false;
    touchDragHasMoved = false;
    touchDragPageEndIndex = -1;
    requestUpdate();
    return;
  } else if (mappedInput.wasScreenTouchPressed(touchX, touchY)) {
    if (rangeStart >= 0) {
      const Rect actions = actionRect();
      if (selectionGeometry::actionAt(actions, BaseTheme::SELECTION_ACTION_PAD_PX, touchX, touchY) >= 0) return;
      const int first = std::min(rangeStart, selected);
      const int last = std::max(rangeStart, selected);
      for (int endpoint = 0; endpoint < 2; ++endpoint) {
        const int index = endpoint == 0 ? first : last;
        if (words[index].pageOffset != currentPageOffset) continue;
        const Rect handle = handleRect(index, endpoint == 0);
        if (touchX < handle.x || touchX >= handle.x + handle.width || touchY < handle.y ||
            touchY >= handle.y + handle.height)
          continue;
        selected = index;
        rangeStart = endpoint == 0 ? last : first;
        dragOffsetX = words[index].x + words[index].width / 2 - touchX;
        dragOffsetY = words[index].y + textOffset() + words[index].height / 2 - touchY;
        touchDragSelecting = true;
        touchDragHasMoved = false;
        touchDragStartX = touchX;
        touchDragStartY = touchY;
        touchDragPageEndIndex = -1;
        return;
      }
      return;
    }
    const int hit = wordAt(touchX, touchY);
    if (hit >= 0) {
      selected = rangeStart = hit;
      dragOffsetX = 0;
      dragOffsetY = 0;
      touchDragSelecting = true;
      touchDragHasMoved = false;
      touchDragStartX = touchX;
      touchDragStartY = touchY;
      touchDragPageEndIndex = -1;
      requestUpdate();
    }
    return;
  }

  const uint8_t buttons = Input::buttonEdges(mappedInput);
  if (buttons & (Input::INPUT_BACK | Input::INPUT_CONFIRM)) {
    handleButtons(buttons);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) {
    moveToPage(static_cast<int>(currentPageOffset) + 1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) {
    moveToPage(static_cast<int>(currentPageOffset) - 1);
    return;
  }

  handleButtons(buttons);
}

bool ClipSelectionActivity::handleButtons(const uint8_t buttons) {
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
    confirmSelection();
    return true;
  }

  // Stepping follows the text and the other pair changes row: along the line with Left/Right and
  // between lines with Up/Down, or, on a vertical page, down the column with Up/Down and between
  // columns with Left/Right (Left runs forward, with the text).
  const bool stepBack = vertical() ? (buttons & Input::INPUT_UP) : (buttons & Input::INPUT_LEFT);
  const bool stepForward = vertical() ? (buttons & Input::INPUT_DOWN) : (buttons & Input::INPUT_RIGHT);
  const bool rowBack = vertical() ? (buttons & Input::INPUT_RIGHT) : (buttons & Input::INPUT_UP);
  const bool rowForward = vertical() ? (buttons & Input::INPUT_LEFT) : (buttons & Input::INPUT_DOWN);
  const int next = selectionGeometry::horizontalIndex(selected, static_cast<int>(wordCount), stepBack, stepForward,
                                                      words[selected].isRtl);
  if (next != selected) {
    selectIndex(next);
  } else if (rowBack) {
    moveVertical(-1);
  } else if (rowForward) {
    moveVertical(1);
  }
  return false;
}

Rect ClipSelectionActivity::handleRect(const int index, const bool start) const {
  const WordBox& word = words[index];
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int size = std::max(24, UITheme::getInstance().getMetrics().verticalSpacing * 2);
  if (vertical()) {
    // The start handle sits above the first cell, the end handle below the last.
    const int edge = start ? word.y - size : word.y + word.height;
    return Rect{std::clamp(word.x + word.width / 2 - size / 2, safe.x, safe.x + safe.width - size),
                std::clamp(edge + textOffset(), safe.y, safe.y + safe.height - size), size, size};
  }
  const bool left = start != word.isRtl;
  const int edge = left ? word.x : word.x + word.width;
  return Rect{std::clamp(edge - (left ? size : 0), safe.x, safe.x + safe.width - size),
              std::clamp(word.y + textOffset() + word.height, safe.y, safe.y + safe.height - size), size, size};
}

int ClipSelectionActivity::selectionTop() const {
  const int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  int top = renderer.getScreenHeight();
  for (int i = first; i <= last; ++i) {
    if (words[i].pageOffset == currentPageOffset) top = std::min(top, static_cast<int>(words[i].y));
  }
  // On a vertical page the start handle sits above the first cell; the bar goes above the handle.
  if (vertical()) top -= std::max(24, UITheme::getInstance().getMetrics().verticalSpacing * 2);
  return top;
}

Rect ClipSelectionActivity::actionRect() const {
  Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int rightEdge = std::min(safe.x + safe.width, renderer.getScreenWidth() - right);
  const int bottomEdge = std::min(safe.y + safe.height, renderer.getScreenHeight() - bottom);
  safe.x = std::max(safe.x, left);
  safe.y = std::max(safe.y, top) + std::max(metrics.topPadding, metrics.verticalSpacing);
  safe.width = rightEdge - safe.x;
  safe.height = bottomEdge - safe.y;
  const int font = uiScaleSpec().smallFontId;
  const int labelWidth =
      std::max({renderer.getTextWidth(font, tr(STR_LOOKUP)), renderer.getTextWidth(font, tr(STR_TRANSLATE)),
                renderer.getTextWidth(font, tr(STR_CLIP)), renderer.getTextWidth(font, tr(STR_BOOKMARK_OPTION))});
  const int padding = BaseTheme::SELECTION_ACTION_PAD_PX;
  // Each slot holds an icon over its label; the widest label sets the slot width.
  const int slotWidth = std::max(labelWidth, BaseTheme::SELECTION_ACTION_ICON_PX) + padding * 4;
  constexpr int count = BaseTheme::SELECTION_ACTION_COUNT;
  const int width = std::min(safe.width, count * slotWidth + padding * (count + 1));
  const int height = BaseTheme::SELECTION_ACTION_ICON_PX + BaseTheme::SELECTION_ACTION_GAP_PX +
                     renderer.getLineHeight(font) + 2 * BaseTheme::SELECTION_ACTION_VPAD_PX;
  int first = std::min(rangeStart, selected);
  const int last = std::max(rangeStart, selected);
  while (first < last && words[first].pageOffset < currentPageOffset) ++first;
  // The gap below the card holds the pointer plus a little air before the selected word.
  const int gap = BaseTheme::SELECTION_ACTION_TAIL_PX + 2;
  // The card hangs left-aligned with the first selected word, but never so far right that the
  // pointer, which must clear the rounded corner, could not sit exactly on the word's centre.
  const int wordX = words[first].x + textXOffset();
  const int apexInset = BaseTheme::SELECTION_ACTION_RADIUS_PX + BaseTheme::SELECTION_ACTION_TAIL_PX;
  const int barX = std::min(wordX - padding, wordX + words[first].width / 2 - apexInset);
  return selectionGeometry::actions(safe, selectionTop(), height, gap, width, barX);
}

int ClipSelectionActivity::textOffset() const {
  // A vertical page is full-bleed and drawn by the reader at its own origin; every cell is on screen.
  if (vertical()) return 0;
  if (!mappedInput.hasTouch() && wordCount) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const WordBox& cursor = words[selected];
    return selectionGeometry::keepVisible(cursor.y, cursor.height, safe.y, safe.height);
  }
  return 0;
}

int ClipSelectionActivity::textXOffset() const {
  if (vertical() || mappedInput.hasTouch() || !wordCount) return 0;
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const WordBox& cursor = words[selected];
  return selectionGeometry::keepVisible(cursor.x, cursor.width, safe.x, safe.width);
}

void ClipSelectionActivity::prewarmWord(const int index) const {
  if (vertical()) return;
  if (index >= 0 && index < static_cast<int>(wordCount) && words[index].text) {
    renderer.getFontCacheManager()->prewarmCache(
        words[index].font.fontId, words[index].text,
        static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[index].style) & 0x03)));
  }
}

void ClipSelectionActivity::ditherGapBetween(const WordBox& a, const WordBox& b, const int offsetX,
                                             const int offset) const {
  if (vertical() || a.row != b.row || a.pageOffset != b.pageOffset) return;
  const int leftRight = std::min(a.x + a.width, b.x + b.width);
  const int rightLeft = std::max(a.x, b.x);
  if (leftRight < rightLeft) {
    renderer.fillRectDither(leftRight + offsetX, a.y + offset, rightLeft - leftRight, a.height, Color::LightGray);
  }
}

void ClipSelectionActivity::clearGapBetween(const WordBox& a, const WordBox& b, const int offsetX,
                                            const int offset) const {
  if (vertical() || a.row != b.row || a.pageOffset != b.pageOffset) return;
  const int leftRight = std::min(a.x + a.width, b.x + b.width);
  const int rightLeft = std::max(a.x, b.x);
  if (leftRight < rightLeft) {
    renderer.fillRect(leftRight + offsetX, a.y + offset, rightLeft - leftRight, a.height, false);
  }
}

void ClipSelectionActivity::drawWordClean(const int index, const int offsetX, const int offset) const {
  if (index < 0 || index >= static_cast<int>(wordCount)) return;
  const WordBox& word = words[index];
  if (word.pageOffset != currentPageOffset) return;
  if (vertical()) {
    invertWord(word, offsetX, offset);  // undoes the highlight's inversion
    return;
  }

  prewarmWord(index);

  renderer.fillRect(word.x + offsetX, word.y + offset, word.width, word.height, false);

  if (index > 0) {
    clearGapBetween(words[index - 1], word, offsetX, offset);
  }
  if (index + 1 < static_cast<int>(wordCount)) {
    clearGapBetween(word, words[index + 1], offsetX, offset);
  }

  clippingDrawWord(renderer, word.font, word.x + offsetX, word.y + offset, word.text, word.style);
}

void ClipSelectionActivity::drawWordHighlight(const int index, const int firstSelected, const int lastSelected,
                                              const int offsetX, const int offset) const {
  if (index < 0 || index >= static_cast<int>(wordCount)) return;
  const WordBox& word = words[index];
  if (word.pageOffset != currentPageOffset) return;
  if (vertical()) {
    invertWord(word, offsetX, offset);
    return;
  }

  prewarmWord(index);

  if (index > firstSelected) {
    ditherGapBetween(words[index - 1], word, offsetX, offset);
  }
  if (index < lastSelected) {
    ditherGapBetween(word, words[index + 1], offsetX, offset);
  }

  renderer.fillRectDither(word.x + offsetX, word.y + offset, word.width, word.height, Color::LightGray);
  clippingDrawWord(renderer, word.font, word.x + offsetX, word.y + offset, word.text, word.style);
}

bool ClipSelectionActivity::renderIncremental() {
  if (actionPopup.isActive() || mappedInput.hasTouch()) return false;
  if (lastRenderedPageOffset < 0 || lastRenderedPageOffset != currentPageOffset) return false;
  const int offset = textOffset();
  const int offsetX = textXOffset();
  if (offset != lastRenderedTextOffset || offsetX != lastRenderedTextXOffset) return false;
  if (lastRenderedSelected < 0 || lastRenderedSelected >= static_cast<int>(wordCount)) return false;
  if (selected < 0 || selected >= static_cast<int>(wordCount)) return false;

  // Case 1: Moving cursor before range selection starts (rangeStart < 0)
  if (rangeStart < 0 && lastRenderedRangeStart < 0) {
    if (selected == lastRenderedSelected) return true;

    drawWordClean(lastRenderedSelected, offsetX, offset);
    drawWordHighlight(selected, selected, selected, offsetX, offset);
    if (!vertical()) {
      const WordBox& cursor = words[selected];
      renderer.drawRect(cursor.x + offsetX, cursor.y + offset, cursor.width, cursor.height, true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    if (!vertical()) GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedSelected = selected;
    return true;
  }

  // Case 2: Transition from rangeStart < 0 to rangeStart >= 0 (user just confirmed rangeStart)
  if (rangeStart >= 0 && lastRenderedRangeStart < 0 && rangeStart == selected && selected == lastRenderedSelected) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    if (!vertical()) GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedRangeStart = rangeStart;
    return true;
  }

  // Case 3: Moving cursor to choose end word (rangeStart >= 0, expanding or shrinking selection)
  if (rangeStart >= 0 && lastRenderedRangeStart == rangeStart) {
    if (selected == lastRenderedSelected) return true;

    const int firstOld = std::min(rangeStart, lastRenderedSelected);
    const int lastOld = std::max(rangeStart, lastRenderedSelected);
    const int firstNew = std::min(rangeStart, selected);
    const int lastNew = std::max(rangeStart, selected);

    if (words[firstOld].pageOffset != currentPageOffset || words[lastOld].pageOffset != currentPageOffset ||
        words[firstNew].pageOffset != currentPageOffset || words[lastNew].pageOffset != currentPageOffset) {
      return false;
    }

    for (int i = firstOld; i <= lastOld; ++i) {
      if (i < firstNew || i > lastNew) {
        drawWordClean(i, offsetX, offset);
      }
    }

    for (int i = firstNew; i <= lastNew; ++i) {
      if (i < firstOld || i > lastOld) {
        drawWordHighlight(i, firstNew, lastNew, offsetX, offset);
      }
    }

    if (lastRenderedSelected >= firstNew && lastRenderedSelected <= lastNew) {
      drawWordHighlight(lastRenderedSelected, firstNew, lastNew, offsetX, offset);
    }

    if (!vertical()) {
      const WordBox& cursor = words[selected];
      renderer.drawRect(cursor.x + offsetX, cursor.y + offset, cursor.width, cursor.height, true);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    if (!vertical()) GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    lastRenderedSelected = selected;
    return true;
  }

  return false;
}

void ClipSelectionActivity::drawSelection() const {
  const int offset = textOffset();
  const int offsetX = textXOffset();
  const int first = rangeStart < 0 ? selected : std::min(rangeStart, selected);
  const int last = rangeStart < 0 ? selected : std::max(rangeStart, selected);
  const WordBox* previous = nullptr;
  for (int i = first; i <= last; ++i) {
    const WordBox& word = words[i];
    if (word.pageOffset != currentPageOffset) continue;
    if (vertical()) {
      invertWord(word, offsetX, offset);
      continue;
    }
    if (previous) ditherGapBetween(*previous, word, offsetX, offset);
    renderer.fillRectDither(word.x + offsetX, word.y + offset, word.width, word.height, Color::LightGray);
    clippingDrawWord(renderer, word.font, word.x + offsetX, word.y + offset, word.text, word.style);
    previous = &word;
  }
  if (rangeStart >= 0 && mappedInput.hasTouch()) {
    if (words[first].pageOffset == currentPageOffset)
      // The point aims at the text: inward along the line, or down/up the column.
      GUI.drawSelectionHandle(renderer, handleRect(first, true),
                              vertical()            ? BaseTheme::HandleCorner::BottomRight
                              : !words[first].isRtl ? BaseTheme::HandleCorner::TopRight
                                                    : BaseTheme::HandleCorner::TopLeft);
    if (words[last].pageOffset == currentPageOffset)
      GUI.drawSelectionHandle(renderer, handleRect(last, false),
                              vertical()          ? BaseTheme::HandleCorner::TopLeft
                              : words[last].isRtl ? BaseTheme::HandleCorner::TopRight
                                                  : BaseTheme::HandleCorner::TopLeft);
    if (!touchDragSelecting) {
      // The pointer aims at the first selected word on this page (its cell, on a vertical page).
      int anchor = std::min(rangeStart, selected);
      while (anchor < last && words[anchor].pageOffset != currentPageOffset) ++anchor;
      GUI.drawSelectionActions(renderer, actionRect(), words[anchor].x + offsetX + words[anchor].width / 2);
    }
  } else if (!vertical()) {
    const WordBox& cursor = words[selected];
    renderer.drawRect(cursor.x + offsetX, cursor.y + offset, cursor.width, cursor.height, true);
  }
}

void ClipSelectionActivity::invertWord(const WordBox& word, const int offsetX, const int offset) const {
  renderer.invertRect(word.x + offsetX, word.y + offset, word.width, word.height);
}

void ClipSelectionActivity::drawPage(const int offsetX, const int offsetY) {
  if (vertical()) {
    if (repaintPage && repaintPage(repaintCtx)) return;
    prewarmVerticalPage();
    VerticalTextBlock block(*verticalPage);
    if (furigana) {
      block.render(renderer, fontId, fontId, marginLeft + offsetX, marginTop + offsetY, true);
    } else {
      block.render(renderer, fontId, marginLeft + offsetX, marginTop + offsetY, true);
    }
    return;
  }
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  pages[currentPageOffset]->render(renderer, fontId, marginLeft + offsetX, marginTop + offsetY);
  scope.endScanAndPrewarm();
  pages[currentPageOffset]->render(renderer, fontId, marginLeft + offsetX, marginTop + offsetY);
}

void ClipSelectionActivity::render(RenderLock&&) {
  if (actionPopup.processRender(renderer, mappedInput)) {
    lastRenderedPageOffset = -1;
    return;
  }

  if (renderIncremental()) {
    return;
  }

  const int offset = textOffset();
  const int offsetX = textXOffset();
  renderer.clearScreen();
  const auto clip = renderer.getClipRect();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // The vertical renderer places glyphs through its own cell geometry and is drawn unclipped,
  // as the reader draws it; the horizontal page is clipped to the safe area as before.
  if (!vertical()) renderer.setClipRect(safe.x, safe.y, safe.width, safe.height);
  drawPage(offsetX, offset);
  if (wordCount != 0) drawSelection();
  renderer.setClipRect(clip[0], clip[1], clip[2], clip[3]);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), rangeStart < 0 ? tr(STR_SELECT) : tr(STR_DONE),
                                            tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  // No hint bar on a vertical page: the text is full-bleed and a label bar would cover the last
  // cells of every column (the word-lookup panel makes the same call).
  if (!vertical()) GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();

  lastRenderedPageOffset = currentPageOffset;
  lastRenderedTextOffset = offset;
  lastRenderedTextXOffset = offsetX;
  lastRenderedRangeStart = rangeStart;
  lastRenderedSelected = selected;
}
