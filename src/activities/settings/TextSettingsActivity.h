#pragma once

#include <SdCardFontRegistry.h>

#include <cstdint>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "TextSettingsPreview.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "util/CjkScript.h"

// Reader text settings for one text profile, under a live preview: Font and Size rows, then the
// Layout and Style rows under their headings. Font and Size open a popup of their choices.
class TextSettingsActivity final : public UiListActivity {
 public:
  // One row of the family list. Public so the list-position helper in the .cpp can
  // take it: the displayed list hides the JP extension families, so a family's list
  // position no longer matches its registry index and has to be looked up by name.
  struct FontEntry {
    std::string name;  // the label for built-in rows, the family name for SD rows
    bool isBuiltin;
    uint8_t settingIndex;
    std::string label;  // shown instead of name when set
    const char* displayName() const { return label.empty() ? name.c_str() : label.c_str(); }
  };

  // profile: the profile these settings are (the title). In a book the live settings already hold
  // it; from Settings (editStoredProfile) it is swapped in on enter and back out on exit.
  // bookScript/verticalText: the open book's script and direction, which pick the preview face
  // and hide rows the book's layout does not read.
  TextSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const SdCardFontRegistry* registry,
                       CrossPointSettings::TextProfileId profile, CjkScript bookScript = CjkScript::None,
                       bool verticalText = false, bool editStoredProfile = false);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  // Not every row applies to every book: visibleLayoutRows() lists the ones shown.
  enum class LayoutRow {
    LineSpacing,
    WordSpacing,
    CharacterSpacing,
    ParaSpacing,
    ParaIndentation,
    Alignment,
    ScreenMargin,
    BookSideMargins,
    Count
  };
  enum class StyleRow { FocusReading, Hyphenation, EmbeddedStyle, AntiAliasing, Count };
  // The rows before Layout.
  static constexpr int FONT_ROW = 0;
  static constexpr int SIZE_ROW = 1;
  static constexpr int LAYOUT_FIRST_ROW = 2;

  // --- UiListActivity contract ---
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void drawChrome() override;
  void drawFooter() override;
  bool handleCustomInput() override;

  void applyFamily(int listIndex);
  void applySize(int listIndex);
  // Repopulates sizes_ (and currentSizeIndex_) from the active family's
  // installed point sizes. Call after any family change.
  void rebuildSizeList();
  void confirmLayoutRow(int row);
  void confirmStyleRow(int row);

  std::string layoutValueText(int row) const;
  // On/off rows draw a switch in place of their value. `checked` is only read when the row is one.
  bool layoutRowIsSwitch(int row, bool& checked) const;
  bool styleRowIsSwitch(int row, bool& checked) const;
  // Button-hint label for Confirm on the selected row.
  const char* confirmLabelText() const;
  // Maps a visible list position to its LayoutRow: vertical text hides most of them, so position
  // and enum value diverge.
  LayoutRow layoutRowAt(int visibleIndex) const;
  // Fills `out` (when non-null, room for LayoutRow::Count) with the Layout rows shown for this
  // book, in order, and returns how many.
  int visibleLayoutRows(LayoutRow* out) const;
  // CJK books hide FocusReading/Hyphenation.
  StyleRow styleRowAt(int visibleIndex) const;
  int styleRowCount() const { return cjkBook_ ? 2 : static_cast<int>(StyleRow::Count); }
  // Sentinel settingIndex for the "Manage Fonts" row appended to the family list. It opens
  // FontDownloadActivity instead of selecting a font, so "the font I want isn't here" is one press away.
  static constexpr uint8_t MANAGE_FONTS_ROW = 0xFF;
  bool isManageFontsRow(int row) const {
    return row >= 0 && row < static_cast<int>(fonts_.size()) && fonts_[row].settingIndex == MANAGE_FONTS_ROW;
  }
  // Rebuilds fonts_ (families installed on the card can change while this screen is open).
  void rebuildFamilyList();

  const CrossPointSettings::TextProfileId profile_;
  // Script of the book these settings apply to (the profile's script outside a book). CJK books
  // share the Japanese row set; the script also picks the preview face.
  const CjkScript bookScript_ = CjkScript::None;
  const bool cjkBook_ = false;
  const bool verticalText_ = false;
  const bool editStoredProfile_;
  // The live (Latin) values while another profile is swapped in for editing.
  bool swappedIn_ = false;
  CrossPointSettings::TextProfile latinWhileEditing_{};

  // Row storage: rowItems_ (label/actionValue/heading) is rebuilt only when its backing data
  // changes (rebuildRowItems()); rowValues_ holds the live per-row value text, refreshed every
  // buildScreen() by assigning into the existing strings.
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::string layoutHeading_;
  std::string styleHeading_;
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

  int currentFamilyIndex_ = 0;
  int currentSizeIndex_ = 0;

  ThemeMetrics metrics_ = {};
  int afterHeader = 0;
  int previewHeight = 0;
};

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
