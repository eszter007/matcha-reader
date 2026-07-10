#pragma once

#include <Epub/FootnoteEntry.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class Epub;
struct Rect;

// Footnote panel in the Word Lookup style: shows the CURRENT page's footnotes' actual text in
// place (extracted on demand from the note's target file -- see FootnoteTextExtractor) instead
// of a bare list of numbers that immediately navigates away. Left/Right cycles through the
// page's footnotes (wrapping), Up/Down scrolls a long note, Confirm jumps to the footnote's
// location (previous behavior), Back returns to the page.
class EpubReaderFootnotesActivity final : public Activity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes, Epub* epub,
                                       int currentSpineIndex, int startIndex = 0)
      : Activity("EpubReaderFootnotes", renderer, mappedInput),
        footnotes(footnotes),
        epub(epub),
        currentSpineIndex(currentSpineIndex),
        startIndex(startIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const std::vector<FootnoteEntry>& footnotes;
  Epub* epub;
  int currentSpineIndex;

  int startIndex = 0;
  int selectedIndex = 0;
  std::string noteText;  // extracted text of the selected footnote
  bool noteLoaded = false;
  int scrollOffset = 0;  // lines scrolled within the current note
  int totalLines = 0;
  int maxScroll = 0;

  bool initialRenderDone = false;
  int fastRefreshCount = 0;
  static constexpr int kFullRefreshInterval = 10;

  ButtonNavigator buttonNavigator;

  void selectFootnote(int index);
  void renderContentArea(const Rect& screen, int contentTop);
};
