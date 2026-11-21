#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"

struct LanguageEntry;

// Settings > Reader > Hyphenation languages: every language whose rules are a download, with
// Download or Installed. Selecting an installed one offers to remove it.
class HyphenationLanguagesActivity final : public UiListActivity {
 public:
  HyphenationLanguagesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;

 private:
  struct Row {
    const LanguageEntry* language;
    std::string name;
    bool installed;
  };
  std::vector<Row> rows;
  std::vector<freeink::ui::ListItem> items;

  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  void rebuildRows();
  void confirmRemove(const Row& row);
};
