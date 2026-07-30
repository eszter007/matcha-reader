#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop, std::string folderName)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop), folderName(std::move(folderName)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Screen box of one selectable word. `text` points into the owned Page's
  // TextBlock arena (NUL-terminated), valid for this activity's lifetime.
  struct WordBox {
    int16_t x;
    int16_t y;
    int16_t width;
    uint16_t row;
    const char* text;
    EpdFontFamily::Style style;
    // The font the line was laid out with: a block carrying a CSS font-size is drawn with its
    // own (larger) font, and measuring its words with the reader's font would size the
    // highlight box for text that is not there.
    int fontId;
  };

  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void extractWords();
  // Hit-test / highlight height for a word: its own line height when the block carries a CSS
  // font-size, the page's otherwise.
  int wordHeight(const WordBox& word) const;
  int closestInRow(uint16_t row, int centerX) const;
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void performLookup();
  bool drawHighlightWithSnapshot();
  void drawHints() const;

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  uint16_t rowCount = 0;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  std::string folderName;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;

  // Differential highlight repaint: the pixels under the current highlight
  // box, so a cursor move restores them and repaints only the two affected
  // boxes instead of re-running the full two-pass page render (which also
  // reloads every SD-font glyph on the page). snapshotIdx is the word whose
  // under-pixels are saved; -1 means the framebuffer no longer holds a clean
  // page (popup drawn, sub-activity shown) and the next render must be full.
  static constexpr size_t SNAPSHOT_CAPACITY = 4096;
  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotIdx = -1;

  // The activity is entered while Confirm is still held (long-press trigger):
  // ignore the stale release until a fresh press is seen.
  bool confirmPressSeen = false;
};
