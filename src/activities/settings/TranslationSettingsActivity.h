#pragma once

#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// Settings for page translation: the Gemini API key, the model and the target language.
class TranslationSettingsActivity final : public UiListActivity {
 public:
  TranslationSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MENU_ITEMS = 3;

  void render(RenderLock&&) override;

 private:
  OptionPopup optionPopup;

  int listCount() const override { return MENU_ITEMS; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void editCustomModel();
  void editCustomLanguage();

  std::string rowValues_[MENU_ITEMS];
  freeink::ui::ListItem rowItems_[MENU_ITEMS]{};
};
