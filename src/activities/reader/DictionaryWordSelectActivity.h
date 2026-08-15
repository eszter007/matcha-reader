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
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::string folderName, int baseFontId)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        fontId(baseFontId),
        folderName(std::move(folderName)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Redraws the reader's page (word boxes over it), so it follows the reading
  // surface's night-mode polarity; a normal-polarity flash mid-lookup jars.
  bool appliesNightMode() const override { return true; }

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
    // The font this word was laid out AND drawn with by the page: the block's own font when it
    // carries a CSS font-size, or an inline font-size on the word itself. Measuring the box with
    // one font and repainting the word with another sizes the highlight for text that is not
    // there -- see drawHighlightWithSnapshot.
    int fontId;
  };

  enum class Popup : uint8_t { None, NotFound, Error };

  void extractWords();
  // Hit-test / highlight height for a word: the line height of its own font when CSS gave it
  // one, the page's otherwise.
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
  // The page's base font, as the reader laid it out: effectiveReaderFontId(), not the raw
  // setting. A book whose script the selected family cannot carry is rendered with a
  // substitute, and measuring the page here with the setting would disagree with the pixels.
  int fontId = 0;
  int lineHeight = 0;

  std::vector<WordBox> words;
  int selected = 0;
  uint16_t rowCount = 0;
  bool confirmPressSeen = false;

  Dictionary dict;
  bool dictOpenAttempted = false;
  bool dictOpenOk = false;
  std::string folderName;
  bool dictNeedsIndex = false;

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
};
