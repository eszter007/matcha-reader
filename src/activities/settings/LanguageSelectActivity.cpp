#include "LanguageSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "LanguagePackDownloadActivity.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

LanguageSelectActivity::LanguageSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("LanguageSelect", renderer, mappedInput) {}

void LanguageSelectActivity::onEnter() {
  UiListActivity::onEnter();

  // Open on the current language, which may sit past the first page; the first
  // screen build pulls the viewport to it (ListNav follow-on-build).
  const auto currentLang = static_cast<uint8_t>(I18N.getLanguage());
  const auto* begin = std::begin(SORTED_LANGUAGE_INDICES);
  const auto* end = std::end(SORTED_LANGUAGE_INDICES);
  const auto* it = std::find(begin, end, currentLang);
  updateRow = LanguagePackDownloadActivity::hasInstalledPacks() ? 1 : 0;
  nav.selected = ((it != end) ? static_cast<int>(std::distance(begin, it)) : 0) + updateRow;

  if (updateRow) {
    fui::ListItem item;
    item.label = tr(STR_UPDATE_LANGUAGE_PACKS);
    item.actionValue = 0;
    rowItems[0] = item;
  }

  // Built once here rather than every buildScreen() call: labels are static,
  // and the "Selected" marker can't go stale mid-visit since activateIndex()
  // finishes the activity immediately on selection.
  for (int i = 0; i < totalItems; ++i) {
    const auto lang = static_cast<Language>(SORTED_LANGUAGE_INDICES[i]);
    fui::ListItem item;
    item.label = I18N.getLanguageName(lang);
    if (SORTED_LANGUAGE_INDICES[i] == currentLang) {
      item.value = tr(STR_SELECTED);
    } else {
      // Selecting either downloads the pack: missing, or built for other firmware and refused.
      const auto pack = I18n::packState(lang);
      if (pack == I18n::PackState::Missing) item.value = tr(STR_DOWNLOAD);
      if (pack == I18n::PackState::Outdated) item.value = tr(STR_UPDATE);
    }
    item.actionValue = static_cast<int16_t>(i + updateRow);
    rowItems[i + updateRow] = item;
  }

  // The Chinese entries name themselves in characters the built-in CJK subset lacks (简, 體):
  // bring the SD companion in for this list, as Home does for a Chinese title.
  sdFontSystem.setUiCjkNeededForTitles(renderer, rowItems, [](const fui::ListItem& item) { return item.label; });
}

const char* LanguageSelectActivity::headerTitle() const { return tr(STR_LANGUAGE); }

void LanguageSelectActivity::activateIndex(const int index) {
  // The activated row leaves this screen; a lingering flash would gray an
  // unrelated element on the next render.
  app.clearTapFlash();
  nav.selected = index;
  if (index < updateRow) {
    downloadPack(Language::_COUNT);
    return;
  }
  const uint8_t langIndex = SORTED_LANGUAGE_INDICES[index - updateRow];

  bool switched = false;
  {
    RenderLock lock(*this);
    switched = I18N.setLanguage(static_cast<Language>(langIndex));
  }

  // Only record a language the UI actually switched to. A missing or stale pack leaves the
  // interface in the previous one; fetch the pack instead, which records the choice once it loads.
  if (!switched) {
    downloadPack(static_cast<Language>(langIndex));
    return;
  }

  SETTINGS.language = langIndex;
  SETTINGS.saveToFile();

  // Return to previous page
  finish();
}

void LanguageSelectActivity::downloadPack(const Language target) {
  startActivityForResult(std::make_unique<LanguagePackDownloadActivity>(renderer, mappedInput, target),
                         [this](const ActivityResult& result) {
                           // Leaving the download screen normally restarts the device to free the
                           // Wi-Fi heap, so this runs only when it did not. A switched language
                           // leaves, as a direct pick does: the rows were built for the old one.
                           if (result.isCancelled) {
                             requestUpdate();
                           } else {
                             finish();
                           }
                         });
}

void LanguageSelectActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // Content: the safe area minus the header band GUI.drawHeader paints.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  // rowItems was built once in onEnter() and is reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(totalItems + updateRow);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  // Label at the value's font size: both sides of the row read as one unit.
  // maxLines=2 also marks the style caller-owned (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
