#pragma once

#include <Epub/FootnoteEntry.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/PanelTextPages.h"
#include "util/ButtonNavigator.h"

class Epub;

// The page's footnotes in the floating dictionary panel, drawn over the page: the note's own text
// (extracted on demand from its target file -- see FootnoteTextExtractor). A long note scrolls
// (Up/Down, vertical swipes); a page turn (Left/Right, or the reader's touch setting) moves to the
// next or previous note. A tap outside the panel or Back returns to the page, and Confirm (or
// Power) jumps to the note's location.
class EpubReaderFootnotesActivity final : public Activity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes, Epub* epub, int currentSpineIndex,
                                       int startIndex = 0)
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
  PanelTextPages textPages;
  int scrollLine = 0;  // first line of the note shown; the note scrolls, notes page

  ButtonNavigator buttonNavigator;

  // Loads note `index` and shows it from its top. Clamped to the notes that exist.
  void selectFootnote(int index);
  // Moves to the next (+1) or previous (-1) note; ends are hard stops.
  void stepNote(int direction);
  // Scrolls the note by `lines`, clamped to its length.
  void scrollBy(int lines);
  void cancel();
};
