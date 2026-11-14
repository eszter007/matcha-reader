#pragma once

#include <vector>

#include "activities/UiListActivity.h"
#include "components/HomeTabBar.h"
#include "components/OptionPopup.h"

/**
 * Activity showing the list of configured OPDS servers.
 * Allows adding new servers and editing/deleting existing ones.
 * When pickerMode is true, selecting a server navigates to the OPDS browser
 * instead of opening the editor (used from File Transfer and the Home menu).
 */
class OpdsServerListActivity final : public UiListActivity {
 public:
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  int listCount() const override { return getItemCount(); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  // Popup input goes first; while it is open it consumes the pass.
  bool handleCustomInput() override;
  // Picker mode backs out to the home menu rather than finishing.
  void onBackButton() override;
  const char* headerTitle() const override;

  // The picker is part of File Transfer in the Cover Grid theme (its OPDS Browser row opens it),
  // so it keeps the bottom bar with that tab marked.
  bool inTransferTab() const { return pickerMode && HomeTabBar::enabled(); }
  HomeTab tabBarTab() const override { return inTransferTab() ? HomeTab::Transfer : HomeTab::Count; }
  // The marked tab is a destination from here too: File Transfer's first screen.
  void ringActivateBottomTab(HomeTab tab) override { HomeTabBar::activate(tab, HomeTab::Count); }

  bool pickerMode = false;
  OptionPopup optionPopup;

  // Row structure (labels, actionValue; server subtitles), rebuilt only when
  // the server list itself reloads (rebuildRowItems(), called from onEnter()
  // and after returning from the server editor) — not on every repaint. The
  // folder/format rows' live subtitle is refreshed in place by buildScreen().
  std::vector<freeink::ui::ListItem> rowItems_;
  void rebuildRowItems();

  int getItemCount() const;
  void handleSelection();
};
