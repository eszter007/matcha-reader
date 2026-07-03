#pragma once

#include <GfxRenderer.h>
#include <Epub/VerticalParsedText.h>

struct Rect;
class Page;

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderWordLookupActivity final : public Activity {
 public:
  // Vertical (tategaki) reading mode.
  explicit EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const VerticalPage& page);
  // Horizontal (yokogaki) reading mode.
  explicit EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const Page& page);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct GlyphRef {
    uint16_t x;
    uint16_t y;
    uint16_t column;
    uint16_t row;
    uint32_t codepoint;
    uint32_t paragraphIndex;
    bool rotated;
  };

  std::vector<GlyphRef> selectableGlyphs;
  std::vector<GlyphRef> allGlyphs;  // Full glyph list for building lookup text
  std::vector<size_t> selectToAllIdx;  // Maps selectableGlyphs index → allGlyphs index
  int cursorIndex = 0;

  bool hasResult = false;
  std::string resultHeadword;
  std::string resultDefinition;
  int resultMatchLen = 0;
  bool hasGrammar = false;
  std::string grammarHeadword;
  std::string grammarDefinition;
  int scrollOffset = 0;  // lines scrolled within current entry
  int totalLines = 0;    // total lines in current definition
  int maxScroll = 0;     // max scroll offset (leaves a screenful visible)

  ButtonNavigator buttonNavigator;

  void moveCursor(int delta);
  void performLookup();
  std::string buildLookupText(size_t startIdx) const;

  // Pre-scan allGlyphs (already populated) to build selectableGlyphs/selectToAllIdx.
  void buildSelectableGlyphs();

  // Heap-aware growth helpers -- bare vector reserve()/push_back() aborts the whole device on OOM
  // under -fno-exceptions (see CLAUDE.md). See EpubReaderWordLookupActivity.cpp for the rationale.
  static void reserveGlyphsSafe(std::vector<GlyphRef>& vec, size_t count);
  static bool pushGlyphSafe(std::vector<GlyphRef>& vec, const GlyphRef& g);

  static void encodeUtf8(uint32_t cp, std::string& out);

  bool initialRenderDone = false;
  int fastRefreshCount = 0;
  static constexpr int kFullRefreshInterval = 10;

  void renderContentArea(const Rect& screen, int contentTop);
};
