#include "HyphenationLanguagesActivity.h"

#include <Epub/hyphenation/HyphenationPack.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/hyphenation/LanguageRegistry.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "HyphenationPackDownloadActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "util/HyphenationPacks.h"

namespace fui = freeink::ui;

HyphenationLanguagesActivity::HyphenationLanguagesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("HyphenationLanguages", renderer, mappedInput) {}

void HyphenationLanguagesActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildRows();
}

const char* HyphenationLanguagesActivity::headerTitle() const { return tr(STR_HYPHENATION_LANGUAGES); }

void HyphenationLanguagesActivity::rebuildRows() {
  size_t count = 0;
  const LanguageEntry* languages = hyphenationLanguages(&count);
  rows.clear();
  rows.reserve(count);
  for (size_t i = 0; i < count; i++) {
    if (languages[i].builtIn) continue;
    rows.push_back({&languages[i], hyphpacks::displayName(languages[i]), hyphpacks::isInstalled(languages[i])});
  }
  items.assign(rows.size(), fui::ListItem{});
  for (size_t i = 0; i < rows.size(); i++) {
    items[i].label = rows[i].name.c_str();
    items[i].value = rows[i].installed ? tr(STR_INSTALLED) : tr(STR_DOWNLOAD);
    items[i].actionValue = static_cast<int16_t>(i);
  }
  requestUpdate();
}

void HyphenationLanguagesActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  nav.selected = index;
  const Row& row = rows[index];
  if (row.installed) {
    confirmRemove(row);
    return;
  }
  auto download = makeUniqueNoThrow<HyphenationPackDownloadActivity>(renderer, mappedInput, *row.language);
  if (!download) {
    LOG_ERR("HYPH", "OOM: download screen");
    return;
  }
  // Leaving the download screen after Wi-Fi restarts the device; this runs only when it did not.
  startActivityForResult(std::move(download), [this](const ActivityResult&) { rebuildRows(); });
}

void HyphenationLanguagesActivity::confirmRemove(const Row& row) {
  char heading[64];
  snprintf(heading, sizeof(heading), tr(STR_HYPHENATION_PACK_REMOVE_FORMAT), row.name.c_str());
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, std::string{}, std::string{},
                                                         tr(STR_REMOVE));
  if (!confirm) {
    LOG_ERR("HYPH", "OOM: confirmation");
    return;
  }
  const LanguageEntry* language = row.language;
  startActivityForResult(std::move(confirm), [this, language](const ActivityResult& result) {
    if (!result.isCancelled) {
      char path[HyphenationPack::PATH_SIZE];
      Hyphenator::resetPacks();  // nothing may still read the pack being removed
      if (HyphenationPack::pathFor(language->primaryTag, path, sizeof(path)) && !Storage.remove(path)) {
        LOG_ERR("HYPH", "Could not remove %s", path);
      }
    }
    rebuildRows();
  });
}

void HyphenationLanguagesActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
