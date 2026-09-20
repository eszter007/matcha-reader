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
  // targetGlyph: a character of panelText (codepoint index, line breaks not counted) that a hold on
  // the page landed on. The lookup opens on the word covering it -- or the nearest word -- instead
  // of the first match or the remembered position. -1 when opened from the menu or a key.
  explicit MangaWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& panelText,
                                   std::string scanCachePath = "", uint16_t pageIndex = 0, uint16_t panelIndex = 0,
                                   int targetGlyph = -1);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Full CPU + fast main-loop ticks while the progressive scan runs (see EpubReaderWordLookupActivity).
  bool skipLoopDelay() override { return !scan.isDone(); }

 private:
  WordSelectionScan scan;

  int cursorIndex = 0;
  int targetGlyph = -1;
  // Selectable word covering targetGlyph, else the one whose start is nearest; -1 when the text
  // has none. Scans ahead as far as needed first.
  int selectableForGlyph(size_t glyph);

  bool hasResult = false;
  std::string resultHeadword;
  std::string resultDefinition;
  std::string resultReading;
  std::string resultGrammar;
  int resultMatchLen = 0;
  int scrollOffset = 0;
  int totalLines = 0;
  int maxScroll = 0;
  // Body lines that fit at once, from the last render: the step a swipe scrolls by.
  int visibleCapacity = 1;

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

  // Draws the definition text (or the loading/no-match notice) into the panel's inner rectangle.
  void renderContentArea(const Rect& body);

  // Which dictionary the shown entry came from, for the panel footer. Literals, not tr()
  // strings: these are the dictionaries' own names. Null until a lookup lands.
  const char* resultSource = nullptr;
  std::string resultDictionaryLabel;
  const char* dictionaryLabel() const {
    return resultDictionaryLabel.empty() ? resultSource : resultDictionaryLabel.c_str();
  }
};
