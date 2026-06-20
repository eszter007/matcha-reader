#pragma once

#include <Epub/VerticalParsedText.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderWordLookupActivity final : public Activity {
 public:
  explicit EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const VerticalPage& page);

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
  int cursorIndex = 0;

  bool hasResult = false;
  std::string resultHeadword;
  std::string resultDefinition;
  int resultMatchLen = 0;

  ButtonNavigator buttonNavigator;

  void moveCursor(int delta);
  void performLookup();
  std::string buildLookupText(size_t startIdx) const;

  static void encodeUtf8(uint32_t cp, std::string& out);
};
