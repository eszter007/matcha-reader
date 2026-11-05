#include "TextSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "FontDownloadActivity.h"
#include "MappedInputManager.h"
#include "ReaderFontSizes.h"
#include "ReaderLineSpacing.h"
#include "SdCardFontSystem.h"
#include "TextSettingsPreview.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/TextProfiles.h"

namespace fui = freeink::ui;

namespace {
// Indexed by LayoutRow / StyleRow; rebuildRowItems() asserts the lengths match.
constexpr StrId LAYOUT_ROW_NAME_IDS[] = {
    StrId::STR_LINE_SPACING,  StrId::STR_WORD_SPACING,          StrId::STR_CHARACTER_SPACING,
    StrId::STR_EXTRA_SPACING, StrId::STR_PARAGRAPH_INDENTATION, StrId::STR_ALIGNMENT,
    StrId::STR_SCREEN_MARGIN, StrId::STR_BOOK_CSS_MARGINS,      StrId::STR_TATEGAKI};
// Only Focus Reading shows in the preview; the other Style rows say so in their label.
constexpr StrId STYLE_ROW_NAME_IDS[] = {StrId::STR_FOCUS_READING, StrId::STR_HYPHENATION_NO_PREVIEW,
                                        StrId::STR_EMBEDDED_STYLE_NO_PREVIEW, StrId::STR_TEXT_AA_NO_PREVIEW};

// Position of the active family within the DISPLAYED list. The list hides the JP
// extension families, so a registry index is not a list index -- match by name.
int findCurrentFontIndex(const std::vector<TextSettingsActivity::FontEntry>& fonts, const char* sdFontFamilyName,
                         uint8_t fontFamily) {
  if (sdFontFamilyName[0] != '\0') {
    const auto match = std::find_if(fonts.begin(), fonts.end(), [sdFontFamilyName](const auto& font) {
      return !font.isBuiltin && font.name == sdFontFamilyName;
    });
    if (match != fonts.end()) return static_cast<int>(match - fonts.begin());
    // Selected family is hidden or gone (JP extension carried over from an older
    // build, or the card was swapped): fall through to the built-in entry.
  }

  const uint8_t builtin = fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? fontFamily : 0;
  const auto entry = std::find_if(fonts.begin(), fonts.end(), [builtin](const auto& font) {
    return font.isBuiltin && font.settingIndex == builtin;
  });
  return entry != fonts.end() ? static_cast<int>(entry - fonts.begin()) : 0;
}

constexpr StrId WORD_SPACING_IDS[] = {StrId::STR_SPACING_50_PERCENT,  StrId::STR_SPACING_75_PERCENT,
                                      StrId::STR_SPACING_100_PERCENT, StrId::STR_SPACING_125_PERCENT,
                                      StrId::STR_SPACING_150_PERCENT, StrId::STR_SPACING_175_PERCENT,
                                      StrId::STR_SPACING_200_PERCENT};
constexpr StrId CHARACTER_SPACING_IDS[] = {StrId::STR_SPACING_MINUS_2, StrId::STR_SPACING_MINUS_1,
                                           StrId::STR_SPACING_ZERO, StrId::STR_SPACING_PLUS_1,
                                           StrId::STR_SPACING_PLUS_2};
constexpr StrId VERTICAL_TEXT_IDS[] = {StrId::STR_VERTICAL_AUTOMATIC, StrId::STR_STATE_ON, StrId::STR_STATE_OFF};
constexpr StrId ALIGNMENT_IDS[] = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER, StrId::STR_ALIGN_RIGHT,
                                   StrId::STR_BOOK_S_STYLE};
constexpr int MARGIN_MIN = CrossPointSettings::SCREEN_MARGIN_MIN;
constexpr int MARGIN_MAX = CrossPointSettings::SCREEN_MARGIN_MAX;
constexpr int MARGIN_STEP = CrossPointSettings::SCREEN_MARGIN_STEP;
constexpr int WORD_SPACING_MIN = CrossPointSettings::WORD_SPACING_MIN;
constexpr int WORD_SPACING_MAX = CrossPointSettings::WORD_SPACING_MAX;
constexpr int WORD_SPACING_STEP = CrossPointSettings::WORD_SPACING_STEP;
static_assert(std::size(WORD_SPACING_IDS) == (WORD_SPACING_MAX - WORD_SPACING_MIN) / WORD_SPACING_STEP + 1);

// Heading text for the LAYOUT and STYLE sections: the label in capitals (ASCII only; scripts
// without case keep theirs).
std::string upperHeading(const char* label) {
  std::string out(label);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](const char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
  return out;
}

// Space between the preview box's border and the sample.
constexpr int PREVIEW_INNER_PADDING = 16;

// Latin for a UI language written in CJK, whose own sample would not be in the Latin profile's script.
// Not a UI string: a typographic sample.
constexpr const char* LATIN_SAMPLE = "The quick brown fox jumps over the lazy dog.";

// The preview sample in the edited profile's script.
const char* previewSample(const CrossPointSettings::TextProfileId profile, const CjkScript script) {
  switch (script) {
    case CjkScript::Japanese:
      return tr(STR_FONT_PREVIEW_TEXT_CJK);
    case CjkScript::SimplifiedChinese:
      return tr(STR_FONT_PREVIEW_TEXT_ZH_HANS);
    case CjkScript::TraditionalChinese:
      return tr(STR_FONT_PREVIEW_TEXT_ZH_HANT);
    case CjkScript::None:
      break;
  }
  if (profile == CrossPointSettings::TextProfileId::Korean) return tr(STR_FONT_PREVIEW_TEXT_KO);
  const Language ui = I18N.getLanguage();
  if (ui == Language::JA || ui == Language::ZHS || ui == Language::ZHT) return LATIN_SAMPLE;
  return tr(STR_FONT_PREVIEW_TEXT);
}
}  // namespace

TextSettingsActivity::TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const SdCardFontRegistry* registry,
                                           const CrossPointSettings::TextProfileId profile, const CjkScript bookScript,
                                           const bool verticalText, const bool editStoredProfile)
    : UiListActivity("TextSettings", renderer, mappedInput),
      profile_(profile),
      bookScript_(bookScript),
      cjkBook_(bookScript != CjkScript::None),
      verticalText_(verticalText),
      editStoredProfile_(editStoredProfile),
      registry_(registry) {}

void TextSettingsActivity::onEnter() {
  // From Settings, a profile other than Latin is edited in the live fields, as a book would read it.
  if (editStoredProfile_ && profile_ != CrossPointSettings::TextProfileId::Latin) {
    RenderLock lock;
    SETTINGS.holdLatinTextProfile(SETTINGS.liveTextProfile());
    SETTINGS.setLiveTextProfile(SETTINGS.textProfile(profile_));
    swappedIn_ = true;
    sdFontSystem.ensureLoaded(renderer);
  }
  UiListActivity::onEnter();

  metrics_ = UITheme::getInstance().getMetrics();
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int safeBottom = safeArea.y + safeArea.height;
  afterHeader = safeArea.y + metrics_.topPadding + metrics_.headerHeight + metrics_.verticalSpacing;
  const int usableHeight = safeBottom - afterHeader - metrics_.verticalSpacing;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;
  boxHeight_ = previewHeight;

  layoutHeading_ = upperHeading(tr(STR_LAYOUT));
  styleHeading_ = upperHeading(tr(STR_STYLE));
  rebuildFamilyList();
  rebuildSizeList();
  currentFamilyIndex_ = findCurrentFontIndex(fonts_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  rebuildRowItems();
}

// Leaving a swapped-in profile: its values go back to it and the Latin ones, which are the live
// settings everywhere else, come back.
void TextSettingsActivity::onExit() {
  if (swappedIn_) {
    RenderLock lock;
    auto edited = SETTINGS.liveTextProfile();
    // A font that failed to load is cleared from the live fields. That is not a choice, so the
    // profile keeps its font unless one was picked here.
    if (!familyChosen_) {
      const auto stored = SETTINGS.textProfile(profile_);
      edited.fontFamily = stored.fontFamily;
      memcpy(edited.sdFontFamilyName, stored.sdFontFamilyName, sizeof(edited.sdFontFamilyName));
    }
    SETTINGS.setTextProfile(profile_, edited);
    SETTINGS.setLiveTextProfile(SETTINGS.releaseLatinTextProfile());
    swappedIn_ = false;
    sdFontSystem.ensureLoaded(renderer);
  }
  UiListActivity::onExit();
}

void TextSettingsActivity::rebuildFamilyList() {
  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0) + 1);
  fonts_.push_back({I18N.get(StrId::STR_NOTO_SERIF), true, static_cast<uint8_t>(CrossPointSettings::NOTOSERIF)});
  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      // The CJK extension families (NotoSansJP/SC/TC and the serifs) are the CJK half of
      // the Noto Serif/Sans rows, not fonts in their own right: listing them would show
      // many Noto rows instead of two, and selecting one directly makes it the reader font
      // for Latin books too, bypassing the coverage-driven companion logic in
      // SdCardFontSystem::ensureCjkFallback(). One sans cut stands in for a missing NotoSans
      // family so the Noto Sans row stays -- see isHiddenCjkExtension.
      if (SdCardFontSystem::isHiddenCjkExtension(families[i].name, registry_)) continue;
      // Same reasoning for a wider-coverage cut of a family already on this list
      // (NotoSerifExtended over Noto Serif): one typeface is one row, and the base row's
      // selection resolves to whichever of the two suits the book being opened.
      if (SdCardFontSystem::isCoverageVariant(families[i].name, registry_)) continue;
      // settingIndex stays the REGISTRY index (what sdFontFamilyName resolves against);
      // the list position is separate now that the list is filtered.
      fonts_.push_back({families[i].name, false, static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i),
                        SdCardFontSystem::pickerLabel(families[i].name, registry_)});
    }
  }
  // Built-in and SD families sort together by the label shown; Manage Fonts stays last.
  std::sort(fonts_.begin(), fonts_.end(),
            [](const FontEntry& a, const FontEntry& b) { return strcasecmp(a.displayName(), b.displayName()) < 0; });
  fonts_.push_back({I18N.get(StrId::STR_MANAGE_FONTS), false, MANAGE_FONTS_ROW});
}

// Structural: call only when its backing data (fonts_/sizes_) changes, never from
// buildScreen(), which just refreshes rowValues_ in place.
void TextSettingsActivity::rebuildRowItems() {
  static_assert(std::size(LAYOUT_ROW_NAME_IDS) == static_cast<size_t>(LayoutRow::Count),
                "LAYOUT_ROW_NAME_IDS must have one label per LayoutRow");
  static_assert(std::size(STYLE_ROW_NAME_IDS) == static_cast<size_t>(StyleRow::Count),
                "STYLE_ROW_NAME_IDS must have one label per StyleRow");
  const int count = listCount();
  rowValues_.assign(count, std::string());
  rowItems_.clear();
  rowItems_.reserve(count);
  const int layoutCount = visibleLayoutRows(nullptr);
  for (int i = 0; i < count; i++) {
    fui::ListItem item;
    item.actionValue = static_cast<int16_t>(i);
    if (i == FONT_ROW) {
      item.label = tr(STR_FONT);
    } else if (i == SIZE_ROW) {
      item.label = tr(STR_SIZE);
    } else if (i < LAYOUT_FIRST_ROW + layoutCount) {
      // Visible position -> LayoutRow: not every row is shown for every book.
      const int layoutIndex = i - LAYOUT_FIRST_ROW;
      const LayoutRow row = layoutRowAt(layoutIndex);
      item.label = row == LayoutRow::VerticalText && profile_ != CrossPointSettings::TextProfileId::Japanese
                       ? tr(STR_ZHIPAI)
                       : I18N.get(LAYOUT_ROW_NAME_IDS[static_cast<int>(row)]);
      if (layoutIndex == 0) item.sectionHeading = layoutHeading_.c_str();
    } else {
      const int styleIndex = i - LAYOUT_FIRST_ROW - layoutCount;
      item.label = I18N.get(STYLE_ROW_NAME_IDS[static_cast<int>(styleRowAt(styleIndex))]);
      if (styleIndex == 0) item.sectionHeading = styleHeading_.c_str();
    }
    rowItems_.push_back(item);
  }
}

// The selectable sizes belong to the active family (in a book, to the cut that book renders
// with), so this runs on entry and again after every family change. A family change goes through ensureLoaded(),
// which snaps SETTINGS.fontPointSize into the new family's set — but entry does
// not, so the highlight is resolved by snapping rather than by exact match.
void TextSettingsActivity::rebuildSizeList() {
  const std::vector<uint8_t> points = SdCardFontSystem::selectablePointSizes(registry_, bookScript_);

  // The stored size can still sit outside this family's set — e.g. the family
  // was deleted while selected, or the card was swapped. Highlight the size the
  // reader actually renders, which getReaderFontId() resolves the same way.
  const uint8_t selectedPt = snapToNearestPointSize(points, SETTINGS.fontPointSize);

  sizes_.clear();
  sizes_.reserve(points.size());
  currentSizeIndex_ = 0;
  for (const uint8_t pt : points) {
    // "pt" is deliberately not translated: it is the typographic unit symbol,
    // written the same way in every language CrossPoint ships.
    char label[12];
    snprintf(label, sizeof(label), "%u pt", pt);
    if (pt == selectedPt) currentSizeIndex_ = static_cast<int>(sizes_.size());
    sizes_.push_back({label, pt});
  }
}

bool TextSettingsActivity::handleCustomInput() {
  return optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); });
}

void TextSettingsActivity::activateIndex(const int index) {
  if (optionPopup_.isActive()) return;
  // Most rows repaint a different surface (popup, preview, new value);
  // a lingering tap flash would gray an unrelated element.
  app.clearTapFlash();
  if (index == FONT_ROW) {
    std::vector<std::string> options;
    options.reserve(fonts_.size());
    std::transform(fonts_.begin(), fonts_.end(), std::back_inserter(options),
                   [](const FontEntry& font) { return std::string(font.displayName()); });
    optionPopup_.show(StrId::STR_FONT, std::move(options), currentFamilyIndex_, [this](const int idx) {
      if (isManageFontsRow(idx)) {
        startActivityForResult(
            std::make_unique<FontDownloadActivity>(renderer, mappedInput), [this](const ActivityResult&) {
              // Fonts may have been installed or deleted while we were away.
              rebuildFamilyList();
              currentFamilyIndex_ = findCurrentFontIndex(fonts_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
              rebuildSizeList();
              requestUpdate();
            });
      } else if (idx != currentFamilyIndex_) {
        applyFamily(idx);
      }
    });
    requestUpdate();
  } else if (index == SIZE_ROW) {
    std::vector<std::string> options;
    options.reserve(sizes_.size());
    std::transform(sizes_.begin(), sizes_.end(), std::back_inserter(options),
                   [](const SizeEntry& size) { return size.name; });
    optionPopup_.show(StrId::STR_SIZE, std::move(options), currentSizeIndex_, [this](const int idx) {
      if (idx != currentSizeIndex_) applySize(idx);
    });
    requestUpdate();
  } else if (index < LAYOUT_FIRST_ROW + visibleLayoutRows(nullptr)) {
    confirmLayoutRow(static_cast<int>(layoutRowAt(index - LAYOUT_FIRST_ROW)));
  } else {
    confirmStyleRow(static_cast<int>(styleRowAt(index - LAYOUT_FIRST_ROW - visibleLayoutRows(nullptr))));
  }
}

void TextSettingsActivity::buildScreen(UiScreen& screen) {
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  // drawChrome() runs first in every render and sizes the box.
  const int listTop = afterHeader + boxHeight_ + metrics_.verticalSpacing;
  const int rightInset = renderer.getScreenWidth() - (safeArea.x + safeArea.width);
  const int bottomInset = renderer.getScreenHeight() - (safeArea.y + safeArea.height);
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(listTop), static_cast<int16_t>(rightInset),
                                                static_cast<int16_t>(bottomInset), static_cast<int16_t>(safeArea.x)});

  // rowItems_ was built by rebuildRowItems(); only the live value text is refreshed here, by
  // assigning into the existing rowValues_ strings (no vector growth).
  const int count = listCount();
  const int layoutCount = visibleLayoutRows(nullptr);
  for (int i = 0; i < count; i++) {
    bool checked = false;
    rowItems_[i].toggle = false;
    rowValues_[i].clear();
    if (i == FONT_ROW) {
      if (currentFamilyIndex_ < static_cast<int>(fonts_.size())) {
        rowValues_[i] = fonts_[currentFamilyIndex_].displayName();
      }
    } else if (i == SIZE_ROW) {
      if (currentSizeIndex_ < static_cast<int>(sizes_.size())) rowValues_[i] = sizes_[currentSizeIndex_].name;
    } else if (i < LAYOUT_FIRST_ROW + layoutCount) {
      const int layoutRow = static_cast<int>(layoutRowAt(i - LAYOUT_FIRST_ROW));
      rowItems_[i].toggle = layoutRowIsSwitch(layoutRow, checked);
      if (!rowItems_[i].toggle) rowValues_[i] = layoutValueText(layoutRow);
    } else {
      rowItems_[i].toggle = styleRowIsSwitch(static_cast<int>(styleRowAt(i - LAYOUT_FIRST_ROW - layoutCount)), checked);
    }
    if (rowItems_[i].toggle) rowItems_[i].toggleChecked = checked;
    rowItems_[i].value = rowValues_[i].empty() ? nullptr : rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  applySwitchStyle(props);
  // Titles match the value's font size (smallText) so both sides of a row
  // read as one unit; labels that still don't fit wrap onto a second line.
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  props.headerText = screen.theme().smallText;
  props.headerText.font = fui::GfxRendererTarget::FONT_LABEL;  // the 8 pt UI font
  props.headerUnderline = 0;
  // The heading sits on its first row (no band under it) with room above, so it reads as that row's.
  props.headerRowHeight = static_cast<int16_t>(screen.target().lineHeight(props.headerText.font));
  props.sectionGap = 28;
  syncListViewport(screen, props);
  screen.list(props);
}

const char* TextSettingsActivity::confirmLabelText() const {
  const int index = nav.selected;
  const int layoutCount = visibleLayoutRows(nullptr);
  if (index >= LAYOUT_FIRST_ROW + layoutCount) return tr(STR_TOGGLE);
  if (index >= LAYOUT_FIRST_ROW) {
    // The two booleans toggle in place; every other row opens a picker.
    const LayoutRow row = layoutRowAt(index - LAYOUT_FIRST_ROW);
    if (row == LayoutRow::ParaSpacing || row == LayoutRow::BookSideMargins) return tr(STR_TOGGLE);
  }
  return tr(STR_SELECT);
}

void TextSettingsActivity::drawChrome() {
  GUI.drawHeader(renderer, UITheme::getInstance().getHeaderRect(renderer, true, false), textprofile::title(profile_));

  // The two inputs are independent: a Japanese book read horizontally is (Japanese, false). A
  // forced-vertical book with no CJK tag previews in the Japanese face, the face the page renders with.
  const CjkScript previewScript =
      bookScript_ != CjkScript::None ? bookScript_ : (verticalText_ ? CjkScript::Japanese : CjkScript::None);
  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int side = metrics_.contentSidePadding;
  // previewHeight is the most the box may take; it shrinks to the lines the sample fills.
  const Rect box{safeArea.x + side, afterHeader, safeArea.width - 2 * side, previewHeight};
  // Inside the 1px border, so the sample cannot draw over it.
  const Rect previewBounds{box.x + 1, box.y + 1, box.width - 2, box.height - 2};
  const auto [savedX, savedY, savedWidth, savedHeight] = renderer.getClipRect();
  const int clipLeft = std::max(previewBounds.x, savedX);
  const int clipTop = std::max(previewBounds.y, savedY);
  const int clipRight = std::min(previewBounds.x + previewBounds.width, savedX + savedWidth);
  const int clipBottom = std::min(previewBounds.y + previewBounds.height, savedY + savedHeight);
  renderer.setClipRect(clipLeft, clipTop, std::max(0, clipRight - clipLeft), std::max(0, clipBottom - clipTop));
  const int textBottom = textsettings::renderPreview(
      renderer, previewLayout_, PREVIEW_INNER_PADDING, metrics_.verticalSpacing, previewBounds, nullptr,
      sdFontSystem.effectiveReaderFontId(previewScript), previewSample(profile_, previewScript));
  renderer.setClipRect(savedX, savedY, savedWidth, savedHeight);
  boxHeight_ = std::min(previewHeight, textBottom + PREVIEW_INNER_PADDING + 1 - box.y);
  renderer.drawRect(box.x, box.y, box.width, boxHeight_, true);
}

// Button hints live here rather than at the end of drawChrome(): UiListActivity draws the footer
// separately, so the hints repaint without redrawing the preview.
void TextSettingsActivity::drawFooter() {
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabelText(), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TextSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;  // picker draws over everything
  UiListActivity::render(std::move(lock));
}

// Font switching runs on the main task from loop(), which deliberately holds no
// RenderLock. ensureLoaded() deletes the resident SdCardFont before loading the
// next one, and the render task walks that same object inside the preview's
// prewarmCache() — so without this lock a font switch can free the mini glyph
// arrays out from under prewarmStyle() (crash: null s.miniGlyphs mid-read/sort).
void TextSettingsActivity::applyFamily(int listIndex) {
  RenderLock lock;
  const auto& font = fonts_[listIndex];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
    sdFontSystem.ensureLoaded(renderer);  // unloads the previously resident SD font
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer);
    }
  }
  // A failed load clears the selection, so read back what is loaded rather than assume the pick.
  currentFamilyIndex_ = findCurrentFontIndex(fonts_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  if (currentFamilyIndex_ != listIndex) {
    rebuildSizeList();
    return;
  }
  familyChosen_ = true;

  // The new family ships its own set of point sizes, and ensureLoaded() may have
  // snapped the selection into it.
  rebuildSizeList();
}

// Same RenderLock rationale as applyFamily(): a size change reloads the SD font
// file, which frees and replaces the SdCardFont the render task may be reading.
void TextSettingsActivity::applySize(int listIndex) {
  RenderLock lock;

  currentSizeIndex_ = listIndex;
  SETTINGS.fontPointSize = sizes_[listIndex].pointSize;
  sdFontSystem.ensureLoaded(renderer);
}

void TextSettingsActivity::confirmLayoutRow(int row) {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::ParaSpacing:
      SETTINGS.extraParagraphSpacing = !SETTINGS.extraParagraphSpacing;
      requestUpdate();
      break;
    case LayoutRow::BookSideMargins:
      SETTINGS.bookCssMargins = !SETTINGS.bookCssMargins;
      requestUpdate();
      break;
    case LayoutRow::ParaIndentation: {
      std::vector<std::string> options;
      options.reserve(6);
      options.push_back(tr(STR_STATE_OFF));
      for (int spaces = 1; spaces <= 5; ++spaces) options.push_back(std::to_string(spaces));
      optionPopup_.show(StrId::STR_PARAGRAPH_INDENTATION, options, SETTINGS.paragraphIndentSpaces, [](int idx) {
        SETTINGS.paragraphIndentSpaces = static_cast<uint8_t>(idx);
        SETTINGS.saveToFile();
      });
      requestUpdate();
      break;
    }
    case LayoutRow::LineSpacing:
      optionPopup_.show(StrId::STR_LINE_SPACING, LINE_SPACING_IDS, static_cast<int>(std::size(LINE_SPACING_IDS)),
                        SETTINGS.lineSpacing, [](int idx) { SETTINGS.lineSpacing = static_cast<uint8_t>(idx); });
      requestUpdate();
      break;
    case LayoutRow::Alignment:
      optionPopup_.show(StrId::STR_ALIGNMENT, ALIGNMENT_IDS, static_cast<int>(std::size(ALIGNMENT_IDS)),
                        SETTINGS.paragraphAlignment,
                        [](int idx) { SETTINGS.paragraphAlignment = static_cast<uint8_t>(idx); });
      requestUpdate();
      break;
    case LayoutRow::WordSpacing: {
      const int cur = (std::clamp<int>(SETTINGS.wordSpacing, WORD_SPACING_MIN, WORD_SPACING_MAX) - WORD_SPACING_MIN) /
                      WORD_SPACING_STEP;
      optionPopup_.show(StrId::STR_WORD_SPACING, WORD_SPACING_IDS, static_cast<int>(std::size(WORD_SPACING_IDS)), cur,
                        [](int idx) {
                          SETTINGS.wordSpacing = static_cast<uint8_t>(WORD_SPACING_MIN + idx * WORD_SPACING_STEP);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    }
    case LayoutRow::CharacterSpacing:
      optionPopup_.show(StrId::STR_CHARACTER_SPACING, CHARACTER_SPACING_IDS,
                        static_cast<int>(std::size(CHARACTER_SPACING_IDS)), SETTINGS.characterSpacing, [](int idx) {
                          SETTINGS.characterSpacing = static_cast<uint8_t>(idx);
                          SETTINGS.saveToFile();
                        });
      requestUpdate();
      break;
    case LayoutRow::VerticalText: {
      uint8_t* setting = verticalTextSetting();
      if (!setting) break;
      const StrId title =
          profile_ == CrossPointSettings::TextProfileId::Japanese ? StrId::STR_TATEGAKI : StrId::STR_ZHIPAI;
      optionPopup_.show(title, VERTICAL_TEXT_IDS, static_cast<int>(std::size(VERTICAL_TEXT_IDS)), *setting,
                        [setting](int idx) { *setting = static_cast<uint8_t>(idx); });
      requestUpdate();
      break;
    }
    case LayoutRow::ScreenMargin: {
      std::vector<std::string> options;
      options.reserve((MARGIN_MAX - MARGIN_MIN) / MARGIN_STEP + 1);
      for (int m = MARGIN_MIN; m <= MARGIN_MAX; m += MARGIN_STEP) options.push_back(std::to_string(m));
      const int cur = (std::clamp<int>(SETTINGS.screenMargin, MARGIN_MIN, MARGIN_MAX) - MARGIN_MIN) / MARGIN_STEP;
      optionPopup_.show(StrId::STR_SCREEN_MARGIN, options, cur,
                        [](int idx) { SETTINGS.screenMargin = static_cast<uint8_t>(MARGIN_MIN + idx * MARGIN_STEP); });
      requestUpdate();
      break;
    }

    default:
      break;
  }
}

bool TextSettingsActivity::layoutRowIsSwitch(const int row, bool& checked) const {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::ParaSpacing:
      checked = SETTINGS.extraParagraphSpacing != 0;
      return true;
    case LayoutRow::BookSideMargins:
      checked = SETTINGS.bookCssMargins != 0;
      return true;
    default:
      return false;
  }
}

bool TextSettingsActivity::styleRowIsSwitch(const int row, bool& checked) const {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      checked = SETTINGS.focusReadingEnabled != 0;
      return true;
    case StyleRow::Hyphenation:
      checked = SETTINGS.hyphenationEnabled != 0;
      return true;
    case StyleRow::EmbeddedStyle:
      checked = SETTINGS.embeddedStyle != 0;
      return true;
    case StyleRow::AntiAliasing:
      checked = SETTINGS.textAntiAliasing != 0;
      return true;
    default:
      return false;
  }
}

std::string TextSettingsActivity::layoutValueText(int row) const {
  switch (static_cast<LayoutRow>(row)) {
    case LayoutRow::LineSpacing: {
      const uint8_t v = SETTINGS.lineSpacing;
      return I18N.get(LINE_SPACING_IDS[v < std::size(LINE_SPACING_IDS) ? v : CrossPointSettings::LINE_SPACING_120]);
    }
    case LayoutRow::ParaIndentation: {
      const int spaces = std::clamp<int>(SETTINGS.paragraphIndentSpaces, 0, 5);
      return spaces == 0 ? tr(STR_STATE_OFF) : std::to_string(spaces);
    }
    case LayoutRow::Alignment: {
      const uint8_t v = SETTINGS.paragraphAlignment;
      return v < std::size(ALIGNMENT_IDS) ? I18N.get(ALIGNMENT_IDS[v]) : I18N.get(StrId::STR_JUSTIFY);
    }
    case LayoutRow::WordSpacing:
      return std::to_string(SETTINGS.wordSpacing) + "%";
    case LayoutRow::CharacterSpacing: {
      const uint8_t v = SETTINGS.characterSpacing;
      return v < std::size(CHARACTER_SPACING_IDS) ? I18N.get(CHARACTER_SPACING_IDS[v])
                                                  : I18N.get(StrId::STR_SPACING_ZERO);
    }
    case LayoutRow::ScreenMargin:
      return std::to_string(SETTINGS.screenMargin);
    case LayoutRow::VerticalText: {
      const uint8_t* setting = verticalTextSetting();
      const uint8_t v = setting ? *setting : 0;
      return I18N.get(VERTICAL_TEXT_IDS[v < std::size(VERTICAL_TEXT_IDS) ? v : 0]);
    }

    // ParaSpacing and BookSideMargins draw switches -- see layoutRowIsSwitch().
    default:
      return "";
  }
}

void TextSettingsActivity::confirmStyleRow(int row) {
  switch (static_cast<StyleRow>(row)) {
    case StyleRow::FocusReading:
      SETTINGS.focusReadingEnabled = !SETTINGS.focusReadingEnabled;
      break;
    case StyleRow::Hyphenation:
      SETTINGS.hyphenationEnabled = !SETTINGS.hyphenationEnabled;
      break;
    case StyleRow::EmbeddedStyle:
      SETTINGS.embeddedStyle = !SETTINGS.embeddedStyle;
      break;
    case StyleRow::AntiAliasing:
      SETTINGS.textAntiAliasing = !SETTINGS.textAntiAliasing;
      break;

    default:
      return;
  }
  requestUpdate();
}

int TextSettingsActivity::listCount() const { return LAYOUT_FIRST_ROW + visibleLayoutRows(nullptr) + styleRowCount(); }

// Only the rows the book's layout engine reads, so a hidden row never keeps changing the page.
// Vertical text uses line spacing, character spacing (the step down a column) and the screen
// margin; horizontal text -- Japanese included -- applies every row.
int TextSettingsActivity::visibleLayoutRows(LayoutRow* out) const {
  static constexpr LayoutRow VERTICAL[] = {LayoutRow::LineSpacing, LayoutRow::CharacterSpacing,
                                           LayoutRow::ScreenMargin};
  int count = 0;
  const auto add = [&](const LayoutRow row) {
    if (out) out[count] = row;
    count++;
  };
  if (verticalTextSetting()) add(LayoutRow::VerticalText);
  if (verticalText_) {
    for (const LayoutRow row : VERTICAL) add(row);
  } else {
    for (int i = 0; i < static_cast<int>(LayoutRow::VerticalText); i++) add(static_cast<LayoutRow>(i));
  }
  return count;
}

// Only from Settings: in a book, a change of direction would need the open chapter laid out again.
uint8_t* TextSettingsActivity::verticalTextSetting() const {
  if (!editStoredProfile_) return nullptr;
  if (profile_ == CrossPointSettings::TextProfileId::Japanese) return &SETTINGS.tategaki;
  if (profile_ == CrossPointSettings::TextProfileId::TraditionalChinese) return &SETTINGS.zhipai;
  if (profile_ == CrossPointSettings::TextProfileId::SimplifiedChinese) return &SETTINGS.zhipaiSimplified;
  return nullptr;
}

TextSettingsActivity::LayoutRow TextSettingsActivity::layoutRowAt(const int visibleIndex) const {
  LayoutRow rows[static_cast<int>(LayoutRow::Count)];
  const int count = visibleLayoutRows(rows);
  return visibleIndex >= 0 && visibleIndex < count ? rows[visibleIndex] : LayoutRow::Count;
}

TextSettingsActivity::StyleRow TextSettingsActivity::styleRowAt(const int visibleIndex) const {
  if (!cjkBook_) return static_cast<StyleRow>(visibleIndex);
  return visibleIndex == 0 ? StyleRow::EmbeddedStyle : StyleRow::AntiAliasing;
}
