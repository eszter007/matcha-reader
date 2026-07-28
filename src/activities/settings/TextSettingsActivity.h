#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "TextSettingsPreview.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

// Reader text settings with a shared live preview pane: tab bar
// (Font | Size | Layout | Style) is position 0 of the Up/Down nav ring, same
// idiom as SettingsActivity. Family/Size rows apply on Confirm; Layout/Style
// rows toggle or open an OptionPopup picker. (Tab::Family/Style are the enum
// names for the Font/Style tabs.)
class TextSettingsActivity final : public Activity {
 public:
  enum class Tab : uint8_t { Family, Size, Layout, Style, Count };

  // One row of the family list. Public so the list-position helper in the .cpp can
  // take it: the displayed list hides the JP extension families, so a family's list
  // position no longer matches its registry index and has to be looked up by name.
  struct FontEntry {
    std::string name;
    bool isBuiltin;
    uint8_t settingIndex;
  };

  TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SdCardFontRegistry* registry,
                       Tab initialTab = Tab::Family);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Row indices per tab. enum class (not plain enum) so a LayoutRow can't be
  // silently confused with a StyleRow of equal value.
  enum class LayoutRow { LineSpacing, ParaSpacing, Alignment, ScreenMargin, Count };
  enum class StyleRow { FocusReading, Hyphenation, EmbeddedStyle, AntiAliasing, Count };

  void applyFamily(int listIndex);
  void applySize(int listIndex);
  // Repopulates sizes_ (and currentSizeIndex_) from the active family's
  // installed point sizes. Call after any family change.
  void rebuildSizeList();
  void confirmLayoutRow(int row);
  void confirmStyleRow(int row);
  // Applies the row at the given list index for the active tab (Confirm and tap share this).
  void activateRow(int row);

  // Handles tab/list/swipe touch input; returns true if an event was consumed (caller returns).
  bool handleTouch();

  // Vertical layout of the preview/tab-bar/list panes.
  // Shared by render() (to draw) and loop() (to hit-test touch) to avoid drift
  struct PaneGeometry {
    int previewTop;
    int tabTop;
    int listTop;
    int listHeight;
  };
  PaneGeometry paneGeometry() const;
  std::string layoutValueText(int row) const;
  std::string styleValueText(int row) const;
  // True when the focused list row is a setting the preview cannot reflect.
  bool focusedRowHasNoPreview() const;
  void switchTab(int direction = 1);
  int currentListSize() const;
  // Navigation ring position for the active tab: 0 = tab bar, 1..N = list item N-1.
  int& selectedIndex();
  int selectedIndex() const;

  // Sentinel settingIndex for the "Manage Fonts" row appended to the family list. It opens
  // FontDownloadActivity instead of selecting a font -- the shortcut the pre-1.5.0 font
  // picker had at the bottom of its list, so "the font I want isn't here" is one press away.
  static constexpr uint8_t MANAGE_FONTS_ROW = 0xFF;
  bool isManageFontsRow(int row) const {
    return row >= 0 && row < static_cast<int>(fonts_.size()) && fonts_[row].settingIndex == MANAGE_FONTS_ROW;
  }
  // Rebuilds fonts_ (families installed on the card can change while this screen is open).
  void rebuildFamilyList();

  struct SizeEntry {
    std::string name;  // the point size, rendered for display ("14 pt")
    uint8_t pointSize;
  };

  const SdCardFontRegistry* registry_;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  std::vector<FontEntry> fonts_;
  std::vector<SizeEntry> sizes_;
  textsettings::PreviewLayout previewLayout_;  // cached preview line layout; relaid only on setting/geometry change

  Tab tab_;
  int selectedIndex_[static_cast<int>(Tab::Count)] =
      {};  // per-Tab nav position (0 = tab bar, 1..N = row); set in onEnter
  int currentFamilyIndex_ = 0;
  int currentSizeIndex_ = 0;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
