#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "util/Dictionary.h"
#include "util/SentenceMining.h"

// Word selection over the current reader page: Left/Right step through words
// in reading order, Up/Down jump rows, Confirm looks the word up and opens
// DictionaryDefinitionActivity, Back returns to the reader. On touch devices a
// touch-down moves the highlight and a tap on a word looks it up directly.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::vector<std::string> folderNames, std::string language, int baseFontId,
                                        int lookupAtX = -1, int lookupAtY = -1)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        lookupAtX(lookupAtX),
        lookupAtY(lookupAtY),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        fontId(baseFontId),
        language(std::move(language)) {
    for (auto& folder : folderNames) {
      if (dictCount >= MAX_DICTIONARIES) break;
      dicts[dictCount++].folder = std::move(folder);
    }
  }

  // Dictionaries a word is looked up in together (see DictionaryRegistry::foldersForLanguage).
  static constexpr size_t MAX_DICTIONARIES = 4;

  // Screen point to open on: the word under it is selected and looked up
  // immediately, so a long press on the page goes straight to the definition
  // instead of dropping the reader into word selection. -1 = normal entry.
  int lookupAtX = -1;
  int lookupAtY = -1;

  // The book and the next page's text, so a looked-up word can be saved for sentence mining.
  void setMiningContext(sentencemining::BookContext context) { mining_ = std::move(context); }
  // Looks this text up straight away instead of a word on the page (a clipping selection's
  // Look Up); closing the definition returns to the page.
  void setLookupText(std::string text) { lookupText = std::move(text); }

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
    // The font this word was laid out AND drawn with by the page: the block's own font when it
    // carries a CSS font-size, or an inline font-size on the word itself. Measuring the box with
    // one font and repainting the word with another sizes the highlight for text that is not
    // there -- see drawHighlightWithSnapshot.
    int fontId;
    // Layout hyphenation splits a word across a line break ("any-" / "one"), and each half reaches
    // this list as its own selectable token. These link the two halves so either one looks up the
    // whole word; -1 when the word is not part of a split. Indices into `words`.
    int16_t joinNext = -1;  // set on the "any-" half: index of the remainder
    int16_t joinPrev = -1;  // set on the "one" half: index of the hyphenated prefix
  };
  // The word to look up for a box, joining a hyphenated pair back together. Returns a reference
  // into `scratch` when a join happened, so the caller owns the storage.
  const char* lookupTextFor(size_t index, std::string& scratch) const;
  // The other half of a hyphenated pair, or nullptr. Both halves are highlighted together: the
  // reader selected one word, and showing only the half they pointed at makes the selection look
  // like it stopped at the line break.
  const WordBox* joinedPartner(size_t index) const;

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
  sentencemining::BookContext mining_;
  // A paragraph's drop cap is drawn outside the text flow, so its letter is in no word: the
  // first word of that line reads "T" for "IT". Kept here (one per page is the norm) so the
  // saved sentence gets the letter back. -1 when the page has none.
  int16_t dropCapWord_ = -1;
  uint32_t dropCapCp_ = 0;
  uint32_t dropCapPrefixCp_ = 0;
  // The sentence around the selected word (and its hyphen-split half, if any), as card HTML.
  std::string miningSentence(std::string_view surface) const;
  uint16_t rowCount = 0;
  bool confirmPressSeen = false;
  unsigned long lastHorizontalMoveTime = 0;
  std::string lookupText;

  // One dictionary of the lookup, opened (and its index built) the first time a word is looked up.
  struct DictSlot {
    Dictionary dict;
    std::string folder;
    bool openAttempted = false;
    bool openOk = false;
    bool needsIndex = false;
  };
  DictSlot dicts[MAX_DICTIONARIES];
  size_t dictCount = 0;
  // The book's EPUB language tag, selecting the dictionary's inflection rules.
  // Empty for an untagged book, which leaves the folder to decide.
  std::string language;

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
