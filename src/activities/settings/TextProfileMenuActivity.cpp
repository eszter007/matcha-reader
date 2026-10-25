#include "TextProfileMenuActivity.h"

#include <I18n.h>

#include <iterator>
#include <memory>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "TextSettingsActivity.h"
#include "components/UITheme.h"
#include "util/TextProfiles.h"

namespace fui = freeink::ui;

namespace {
// Menu order; TextProfileId is the storage order.
constexpr CrossPointSettings::TextProfileId MENU_PROFILES[] = {
    CrossPointSettings::TextProfileId::Japanese, CrossPointSettings::TextProfileId::TraditionalChinese,
    CrossPointSettings::TextProfileId::SimplifiedChinese, CrossPointSettings::TextProfileId::Korean,
    CrossPointSettings::TextProfileId::Latin};
}  // namespace

int TextProfileMenuActivity::listCount() const { return static_cast<int>(std::size(MENU_PROFILES)); }

const char* TextProfileMenuActivity::headerTitle() const { return tr(STR_TEXT_SETTINGS); }

void TextProfileMenuActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (items_.empty()) {
    items_.reserve(std::size(MENU_PROFILES));
    for (int i = 0; i < listCount(); i++) {
      fui::ListItem item;
      item.label = textprofile::title(MENU_PROFILES[i]);
      item.actionValue = static_cast<int16_t>(i);
      items_.push_back(item);
    }
  }
  fui::ListProps props;
  props.items = items_.data();
  props.count = static_cast<uint16_t>(items_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void TextProfileMenuActivity::activateIndex(const int index) {
  app.clearTapFlash();
  const auto profile = MENU_PROFILES[index];
  startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                profile, textprofile::script(profile),
                                                                /*verticalText=*/false, /*editStoredProfile=*/true),
                         [this](const ActivityResult&) {
                           // Not every row there saves, and the profile went back only in memory.
                           SETTINGS.saveToFile();
                           requestUpdate();
                         });
}
