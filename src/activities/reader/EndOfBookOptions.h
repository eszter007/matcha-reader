#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
struct Rect;
class MappedInputManager;

// Shared End-of-Book screen for the book readers: the finished book as a ticket torn in two -- the
// cover and title on one half with a 読了 stamp, the reading stats on the stub -- and under it a
// Home button beside "Continue with" the next book in the folder (Home alone when there is none).
class EndOfBookOptions {
 public:
  enum class Action { None, Redraw, OpenBook, GoHome, LastPage };

  // Height the reader generates the cover thumbnail at; the ticket scales it down to fit.
  static constexpr int COVER_HEIGHT = 300;

  explicit EndOfBookOptions(GfxRenderer& renderer);

  // Loads the stats and finds the next book; no-op when already loaded. Call ONLY from the
  // reader's render() (the render task, serialized by RenderLock) — the loaded flag is the
  // release/acquire publication point that lets the main task read the result safely.
  // Also marks the book finished, so the milestone counts it. bookLanguage may be null;
  // coverThumbPath empty when the book has no cover thumbnail.
  void loadOnce(const std::string& currentBookPath, const std::string& bookTitle, const char* bookLanguage,
                const std::string& coverThumbPath);

  // True when the end screen is showing and should own the reader's input.
  bool menuActive() const;

  // Left/Right and side Up/Down move between Home and Continue, Confirm or a tap activates one,
  // and a short Back press returns to the last page of the book. Fills openPath when the result
  // is OpenBook. Returns Action::None when nothing relevant was pressed; callers continue their
  // normal input path (keeping long-press Back to the file browser working).
  Action handleMenuInput(const MappedInputManager& input, std::string* openPath);

  // Draws the full end screen onto a cleared buffer.
  void render(GfxRenderer& renderer, const MappedInputManager& input);

 private:
  GfxRenderer& renderer;
  std::string folder;
  std::string title;
  std::string coverPath;
  const uint8_t* stamp_ = nullptr;  // the 読了 / 读完 / 讀完 / 睇完 hanko for the book's language
  // The next book: its file name, and its cover thumbnail path template (empty if none).
  std::string nextName;
  std::string nextCoverTemplate;
  // Snapshot of the stats on the ticket stub, taken once in loadOnce(): the per-book history is
  // loaded, read and dropped there rather than held for the life of the screen.
  struct Summary {
    uint32_t minutes = 0;
    int daySpan = 0;
    int streak = 0;
    uint32_t lookups = 0;
    uint32_t sentences = 0;
    uint16_t booksFinished = 0;
    uint16_t languageBooksFinished = 0;
    std::string languageName;  // empty when the book declares no language
    char finishedOn[12] = {};  // "2026.10.15", the date on the stamp
  };
  Summary summary;
  void loadSummary(const std::string& bookPath, const char* bookLanguage);

  // Draws the ticket halves and the stamp above `bottom`.
  void drawTicket(const Rect& safe, int bottom) const;
  void drawStub(int x, int y, int w, int h) const;
  int stubHeight() const;
  void drawButtons(int x, int y, int w, int h, int selected) const;

  // 0 = Continue (when there is a next book), the last index = Home.
  int itemCount() const { return nextName.empty() ? 1 : 2; }
  bool isHomeIndex(int index) const { return index == itemCount() - 1; }

  std::atomic<int> selector{0};
  std::atomic<bool> isLoaded{false};

  // Button hit rects in screen coordinates, published by render() for touch input.
  struct HitRect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  };
  mutable HitRect continueRect;
  mutable HitRect homeRect;

  std::string nextPath() const;
};
