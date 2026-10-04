#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "EndOfBookOptions.h"
#include "ReaderSession.h"
#include "activities/Activity.h"

class ReaderActivity : public Activity {
 protected:
  const std::string bookPath;
  int pagesUntilFullRefresh;
  bool forcedRefreshPending = false;

  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  std::atomic<bool> endOfBookOptionsReady{false};
  ReaderSession readerSession;
  std::atomic<bool> pageRendered{false};
  bool bookRemembered = false;
  void markPageRendered();
  void rememberBookOnceRendered();

  explicit ReaderActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput,
                          std::string bookPath, bool allowFastInitialRefresh);

  virtual bool loadBook() = 0;
  virtual bool hasBook() const = 0;
  virtual std::string getBookTitle() const = 0;
  virtual std::string getBookAuthor() const { return ""; }
  virtual std::string getBookThumbBmpPath() const { return ""; }
  virtual const char* getBookLanguage() const { return nullptr; }
  virtual void onReaderEnter() = 0;
  virtual void onReaderExit() = 0;
  virtual void readerLoop() = 0;
  // Called when loadBook() failed. Return true to keep the activity alive
  // (e.g. showing a dialog); false finishes it (the default).
  virtual bool handleLoadFailure() { return false; }
  // Input for whatever handleLoadFailure() left on screen. True while it owns the loop.
  virtual bool handleLoadFailureInput() { return false; }
  // Whole-book progress for the reader.exit plugin event, reusing the
  // per-reader ScreenshotInfo implementations.
  //
  // Upstream added this helper in d3e55c53 with an earlier slice of this
  // feature, then removed it in c1e1fec3 as dead code once no caller remained
  // ("removes unnecessary wrapper structs"). It is reinstated here because this
  // change reintroduces the callers: ReaderActivity.cpp uses it for the progress
  // string and for the session's render-complete basis points, and
  // EpubReaderActivity overrides getProgressBasisPoints() and falls back to it.
  int getProgressPercent() const { return getScreenshotInfo().progressPercent; }
  virtual int getProgressBasisPoints() const { return getProgressPercent() * 100; }

  virtual bool isAtEndOfBook() const { return false; }
  virtual void onReturnFromEndOfBook() {}

  void clearEndOfBookOptionsIfNeeded();
  bool handleBackNavigation();
  /** True while the end-of-book suggestion menu is on screen and owning input. */
  bool endOfBookMenuActive() const;
  bool handleEndOfBookMenu(bool suppressConfirmRelease = false);
  bool handleEndOfBookPageTurn(bool prevTriggered, bool nextTriggered);
  bool renderEndOfBook(const char* logTag);
  void disableFastInitialRefresh() { pagesUntilFullRefresh = 0; }
  void notePageTurn(bool forward, bool succeeded);
  void flushReaderSession();

 public:
  ~ReaderActivity() override = default;

  static std::unique_ptr<Activity> create(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                          bool allowFastInitialRefresh);

  void onEnter() final;
  void onExit() final;
  void loop() final;
  void prepareForSleep() override;

  bool isReaderActivity() const final { return true; }
  bool handleForcedRefresh() final;

 private:
  unsigned long readingSessionStartMs = 0;
};
