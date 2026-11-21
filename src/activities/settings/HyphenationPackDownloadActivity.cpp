#include "HyphenationPackDownloadActivity.h"

#include <Arduino.h>
#include <Epub/hyphenation/HyphenationPack.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/hyphenation/LanguageRegistry.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointState.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/settings/SettingsSection.h"
#include "components/CatalogScreens.h"
#include "util/HyphenationPacks.h"

// One release per pack format, not per firmware: a pack stays valid across firmware updates.
#define HYPHENATION_STRINGIFY_(x) #x
#define HYPHENATION_STRINGIFY(x) HYPHENATION_STRINGIFY_(x)
#ifndef HYPHENATION_PACKS_URL
#define HYPHENATION_PACKS_URL                                                                               \
  "https://github.com/eszter007/matcha-reader-assets/releases/download/hyphenation-" HYPHENATION_STRINGIFY( \
      HYPHENATION_PACK_FORMAT_VERSION) "/"
#endif

namespace {
// Same floors as the language pack download; downloadFile() checks again for TLS.
constexpr size_t MIN_FREE_HEAP_BEFORE_WIFI = 48 * 1024;
constexpr size_t MIN_MAX_ALLOC_BEFORE_WIFI = 12 * 1024;
}  // namespace

HyphenationPackDownloadActivity::HyphenationPackDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                                 const LanguageEntry& language, std::string bookPath)
    : CatalogActivity("HyphenationPackDownload", renderer, mappedInput),
      language(language),
      bookPath(std::move(bookPath)) {}

void HyphenationPackDownloadActivity::onEnter() {
  CatalogActivity::onEnter();
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));

  // Wi-Fi cannot start beside the resident SD fonts on the C3. Leaving after Wi-Fi restarts the
  // device, which loads them again.
  {
    RenderLock lock(*this);
    sdFontSystem.releaseAllResidentFonts(renderer);
  }
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_BEFORE_WIFI || ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_BEFORE_WIFI) {
    LOG_ERR("HYPH", "Low heap before Wi-Fi (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    {
      RenderLock lock(*this);
      sdFontSystem.ensureLoaded(renderer);
    }
    fail(StrId::STR_LOW_MEMORY_RETRY);
    return;
  }
  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);
  requestUpdate();
  checkAndConnectWifi();
}

void HyphenationPackDownloadActivity::startBrowse() {
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(HyphenationPack::DIR) && !Storage.mkdir(HyphenationPack::DIR))) {
    LOG_ERR("HYPH", "Unable to create %s", HyphenationPack::DIR);
    finishDownload(HttpDownloader::FILE_ERROR);
    return;
  }
  char path[HyphenationPack::PATH_SIZE];
  if (!HyphenationPack::pathFor(language.primaryTag, path, sizeof(path))) {
    finishDownload(HttpDownloader::FILE_ERROR);
    return;
  }
  beginDownload(hyphpacks::displayName(language));
  requestUpdateAndWait();
  const std::string url = std::string(HYPHENATION_PACKS_URL) + language.primaryTag + ".cphyph";
  const auto result = downloadFile(url, path);
  if (result == HttpDownloader::HTTP_ERROR && HttpDownloader::lastStatus() == 404) {
    LOG_ERR("HYPH", "No pack at %s", url.c_str());
    fail(StrId::STR_HYPHENATION_PACK_NOT_RELEASED);
    return;
  }
  finishDownload(result);
}

void HyphenationPackDownloadActivity::downloadFinished(const bool cancelled) {
  if (cancelled) {
    leave();
    return;
  }
  char path[HyphenationPack::PATH_SIZE];
  HyphenationPack::pathFor(language.primaryTag, path, sizeof(path));
  if (!HyphenationPack::isValid(path, language.primaryTag)) {
    // A file that will never load must not count as installed.
    Storage.remove(path);
    fail(StrId::STR_HYPHENATION_PACK_DAMAGED);
    return;
  }
  Hyphenator::resetPacks();
  ActivityResult result;
  setResult(std::move(result));
  state = State::DONE;
  requestUpdate();
}

bool HyphenationPackDownloadActivity::handleCustomInput() {
  if (CatalogActivity::handleCustomInput()) return true;
  if (state != State::DONE) return false;
  int x = 0, y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    leave();
  }
  return true;
}

void HyphenationPackDownloadActivity::leave() {
  if (bookPath.empty()) {
    finish();
  } else {
    activityManager.goToReader(bookPath);
  }
}

void HyphenationPackDownloadActivity::restartAfterWifi() {
  if (bookPath.empty()) {
    silentRestartToSettings(static_cast<int>(SettingsSection::Reader));
    return;
  }
  APP_STATE.openEpubPath = bookPath;
  APP_STATE.saveToFile();
  silentRestartToReader();
}

void HyphenationPackDownloadActivity::buildScreen(UiScreen& screen) {
  char title[64];
  snprintf(title, sizeof(title), tr(STR_HYPHENATION_PACK_TITLE_FORMAT), hyphpacks::displayName(language).c_str());
  screenHeader(screen, title);
  if (buildStatusScreen(screen, true, true)) return;
  if (state == State::DONE) catalogCenteredBlock(screen, {{tr(STR_DOWNLOAD_COMPLETE), true}});
}
