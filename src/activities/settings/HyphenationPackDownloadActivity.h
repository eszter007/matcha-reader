#pragma once

#include <string>

#include "activities/CatalogActivity.h"

struct LanguageEntry;

// Downloads one language's .cphyph hyphenation pack. Opened from a book (bookPath set), the reader
// has already closed and this screen reopens the book; opened from Settings, it returns there.
class HyphenationPackDownloadActivity final : public CatalogActivity {
 public:
  HyphenationPackDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const LanguageEntry& language,
                                  std::string bookPath = {});

  void onEnter() override;

 private:
  const LanguageEntry& language;
  const std::string bookPath;

  int listCount() const override { return 0; }
  bool hasSearch() const override { return false; }
  void activateIndex(int) override {}
  void buildScreen(UiScreen& screen) override;
  bool handleCustomInput() override;
  void onBackButton() override { leave(); }
  void startBrowse() override;
  void performSearch(const std::string&) override {}
  void downloadFinished(bool cancelled) override;
  void restartAfterWifi() override;

  void leave();
};
