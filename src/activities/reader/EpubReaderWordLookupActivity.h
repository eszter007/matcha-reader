#pragma once

#include <Epub/VerticalParsedText.h>
#include <GfxRenderer.h>
#include <freertos/FreeRTOS.h>

struct Rect;
class Page;

#include <string>
#include <vector>

#include "WordSelectionScan.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// What the vertical reader hands the panel so the word cursor can be shown ON the page (select
// mode) instead of opening straight into the definition view.
//
// The panel deliberately does NOT own a copy of the page: a VerticalPage runs to ~15KB of glyphs,
// which is the same headroom the scan and the dictionary caches need. It works from the page
// geometry below plus the pixels the reader already left in the framebuffer, and when those
// pixels are gone (returning from a definition) it asks the reader to paint them again.
struct VerticalSelectContext {
  int marginLeft = 0;
  int marginTop = 0;
  // Kihon-hanmen cell, measured by the reader while its fonts were still resident. Measuring it
  // in the panel would probe a released SD font and silently fall back to the line height.
  int cellPx = 0;
  // Repaints the reader's current page (body + status bar) into the framebuffer. Called from the
  // panel's render(), i.e. under the render lock, which is what the shared page slot requires.
  // Returns false when the page could not be drawn (slot not re-faultable), so the panel can
  // retry on the next render instead of leaving a blank frame with a cursor on it.
  bool (*repaintPage)(void*) = nullptr;
  void* repaintCtx = nullptr;
  // The framebuffer already holds that page, so the first render can skip the repaint entirely
  // and just place the cursor -- this is what makes opening the panel feel instant. False when
  // the panel is opened from the reader menu, which left its own pixels on screen.
  bool pageOnScreen = false;
  bool valid() const { return repaintPage != nullptr && cellPx > 0; }
};

class EpubReaderWordLookupActivity final : public Activity {
 public:
  // Progressive open (see WordSelectionScan): the constructor only scans far enough to show the
  // first word (~300ms); the rest of the page is mapped in the background from loop(). When
  // scanCachePath is given, a completed scan is persisted there keyed by (spine, page) -- a
  // later re-open of the same unchanged page loads it back and skips scanning entirely.
  // Vertical (tategaki) reading mode. With a valid selectContext the panel opens in SELECT mode:
  // the page stays on screen with the current word highlighted, and the definition view is only
  // entered on Confirm. Without one it opens straight into the definition view (the horizontal
  // and manga behaviour).
  explicit EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const VerticalPage& page, std::string scanCachePath = "",
                                        uint16_t spineIndex = 0, uint16_t pageIndex = 0,
                                        const VerticalSelectContext& selectContext = {});
  // Horizontal (yokogaki) reading mode.
  explicit EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const Page& page,
                                        std::string scanCachePath = "", uint16_t spineIndex = 0,
                                        uint16_t pageIndex = 0);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Full CPU + fast main-loop ticks while the progressive scan is running, so the between-poll
  // scan slices stay short enough that button presses are never missed.
  bool skipLoopDelay() override { return !scan.isDone(); }
  // Part of the reading flow, opened from the page mid-read: both views keep the reading
  // surface's night-mode polarity, exactly as the English word-select and definition activities
  // do. Resolving it per mode instead would inflict a full-screen polarity change on every
  // Confirm and every Back.
  bool appliesNightMode() const override { return true; }

 private:
  WordSelectionScan scan;

  // Select: the page with a highlight box on the current word. Definition: the full-screen
  // dictionary entry. Confirm moves Select -> Definition, Back moves back.
  enum class Mode : uint8_t { Select, Definition };
  Mode mode = Mode::Definition;
  VerticalSelectContext selectCtx;

  // --- Select mode state -------------------------------------------------------------------
  // The framebuffer holds the page, so a cursor move only has to XOR two boxes.
  bool selectPageDrawn = false;

  // Highlight geometry in screen coordinates. Computed on the main task whenever the cursor
  // moves and read by the render task: the render path deliberately does NOT index the scan's
  // vectors, because the background walk grows them from the main task and a reallocation under
  // a render would be a use-after-free.
  struct HighlightBox {
    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
  };
  // A match spans one column, or two when it wraps a column break; the cap only bites on an
  // implausibly long digit run, where dropping the tail of the box is purely cosmetic.
  static constexpr int kMaxHighlightBoxes = 4;
  HighlightBox cursorBoxes[kMaxHighlightBoxes];
  int cursorBoxCount = 0;
  // Guards cursorBoxes/cursorBoxCount only, and only for the copy in and the copy out: the two
  // tasks must never pair one move's count with another move's rectangles. A ~32-byte copy is
  // short enough for a critical section, and far cheaper than making the main loop wait on the
  // render lock, which is held across a full e-ink refresh.
  portMUX_TYPE boxMux = portMUX_INITIALIZER_UNLOCKED;
  // What is XOR-ed into the framebuffer right now, owned by the render task alone. Kept separate
  // from cursorBoxes so the erase always matches what was actually drawn, even if the cursor
  // moved while a render was in flight -- an erase against the wrong rectangle would leave
  // inverted debris on the page.
  HighlightBox drawnBoxes[kMaxHighlightBoxes];
  int drawnBoxCount = 0;

  // The reader opens the panel on a long press, so the Confirm release that follows belongs to
  // that press, not to a selection. Ignore it until a fresh press is seen.
  bool confirmPressSeen = false;

  // A move the user has already asked for that the sequential scan has not reached yet. Page
  // positions are known for every cell from the moment the page loads, but which cell STARTS a
  // word is only known below the scan frontier -- so a jump into unmapped text is parked here
  // and completed from loop() as the frontier passes it, instead of blocking the activity (Back
  // and further presses keep working) or refusing the move. This is what makes the bottom and
  // the left of a page reachable while the page is still being mapped.
  struct PendingMove {
    enum class Kind : uint8_t { None, Word, Column };
    Kind kind = Kind::None;
    int delta = 0;              // direction, in words or in columns
    uint16_t targetColumn = 0;  // Column: the column being entered
    uint16_t anchorRow = 0;     // Column: the row to land nearest to
    // Column: last glyph of the column being waited on, remembered so each further tick of the
    // wait is one comparison. Finding it means walking the page's glyphs, and doing that on every
    // tick would steal the CPU from the walk the wait is waiting for -- loop() runs flat out
    // while the scan is unfinished (see skipLoopDelay()).
    size_t waitUntilGlyph = 0;
    bool hasWaitTarget = false;
  };
  PendingMove pending;

  void renderSelect();
  // Rebuild cursorBoxes for the current cursor. Main task only (it reads the scan vectors).
  // A match that wraps from the foot of one column to the head of the next becomes one box per
  // column, so the highlight never covers the gutter between them.
  void refreshCursorBoxes();
  // XOR the given boxes. Self-inverse -- the same call over the same boxes erases them.
  void invertBoxes(const HighlightBox* boxes, int count) const;
  void enterDefinition();
  void returnToSelect();
  bool handleSelectInput();
  bool handleDefinitionInput();
  void moveSelection(int delta);
  void jumpColumn(int direction);
  // Index math for a reading-order step, WITHOUT the dictionary read moveCursor() does. Returns
  // false when the step needs text the scan has not mapped yet, leaving outIndex unchanged.
  bool stepCursor(int delta, int& outIndex);
  void resolvePendingMove();
  // First and last allGlyphs index of a column, or false when the page has no such column.
  bool columnRange(uint16_t column, size_t& first, size_t& last) const;
  // Lowest and highest column on the page. Layout data, known as soon as the page loads.
  bool columnBounds(uint16_t& outMin, uint16_t& outMax) const;
  // allGlyphs index of the cell in `column` nearest `row`. The cell a jump actually lands on, so
  // it is what a parked jump waits for -- waiting for the column's LAST cell segments the whole
  // column when only this part of it is needed. False when the page has no such column.
  bool columnAnchorGlyph(uint16_t column, uint16_t row, size_t& outGlyph) const;
  // Point the walk at the next column in `direction` so a second jump the same way finds it
  // already segmented. Costs nothing when that column is done: aimAtGlyph() ignores it.
  void prefetchColumn(uint16_t fromColumn, int direction);
  // Selectable entry in `column` nearest `anchorRow`; -1 when the column has none (mapped).
  int selectableInColumn(uint16_t column, uint16_t anchorRow) const;
  // Open the cursor mid-page instead of at the first word, so any word on the page is at most
  // half a page of presses away (the horizontal picker does the same). Parks as a pending column
  // move when the walk has not reached the middle yet.
  void selectMiddleOfPage();
  // Middle column/row of the page and the allGlyphs index that column starts at -- where the
  // wrapped walk begins. False when the page has no glyphs. Layout data only, no dictionary work.
  bool middleTarget(uint16_t& outColumn, uint16_t& outRow, size_t& outFirstGlyph) const;

  int cursorIndex = 0;

  bool hasResult = false;
  // Which dictionary the shown entry came from, for the panel footer. A literal, not a tr()
  // string: these are the dictionaries' own names, the same way the English panel shows the
  // .ifo's bookname. Null until a lookup lands.
  const char* resultSource = nullptr;
  const char* dictionaryLabel() const { return resultSource; }
  std::string resultHeadword;
  std::string resultDefinition;
  int resultMatchLen = 0;
  bool hasGrammar = false;
  std::string grammarHeadword;
  std::string grammarDefinition;
  // Tategaki shows ONE source per page: the merged definition is split at the separators the
  // dictionaries put between entries, each piece keeping the attribution line it ended with as
  // its footer label. Left/Right walk these; the word cursor does not move from the definition
  // view here, which is what horizontal and manga use those buttons for.
  std::vector<std::string> sectionText;
  std::vector<std::string> sectionLabel;
  int currentSection = 0;
  void splitDefinitionIntoSections();
  // Text and footer label for what is on screen: the current section in tategaki, the whole
  // merged definition otherwise.
  const std::string& visibleDefinition() const;
  const char* visibleLabel() const;

  int scrollOffset = 0;     // lines scrolled within current entry
  int totalLines = 0;       // total lines in current definition
  int maxScroll = 0;        // max scroll offset (leaves a screenful visible)
  int visibleCapacity = 1;  // body lines that fit at once; the step size in paged mode

  // Vertical (tategaki) reading pages the definition a screenful at a time and shows a page
  // counter, instead of the free scrolling the horizontal and manga panels keep. scrollOffset
  // is still the source of truth -- paging just quantizes it to whole screenfuls.
  bool pagedDefinition() const { return selectCtx.valid(); }

  ButtonNavigator buttonNavigator;

  // Scan-result persistence (empty path = disabled).
  std::string scanCachePath;
  uint16_t scanSpine = 0;
  uint16_t scanPage = 0;
  // Book cache dir (derived from scanCachePath) for the furigana glossary; empty = disabled.
  std::string bookCachePath;

  // Prepend "In this book: <reading>" to resultDefinition when the book's furigana
  // glossary has an entry for the selected surface text (see RubyGlossary). Books
  // typically annotate a name's reading only on first appearance -- this surfaces it
  // on every later occurrence too. No entry -> no line.
  void prependBookReading(const std::string& surface);

  void reclaimFontHeap();
  void initScanFromCacheOrBurst(const char* label);
  void runInitialBurst(const char* label);
  // Wraps scan.step(): if the scan aborted a walk under low heap (fewer entries than the page
  // really has), release the font caches once and restart the walk over the intact glyph list,
  // so a fragmented first open self-heals instead of the user having to reopen via the menu.
  bool stepScan(uint32_t budgetMs);
  bool scanHealAttempted = false;
  void moveCursor(int delta);
  void performLookup();
  void performLookupImpl();
  // True while performLookup() is executing; render() shows "Loading..." instead of
  // "No match found" so fast navigation never flashes a false negative.
  bool lookupInFlight = false;
  std::string buildLookupText(size_t startIdx) const;

  bool initialRenderDone = false;
  int fastRefreshCount = 0;
  static constexpr int kFullRefreshInterval = 10;

  // Draws the definition text (or the loading/no-match notice) into the panel's inner rectangle.
  void renderContentArea(const Rect& body);
};
