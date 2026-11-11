#include "TranslationSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <array>
#include <memory>
#include <string>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "util/GeminiConfig.h"

namespace fui = freeink::ui;

namespace {
constexpr int IDX_KEY = 0;
constexpr int IDX_MODEL = 1;
constexpr int IDX_TARGET = 2;

constexpr StrId menuNames[TranslationSettingsActivity::MENU_ITEMS] = {StrId::STR_GEMINI_API_KEY,
                                                                      StrId::STR_GEMINI_MODEL, StrId::STR_TRANSLATE_TO};

// The listed languages, then Custom (gemini::TARGET_CUSTOM).
constexpr auto targetLabels = [] {
  std::array<StrId, gemini::TARGET_LANGUAGE_COUNT + 1> labels{};
  for (size_t i = 0; i < gemini::TARGET_LANGUAGE_COUNT; i++) labels[i] = gemini::TARGET_LANGUAGES[i].label;
  labels[gemini::TARGET_CUSTOM] = StrId::STR_GEMINI_MODEL_CUSTOM;
  return labels;
}();

// Indexed by gemini::ModelChoice.
constexpr int MODEL_ITEMS = 3;
constexpr StrId modelNames[MODEL_ITEMS] = {StrId::STR_GEMINI_MODEL_DEFAULT, StrId::STR_GEMINI_MODEL_LITE,
                                           StrId::STR_GEMINI_MODEL_CUSTOM};
}  // namespace

TranslationSettingsActivity::TranslationSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("TranslationSettings", renderer, mappedInput) {
  for (int i = 0; i < MENU_ITEMS; i++) {
    rowItems_[i].label = I18N.get(menuNames[i]);
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }
}

const char* TranslationSettingsActivity::headerTitle() const { return tr(STR_TRANSLATION_SETTINGS); }

bool TranslationSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void TranslationSettingsActivity::editCustomModel() {
  const std::string current = gemini::readModel();
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GEMINI_MODEL),
                                              current.empty() ? "gemini-" : current, 63, InputType::Text),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          gemini::writeModel(std::get<KeyboardResult>(result.data).text);
        }
        requestUpdate();
      });
}

void TranslationSettingsActivity::editCustomLanguage() {
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_XLAT_CUSTOM_LANGUAGE),
                                                                 gemini::readCustomLanguage(),
                                                                 gemini::CUSTOM_LANGUAGE_MAX, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled)
                             gemini::writeCustomLanguage(std::get<KeyboardResult>(result.data).text);
                           requestUpdate();
                         });
}

void TranslationSettingsActivity::activateIndex(const int index) {
  if (optionPopup.isActive()) return;
  app.clearTapFlash();
  if (index == IDX_KEY) {
    std::string current;
    gemini::readKey(current);
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_GEMINI_API_KEY),
                                                                   current, 191, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) gemini::writeKey(std::get<KeyboardResult>(result.data).text);
                             requestUpdate();
                           });
  } else if (index == IDX_MODEL) {
    optionPopup.show(StrId::STR_GEMINI_MODEL, modelNames, MODEL_ITEMS, static_cast<int>(gemini::modelChoice()),
                     [this](const int selected) {
                       switch (static_cast<gemini::ModelChoice>(selected)) {
                         case gemini::ModelChoice::Default:
                           gemini::writeModel("");
                           break;
                         case gemini::ModelChoice::Lite:
                           gemini::writeModel(gemini::LITE_MODEL);
                           break;
                         case gemini::ModelChoice::Custom:
                           editCustomModel();
                           break;
                       }
                     });
    requestUpdate();
  } else if (index == IDX_TARGET) {
    optionPopup.show(StrId::STR_TRANSLATE_TO, targetLabels.data(), static_cast<int>(targetLabels.size()),
                     static_cast<int>(SETTINGS.translationTarget), [this](const int selected) {
                       SETTINGS.translationTarget = static_cast<uint8_t>(selected);
                       SETTINGS.saveToFile();
                       if (selected == gemini::TARGET_CUSTOM) editCustomLanguage();
                       requestUpdate();
                     });
    requestUpdate();
  }
}

void TranslationSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  rowValues_[IDX_KEY] = gemini::maskedKey();
  if (rowValues_[IDX_KEY].empty()) rowValues_[IDX_KEY] = tr(STR_NOT_SET);
  const std::string model = gemini::readModel();
  rowValues_[IDX_MODEL] = model.empty() ? gemini::DEFAULT_MODEL : model;
  rowValues_[IDX_TARGET] = gemini::targetLanguageLabel(SETTINGS.translationTarget);
  for (int i = 0; i < MENU_ITEMS; i++) rowItems_[i].value = rowValues_[i].c_str();

  fui::ListProps props;
  props.items = rowItems_;
  props.count = static_cast<uint16_t>(MENU_ITEMS);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}

void TranslationSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}
