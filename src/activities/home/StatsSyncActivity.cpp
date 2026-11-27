#include "StatsSyncActivity.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/ClockSyncActivity.h"
#include "activities/settings/SettingsSection.h"
#include "components/HomeTabBar.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "stats/StatsSync.h"
#include "sync/SyncEngine.h"

void StatsSyncActivity::onEnter() {
  Activity::onEnter();
  const bool hasAccount =
      KOREADER_STORE.getServerType() == KOReaderServerType::CROSSPOINT && KOREADER_STORE.hasCredentials();
  const bool hasProgressSync = syncsBooks() && KOREADER_STORE.hasCredentials();
  if (!hasAccount && !syncClock && !hasProgressSync) {
    state = NO_ACCOUNT;
    requestUpdate();
    return;
  }
  state = SYNCING;
  if (hasProgressSync) ensureLibraryIndex();
  if (WiFi.status() == WL_CONNECTED) {
    requestUpdate();
    return;
  }
  restartOnExit = true;
  // Resident SD fonts (a Japanese title on Home) leave too little heap for TLS once Wi-Fi is up:
  // 89KB free before Wi-Fi left a 17KB block, and both requests were refused. The restart on
  // exit loads them again.
  {
    RenderLock lock(*this);
    sdFontSystem.releaseAllResidentFonts(renderer);
    // A book left underneath keeps its glyph caches; they are the largest thing Wi-Fi can have back.
    if (origin == Origin::ShortcutInReader) {
      if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
    }
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("STATS", "OOM: WifiSelectionActivity");
    state = FAILED;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    // Left from loop(): replacing this activity inside its own callback is unsafe.
    if (result.isCancelled) state = CANCELLED;
    requestUpdate();
  });
}

void StatsSyncActivity::onExit() {
  Activity::onExit();
  // The restart clears the heap WiFi and TLS fragmented, and its boot folds the download in.
  if (restartOnExit) {
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(false);
      delay(30);
    }
    switch (origin) {
      case Origin::Settings:
        silentRestartToSettings(static_cast<int>(SettingsSection::Insights));
        break;
      case Origin::Insights:
        silentRestartToHomeTab(static_cast<int>(HomeTab::Stats));
        break;
      case Origin::ShortcutInReader:
        silentRestartToReader();
        break;
      case Origin::Shortcut:
        silentRestart();
        break;
    }
  }
}

void StatsSyncActivity::leave() {
  if (origin != Origin::Insights) {
    finish();
  } else {
    activityManager.goToReadingStats();
  }
}

void StatsSyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    state = FAILED;
    requestUpdate();
    return;
  }
  const bool clockOk = !syncClock || ClockSyncActivity::syncAndRecord();
  const bool hasAccount =
      KOREADER_STORE.getServerType() == KOReaderServerType::CROSSPOINT && KOREADER_STORE.hasCredentials();
  // Any KOSync account syncs progress; Insights need the CrossPoint server.
  const bool booksOk = !syncsBooks() || !KOREADER_STORE.hasCredentials() || syncAllBooks();
  if (!hasAccount && !(syncsBooks() && KOREADER_STORE.hasCredentials())) {
    // The clock is set; say why Insights were skipped.
    state = clockOk ? NO_ACCOUNT : FAILED;
    requestUpdate();
    return;
  }
  // Download first: the upload's TLS sessions would otherwise leave less room for the list.
  const bool downloaded = !hasAccount || statsSync::download();
  const bool uploaded = !hasAccount || statsSync::run("", "");
  // A saved download is merged at boot, so it needs the restart even when WiFi was already up.
  if (hasAccount && downloaded) restartOnExit = true;
  if (downloaded && uploaded && clockOk && booksOk) {
    // The restart then returns to where the sync started; "Done" is the only sign it went through.
    state = DONE;
    requestUpdateAndWait();
    delay(1000);
    leave();
    return;
  }
  LOG_ERR("STATS", "Refresh incomplete: download %d, upload %d, clock %d, books %d", downloaded, uploaded, clockOk,
          booksOk);
  state = FAILED;
  requestUpdate();
}

// The book list comes from the library index; rebuilt here, before Wi-Fi takes the heap, when a
// firmware update or a book transfer left it stale.
void StatsSyncActivity::ensureLibraryIndex() {
  if (!library::isLibraryIndexDirty()) {
    auto index = makeUniqueNoThrow<library::LibraryIndexFile>();
    if (index && index->open(library::libraryIndexPath())) return;
  }
  library::BuildStats stats;
  if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0)) {
    LOG_ERR("STATS", "Library index build failed");
  }
}

bool StatsSyncActivity::syncAllBooks() {
  // The index and its record stay off the stack; one book is loaded at a time.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record{};
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("STATS", "OOM: library index");
    return false;
  }
  if (!reader->index.open(library::libraryIndexPath())) {
    LOG_ERR("STATS", "Cannot open library index");
    return false;
  }
  bookTotal = reader->index.bookCount();
  bool allOk = true;
  std::string path;
  for (uint16_t i = 0; i < bookTotal; i++) {
    bookDone = static_cast<uint16_t>(i + 1);
    if (!reader->index.readRecord(i, reader->record) || !reader->index.readPath(reader->record, path)) continue;
    if (!FsHelpers::hasEpubExtension(path)) continue;
    requestUpdateAndWait();
    KOReaderProgress progress;
    bool prepared;
    {
      RenderLock lock;
      GfxRenderer::FrameBufferLoan loan(renderer);
      prepared = syncEngine::savedBookProgress(path, progress);
    }
    if (!prepared) continue;
    bool ok;
    {
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
      // The loan frees the framebuffer to the heap for TLS.
      RenderLock lock;
      GfxRenderer::FrameBufferLoan loan(renderer);
#endif
      ok = syncEngine::uploadUnlessBehind(path, progress);
    }
    if (!ok) allOk = false;
  }
  bookTotal = 0;
  return allOk;
}

void StatsSyncActivity::loop() {
  if (state == CANCELLED) {
    leave();
    return;
  }
  if (state == SYNCING) {
    // Paint "Syncing..." before blocking on the network.
    requestUpdateAndWait();
    runSync();
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) leave();
}

void StatsSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int midY = renderer.getScreenHeight() / 2;
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 syncClock                    ? tr(STR_SYNC_STATS_CLOCK)
                 : origin == Origin::Insights ? tr(STR_REFRESH_STATS)
                                              : tr(STR_SYNC_STATS));
  switch (state) {
    case SYNCING:
      if (bookTotal > 0) {
        char text[64];
        I18N.formatCountOf(text, sizeof(text), StrId::STR_LIBRARY_POSITION, bookDone, bookTotal);
        renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_SYNC_PROGRESS));
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, text);
      } else {
        renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_SYNCING_STATS));
      }
      break;
    case NO_ACCOUNT:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_SET_CREDENTIALS_FIRST), true, EpdFontFamily::BOLD);
      break;
    case DONE:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_DONE), true, EpdFontFamily::BOLD);
      break;
    case CANCELLED:
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_FAIL), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_TRY_AGAIN_LATER));
      break;
  }
  if (state != SYNCING && state != DONE) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
