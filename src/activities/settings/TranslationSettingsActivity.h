#pragma once

#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Settings for page translation: the Gemini API key and the model.
class TranslationSettingsActivity final : public UiListActivity {
 public:
  TranslationSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEMS = 2;

  void render(RenderLock&&) override;

 private:
  OptionPopup optionPopup;

  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void editCustomModel();

  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};
