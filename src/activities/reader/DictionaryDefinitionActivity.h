#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#include "util/SentenceMining.h"

// Paged viewer for the definitions of one word, one entry per dictionary that has it, read as a
// single flow: paging past an entry's last page opens the next. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain text, or HTML too
// damaged to parse) is word-wrapped on entry and each page renders spans of the original string,
// so no per-line copies are held. Only the entry on screen is laid out.
class DictionaryDefinitionActivity final : public Activity {
 public:
  struct Entry {
    std::string headword;
    std::string definition;
    bool html = false;
    std::string dictName;  // footer title; empty when the dictionary has none
    // Enables sentence mining for this entry: Select (and the + button on touch boards) saves it
    // with this entry as its definition. Invalid = no saving.
    sentencemining::Draft draft;
  };

  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::vector<Entry> entries);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class MiningStatus : uint8_t { None, Saved, Failed };
  MiningStatus miningStatus_ = MiningStatus::None;
  void saveSentence();
  const sentencemining::Draft& miningDraft() const { return entries[currentEntry].draft; }

  // Lays out entries[index] and shows its first page, or its last when stepping back into it.
  void showEntry(size_t index, bool atLastPage);
  // One page forward (+1) or back (-1) through the whole flow. False at either end.
  bool stepPage(int direction);

  // One wrapped display line: a byte span of `definition`. Wrapping keeps
  // lines under the screen width, so uint16_t length is ample.
  struct Line {
    uint32_t start;
    uint16_t len;
  };

  // Usable body-text area: the panel's inner rectangle.
  struct BodyArea {
    int width;
    int height;
  };

  // The font the definition is laid out AND drawn with: the Word Lookup Font Size setting, not
  // the reader's body font. A dictionary entry at reading size fills the panel in three lines,
  // and the Japanese panels have always used this setting -- the English one now matches.
  static int definitionFontId();
  // Point size matching definitionFontId(), for the SD-card glyph fallback.
  static uint8_t definitionPointSize();
  // Let the selected SD reader family fill glyphs the built-in serif lacks -- IPA in a
  // pronunciation, above all. The Japanese panels have always done this; without it a reader
  // who installs an IPA-bearing font still sees nothing for those codepoints.
  void ensureGlyphFallback() const;

  BodyArea bodyArea() const;
  bool layoutHtmlPages();
  void wrapText();
  int measureSpan(int fontId, const char* text, size_t len) const;
  void drawBody(int fontId, int x, int startY) const;

  std::vector<Entry> entries;
  size_t currentEntry = 0;
  // The entry on screen, laid out: a copy of its definition, with embedded NULs (StarDict
  // multi-type separators) normalized to newlines so C-string APIs see the whole text.
  std::string definition;
  // Footer title: the dictionary name, plus the entry's place when there are several.
  std::string dictLabel;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
};
