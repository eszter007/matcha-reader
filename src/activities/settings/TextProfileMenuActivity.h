#pragma once

#include <vector>

#include "activities/UiListActivity.h"

// The five text profiles, from Settings: picking one opens its Text Settings.
class TextProfileMenuActivity final : public UiListActivity {
 public:
  TextProfileMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("TextProfiles", renderer, mappedInput) {}

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  std::vector<freeink::ui::ListItem> items_;
};
