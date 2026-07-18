#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

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
  bool preventAutoSleep() override { return state == WIFI_SELECTION || state == TRANSLATING; }

 private:
  enum State {
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

  int scrollOffset = 0;
  int maxScrollOffset = 0;

  ButtonNavigator buttonNavigator;

  bool readApiKey(std::string& keyOut);
  bool callGeminiApi(const std::string& apiKey);

  void onWifiComplete(bool success);
};
