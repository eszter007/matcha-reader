#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"

// A book read by page (XTC/XTCH, manga). Its position is a page index, and KOReader's convention for
// paged documents applies: the progress string is the 1-based page number.
struct PagedSyncBook {
  uint32_t page = 0;  // 0-based
  uint32_t pageCount = 0;
  std::string title;
  std::string author;
  std::string language;  // empty when unknown
  std::string series;    // empty when unknown
  std::optional<float> seriesIndex;
};

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public Activity, private UiAppHost {
 public:
  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                                CrossPointPosition localPosition, SavedProgressPosition localKoPos,
                                std::string localChapterName, const bool pluginEventsOnly = false);
  KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& path,
                       PagedSyncBook book);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Also syncs the clock and downloads Insights on the same connection (the button shortcut).
  void alsoSyncClockAndInsights() { withClockAndInsights = true; }
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    SYNC_COMPLETE,
    NO_REMOTE_PROGRESS,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // null until lazy-loaded after TLS in performSync()
  std::string epubPath;
  std::string localChapterName;
  CrossPointPosition localPosition;

  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string documentHash;

  // Set for a paged book; the EPUB-only members above then stay unused.
  std::optional<PagedSyncBook> paged;
  uint32_t remotePage = 0;

  // Remote progress data
  bool hasRemoteProgress = false;
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Local progress as KOReader format (pre-computed before Epub was released)
  SavedProgressPosition localProgress;

  // Selection in result screen (0=Apply, 1=Upload)
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;

  // Reuses the KOReader WiFi connect/drain/return flow without a server: flush
  // plugged-outbox events instead of syncing progress.
  bool pluginEventsOnly = false;
  bool withClockAndInsights = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void performUpload();
  bool smartSyncEnabled() const;
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  KOReaderSyncClient::Error fetchTwinProgress();
  void decidePaged(bool fromTwin);
  void saveProgressAndReturn(int spineIndex, int page);
  void returnToReader();

  // The UiAppHost app hosts the interactive states (SHOWING_RESULT compare
  // rows and the NO_REMOTE_PROGRESS upload prompt) so they get themed
  // rows/buttons and tap-flash; the header stays on GUI.drawHeader for the
  // battery indicator and the purely-informational states keep their raw
  // centered text.
  static void resultScreen(UiScreen& screen, void* user);
  static void onResultRow(const freeink::ui::ActionEvent& event, void* user);
  void buildResultScreen(UiScreen& screen);
  void chooseResultOption();
  void startUpload();
};
