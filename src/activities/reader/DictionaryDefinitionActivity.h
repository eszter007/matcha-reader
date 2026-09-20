#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Paged viewer for one dictionary definition. HTML definitions are laid out
// through the EPUB chapter parser into styled Pages; anything else (plain
// text, or HTML too damaged to parse) is word-wrapped once on entry and each
// page renders spans of the original string, so no per-line copies are held.
class DictionaryDefinitionActivity final : public Activity {
 public:
  explicit DictionaryDefinitionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string headword,
                                        std::string definition, bool htmlDefinition = false, std::string dictName = "")
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(std::move(headword)),
        definition(std::move(definition)),
        htmlDefinition(htmlDefinition),
        dictName(std::move(dictName)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
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

  const std::string headword;
  // Not const: onEnter() normalizes embedded NULs (StarDict multi-type
  // separators) to newlines so C-string APIs see the whole text.
  std::string definition;
  const bool htmlDefinition;
  // Shown in the panel footer; empty when the caller had no dictionary title.
  const std::string dictName;
  // Styled path: reader-identical Pages laid out from the HTML definition.
  // Empty means the plain-text span path below is active.
  std::vector<std::unique_ptr<Page>> pages;
  std::vector<Line> lines;
  int currentPage = 0;
  int totalPages = 1;
  int linesPerPage = 1;
  ButtonNavigator buttonNavigator;
};
