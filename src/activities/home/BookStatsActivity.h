#pragma once
#include <string>

#include "BookStats.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// One book's stats, opened by long-pressing it in the Library or on the Home cover grid.
// startActivityForResult, not replaceActivity, so the screen underneath keeps its selection.
class BookStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  std::string bookPath;
  std::string bookTitle;
  // Held for the activity's life: a few hundred bytes, and re-reading it would hit SD per frame.
  BookStats stats;
  // Swallows the release that ends a long Back press, so going home does not also finish().
  bool backLongPressFired = false;
  int scrollOffset = 0;
  int maxScrollOffset = 0;
  uint16_t calYear = 0;
  uint8_t calMonth = 1;

 public:
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path, std::string title)
      : Activity("BookStats", renderer, mappedInput), bookPath(std::move(path)), bookTitle(std::move(title)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Open a book's stats over `host`. The one place both cover grids go through, so a long press
  // means the same thing in each. `onDone` runs when the stats screen closes -- the host uses it
  // to drop any partial-redraw state, since the stats painted over its frame.
  static void openFor(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                      std::string title, ActivityResultHandler onDone);
};
