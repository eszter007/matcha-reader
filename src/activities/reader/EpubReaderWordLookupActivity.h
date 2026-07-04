#pragma once

#include <GfxRenderer.h>
#include <Epub/VerticalParsedText.h>

struct Rect;
class Page;

#include <string>
#include <vector>

#include "WordSelectionScan.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderWordLookupActivity final : public Activity {
 public:
  // Progressive open (see WordSelectionScan): the constructor only scans far enough to show the
  // first word (~300ms); the rest of the page is mapped in the background from loop(). When
  // scanCachePath is given, a completed scan is persisted there keyed by (spine, page) -- a
  // later re-open of the same unchanged page loads it back and skips scanning entirely.
  // Vertical (tategaki) reading mode.
  explicit EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const VerticalPage& page, std::string scanCachePath = "",
                                        uint16_t spineIndex = 0, uint16_t pageIndex = 0);
  // Horizontal (yokogaki) reading mode.
  explicit EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const Page& page, std::string scanCachePath = "",
                                        uint16_t spineIndex = 0, uint16_t pageIndex = 0);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Full CPU + fast main-loop ticks while the progressive scan is running, so the between-poll
  // scan slices stay short enough that button presses are never missed.
  bool skipLoopDelay() override { return !scan.isDone(); }

 private:
  WordSelectionScan scan;

  int cursorIndex = 0;

  bool hasResult = false;
  std::string resultHeadword;
  std::string resultDefinition;
  int resultMatchLen = 0;
  bool hasGrammar = false;
  std::string grammarHeadword;
  std::string grammarDefinition;
  int scrollOffset = 0;  // lines scrolled within current entry
  int totalLines = 0;    // total lines in current definition
  int maxScroll = 0;     // max scroll offset (leaves a screenful visible)

  ButtonNavigator buttonNavigator;

  // Scan-result persistence (empty path = disabled).
  std::string scanCachePath;
  uint16_t scanSpine = 0;
  uint16_t scanPage = 0;
  bool scanCacheSaved = false;

  void initScanFromCacheOrBurst(const char* label);
  void runInitialBurst(const char* label);
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

  void renderContentArea(const Rect& screen, int contentTop);
};
