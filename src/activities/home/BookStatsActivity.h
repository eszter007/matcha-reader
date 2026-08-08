#pragma once
#include <string>

#include "BookStats.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Per-book reading stats: sessions, time, average session, days read, and a calendar of the days
// this book was read. Reached by long-pressing a book in the Library.
//
// Started with startActivityForResult rather than replaceActivity so the Library keeps its tab,
// scroll position and selected book while this is on top.
class BookStatsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  std::string bookPath;
  std::string bookTitle;
  // Loaded once in onEnter and held for the life of the activity: it is one book's history
  // (a few hundred bytes) and re-reading it on every render would hit the SD card per frame.
  BookStats stats;
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
