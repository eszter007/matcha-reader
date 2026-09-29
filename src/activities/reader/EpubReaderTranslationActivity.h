#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/PanelTextPages.h"
#include "util/ButtonNavigator.h"

// The page's translation in the floating dictionary panel, drawn over the page: the connection,
// progress and any error are shown inside the panel too. A saved network is joined from the panel
// itself; only picking a network or typing a password opens the full-screen Wi-Fi list, after
// which the page behind the panel is restored from the copy saved on entry.
class EpubReaderTranslationActivity final : public Activity {
 public:
  // preTranslatedText: if non-empty, the activity shows it directly without
  // any network call (used when a translation was already extracted offline
  // during manga conversion and stored alongside the page data).
  // resumedAfterRestart: true only when setup() re-created this activity from the
  // TRANSLATE_STASH_PATH stash after a silent restart. Gates the stash-and-restart
  // fallback to one attempt -- on a pristine post-boot heap a second gate failure
  // is a real error, not fragmentation, so it must show the message instead of
  // restart-looping.
  explicit EpubReaderTranslationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string sourceText,
                                         std::string preTranslatedText = "", bool resumedAfterRestart = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == WIFI_SELECTION || state == TRANSLATING; }

 private:
  enum State {
    CONNECTING,  // joining the last-used saved network, shown in the panel
    WIFI_SELECTION,
    TRANSLATING,
    SHOWING_RESULT,
    ERROR,
  };

  State state = WIFI_SELECTION;
  std::string sourceText;
  std::string translatedText;
  std::string errorMessage;
  bool hasPreTranslation = false;
  bool resumedAfterRestart = false;

  // Write sourceText to TRANSLATE_STASH_PATH and silent-restart into a fresh-heap
  // translation (see SilentRestart.h). Returns false if the stash could not be
  // written -- caller then falls back to the low-memory error message.
  bool stashAndRestart();

  PanelTextPages textPages;  // laid out once the translation arrives
  int currentPage = 0;
  unsigned long connectStartMs = 0;
  // The page behind the panel must be copied back from SD before the next render: set after the
  // full-screen Wi-Fi list, and on a restarted translation, whose framebuffer starts blank.
  bool restoreBackgroundPending = false;

  // The framebuffer (the reader's page) saved to SD on entry and read back after anything that
  // drew over it. Written straight from and into the framebuffer, so no heap is borrowed.
  static bool saveBackground(const GfxRenderer& renderer);
  static bool restoreBackground(const GfxRenderer& renderer);
  // Joins the last-used saved network without the Wi-Fi list; false when there is none.
  bool startQuickConnect();
  void pollQuickConnect();
  void startWifiSelection();
  void stepPage(int direction);
  void cancel();

  ButtonNavigator buttonNavigator;

  bool readApiKey(std::string& keyOut);
  bool callGeminiApi(const std::string& apiKey);

  void onWifiComplete(bool success);
};
