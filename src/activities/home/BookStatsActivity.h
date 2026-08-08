#pragma once
#include <string>

#include "BookStats.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// One book's stats, opened by long-pressing it in the Library.
// startActivityForResult, not replaceActivity, so the Library keeps its tab and selection.
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
};
