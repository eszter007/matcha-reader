#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "WordSelectionScan.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class MangaWordLookupActivity final : public Activity {
 public:
  // Progressive open, same engine as EpubReaderWordLookupActivity (see WordSelectionScan): the
  // constructor only scans far enough to show the first word; the rest of the text is mapped in
  // the background from loop(). When scanCachePath is given, a completed scan is persisted there
  // keyed by (pageIndex, panelIndex) and re-opening the same unchanged text skips scanning.
  explicit MangaWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string panelText,
                                   std::string scanCachePath = "", uint16_t pageIndex = 0, uint16_t panelIndex = 0);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Full CPU + fast main-loop ticks while the progressive scan runs (see EpubReaderWordLookupActivity).
  bool skipLoopDelay() override { return !scan.isDone(); }

 private:
  WordSelectionScan scan;

  int cursorIndex = 0;

  bool hasResult = false;
  std::string resultHeadword;
  std::string resultDefinition;
  int resultMatchLen = 0;
  int scrollOffset = 0;
  int totalLines = 0;
  int maxScroll = 0;

  ButtonNavigator buttonNavigator;

  // Scan-result persistence (empty path = disabled).
  std::string scanCachePath;
  uint16_t scanPage = 0;
  uint16_t scanPanel = 0;

  void initScanFromCacheOrBurst();
  void runInitialBurst();
  void moveCursor(int delta);
  void performLookup();
  void performLookupImpl();
  // True while performLookup() executes; render() shows "Loading..." instead of "No match found".
  bool lookupInFlight = false;
  std::string buildLookupText(size_t startIdx) const;

  bool initialRenderDone = false;
  int fastRefreshCount = 0;
  static constexpr int kFullRefreshInterval = 10;

  void renderContentArea(const Rect& screen, int contentTop);
};
