#pragma once

#include <I18n.h>

#include "activities/CatalogActivity.h"

// Downloads .cplang packs from the language-packs-<firmware version> release. With a target
// language it fetches that one pack and switches to it; without one it refreshes every pack on
// the card plus the selected language's, e.g. after a firmware update made them stale.
class LanguagePackDownloadActivity final : public CatalogActivity {
 public:
  LanguagePackDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                               Language target = Language::_COUNT);

  void onEnter() override;

  // True when "update" has anything to fetch.
  static bool hasInstalledPacks();

 private:
  Language target;
  bool switched = false;

  int listCount() const override { return 0; }
  bool hasSearch() const override { return false; }
  void activateIndex(int) override {}
  void buildScreen(UiScreen& screen) override;
  bool handleCustomInput() override;
  void onBackButton() override { finish(); }
  void startBrowse() override;
  void performSearch(const std::string&) override {}
  void downloadFinished(bool cancelled) override;

  HttpDownloader::DownloadError downloadPack(Language lang);
};
