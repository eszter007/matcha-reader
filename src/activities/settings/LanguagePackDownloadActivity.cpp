#include "LanguagePackDownloadActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "components/CatalogScreens.h"

#ifndef CROSSPOINT_BASE_VERSION
#define CROSSPOINT_BASE_VERSION CROSSPOINT_VERSION
#endif

// Packs are published by release.yml to the assets repo, one release per firmware version, since
// a pack only loads on the firmware it was generated with. Overridable for testing.
#ifndef LANGUAGE_PACKS_URL
#define LANGUAGE_PACKS_URL \
  "https://github.com/eszter007/matcha-reader-assets/releases/download/language-packs-" CROSSPOINT_BASE_VERSION "/"
#endif

namespace {
constexpr const char* PACKS_RELEASE_URL = LANGUAGE_PACKS_URL;
// esp_wifi_init reports OOM as an error nothing can act on; the next allocation after it can abort.
// Same floor as the Manage Fonts screen; downloadFile() checks again for TLS before the transfer.
constexpr size_t MIN_FREE_HEAP_BEFORE_WIFI = 48 * 1024;
constexpr size_t MIN_MAX_ALLOC_BEFORE_WIFI = 12 * 1024;

bool needsPack(const Language lang) { return !isLanguageBuiltIn(lang); }

// Every pack language the update should refresh: the ones on the card, and the selected
// language even when its pack is gone or was refused, since that is the one the user wants back.
bool shouldUpdate(const Language lang) {
  return needsPack(lang) && (I18n::packState(lang) != I18n::PackState::Missing ||
                             static_cast<uint8_t>(lang) == SETTINGS.language);
}
}  // namespace

LanguagePackDownloadActivity::LanguagePackDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                           const Language target)
    : CatalogActivity("LanguagePackDownload", renderer, mappedInput), target(target) {}

bool LanguagePackDownloadActivity::hasInstalledPacks() {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (shouldUpdate(static_cast<Language>(i))) return true;
  }
  return false;
}

void LanguagePackDownloadActivity::onEnter() {
  CatalogActivity::onEnter();
  // Cancelled until a pack loads: an error or Back leaves the language screen as it was.
  ActivityResult cancelled;
  cancelled.isCancelled = true;
  setResult(std::move(cancelled));

  // Reached four screens deep, with a pack language's ~15KB strings and the SD fonts resident, Wi-Fi
  // cannot start. Drop both: leaving this screen after Wi-Fi restarts the device, which loads the
  // selected language again, and downloadFinished() switches as soon as the new pack is in.
  {
    RenderLock lock(*this);
    I18N.setLanguage(Language::EN);
    sdFontSystem.releaseAllResidentFonts(renderer);
  }
  LOG_DBG("LANG", "Heap before Wi-Fi: %u free, %u max block", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  if (ESP.getFreeHeap() < MIN_FREE_HEAP_BEFORE_WIFI || ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_BEFORE_WIFI) {
    LOG_ERR("LANG", "Low heap before Wi-Fi (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    {
      // No Wi-Fi was started, so no restart puts these back.
      RenderLock lock(*this);
      I18N.setLanguage(static_cast<Language>(SETTINGS.language));
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

HttpDownloader::DownloadError LanguagePackDownloadActivity::downloadPack(const Language lang) {
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(I18n::PACK_DIR) && !Storage.mkdir(I18n::PACK_DIR))) {
    LOG_ERR("LANG", "Unable to create %s", I18n::PACK_DIR);
    return HttpDownloader::FILE_ERROR;
  }
  const char* code = LANGUAGE_CODES[static_cast<size_t>(lang)];
  const std::string file = std::string(code) + ".cplang";
  beginDownload(I18N.getLanguageName(lang));
  requestUpdateAndWait();
  return downloadFile(PACKS_RELEASE_URL + file, std::string(I18n::PACK_DIR) + "/" + file);
}

void LanguagePackDownloadActivity::startBrowse() {
  auto result = HttpDownloader::OK;
  if (target != Language::_COUNT) {
    result = downloadPack(target);
  } else {
    for (uint8_t i = 0; i < getLanguageCount() && result == HttpDownloader::OK; i++) {
      const auto lang = static_cast<Language>(i);
      if (shouldUpdate(lang)) result = downloadPack(lang);
    }
  }
  if (result == HttpDownloader::HTTP_ERROR && HttpDownloader::lastStatus() == 404) {
    LOG_ERR("LANG", "No packs for %s", CROSSPOINT_BASE_VERSION);
    fail(StrId::STR_LANGUAGE_PACK_NOT_RELEASED);
    return;
  }
  finishDownload(result);
}

void LanguagePackDownloadActivity::downloadFinished(const bool cancelled) {
  if (cancelled) {
    finish();
    return;
  }
  // A pack is only usable when it was built for this firmware; switching proves it loads.
  const auto wanted = target != Language::_COUNT ? target : static_cast<Language>(SETTINGS.language);
  {
    RenderLock lock(*this);
    switched = I18N.setLanguage(wanted);
  }
  if (!switched) {
    // A pack that will never load must not count as installed, or the language stops reading
    // "Download" and the update keeps fetching the same file.
    char path[48];
    snprintf(path, sizeof(path), "%s/%s.cplang", I18n::PACK_DIR, LANGUAGE_CODES[static_cast<size_t>(wanted)]);
    Storage.remove(path);
    fail(StrId::STR_LANGUAGE_PACK_MISMATCH);
    return;
  }
  if (SETTINGS.language != static_cast<uint8_t>(wanted)) {
    SETTINGS.language = static_cast<uint8_t>(wanted);
    SETTINGS.saveToFile();
  }
  ActivityResult result;
  setResult(std::move(result));
  state = State::DONE;
  requestUpdate();
}

bool LanguagePackDownloadActivity::handleCustomInput() {
  if (CatalogActivity::handleCustomInput()) return true;
  if (state != State::DONE) return false;

  int x = 0, y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
  return true;
}

void LanguagePackDownloadActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, tr(STR_LANGUAGE));
  if (buildStatusScreen(screen, true, true)) return;

  if (state == State::DONE) {
    catalogCenteredBlock(
        screen, {{target != Language::_COUNT ? tr(STR_DOWNLOAD_COMPLETE) : tr(STR_LANGUAGE_PACKS_UPDATED), true}});
  }
}
