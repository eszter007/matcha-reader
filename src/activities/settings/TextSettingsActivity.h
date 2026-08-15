#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "TextSettingsPreview.h"
#include "activities/UiTabListActivity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"

// Reader text settings with a shared live preview pane: tab bar
// (Font | Size | Layout | Style) is position 0 of the Up/Down nav ring, same
// idiom as SettingsActivity. Family/Size rows apply on Confirm; Layout/Style
// rows toggle or open an OptionPopup picker. (Tab::Family/Style are the enum
// names for the Font/Style tabs.)
class TextSettingsActivity final : public UiTabListActivity {
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
                       Tab initialTab = Tab::Family, bool japaneseBook = false);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  // Row indices per tab. enum class (not plain enum) so a LayoutRow can't be
  // silently confused with a StyleRow of equal value.
  // BookSideMargins is horizontal-only (the vertical engine never reads honorBookInsets), so it
  // is hidden for a Japanese book alongside ParaSpacing and Alignment -- see layoutRowAt().
  enum class LayoutRow { LineSpacing, ParaSpacing, Alignment, ScreenMargin, BookSideMargins, Count };
  enum class StyleRow { FocusReading, Hyphenation, EmbeddedStyle, AntiAliasing, Count };

  // --- UiTabListActivity contract ---
  int listCount() const override;
  int tabCount() const override;
  int activeTab() const override { return static_cast<int>(tab_); }
  const char* tabLabel(int index) const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onTabAction(int index) override;
  void stepTab(int direction) override { switchTab(direction); }
  bool handleButtons() override;
  bool handleCustomInput() override;

  void applyFamily(int listIndex);
  void applySize(int listIndex);
  // Repopulates sizes_ (and currentSizeIndex_) from the active family's
  // installed point sizes. Call after any family change.
  void rebuildSizeList();
  void confirmLayoutRow(int row);
  void confirmStyleRow(int row);
  // Applies the row at the given list index for the active tab (Confirm and tap share this).
  void activateRow(int row);

  std::string layoutValueText(int row) const;
  // On/off rows draw a switch in place of their value. `checked` is only read when the row is
  // one; `row` is the visible index, so Layout goes through layoutRowAt() like its value text.
  bool layoutRowIsSwitch(int row, bool& checked) const;
  bool styleRowIsSwitch(int row, bool& checked) const;
  // Button-hint label for Confirm at the current ring position.
  const char* confirmLabelText() const;
  // True when the focused list row is a setting the preview cannot reflect.
  bool focusedRowHasNoPreview() const;
  void switchTab(int direction = 1);
  // Maps a visible list position to its LayoutRow: a Japanese book hides ParaSpacing,
  // Alignment and BookSideMargins, so position and enum value diverge.
  LayoutRow layoutRowAt(int visibleIndex) const;
  // Sentinel settingIndex for the "Manage Fonts" row appended to the family list. It opens
  // FontDownloadActivity instead of selecting a font -- the shortcut the pre-1.5.0 font
  // picker had at the bottom of its list, so "the font I want isn't here" is one press away.
  static constexpr uint8_t MANAGE_FONTS_ROW = 0xFF;
  bool isManageFontsRow(int row) const {
    return row >= 0 && row < static_cast<int>(fonts_.size()) && fonts_[row].settingIndex == MANAGE_FONTS_ROW;
  }
  // Rebuilds fonts_ (families installed on the card can change while this screen is open).
  void rebuildFamilyList();
  const bool japaneseBook_ = false;

  // Row storage for the active tab: rowItems_ (label/actionValue) is
  // rebuilt only when the tab or its backing data changes (rebuildRowItems(),
  // called from onEnter()/onTabAction()/switchTab()); rowValues_ holds the
  // live per-row value text, refreshed every buildScreen() call by assigning
  // into the existing strings (no vector growth), so steady-state rendering
  // never allocates/frees row storage.
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  void rebuildRowItems();

  struct SizeEntry {
    std::string name;  // the point size, rendered for display ("14 pt")
    uint8_t pointSize;
  };

  const SdCardFontRegistry* registry_;
  OptionPopup optionPopup_;
  std::vector<FontEntry> fonts_;
  std::vector<SizeEntry> sizes_;
  textsettings::PreviewLayout previewLayout_;  // cached preview line layout; relaid only on setting/geometry change

  Tab tab_;
  int currentFamilyIndex_ = 0;
  int currentSizeIndex_ = 0;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int bottomReserved = 0;
  int usableHeight = 0;
  int previewHeight = 0;
};
