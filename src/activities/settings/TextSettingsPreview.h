#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class GfxRenderer;
class TextBlock;

namespace textsettings {

// Settings + geometry that determine the laid-out lines; used to invalidate the cache.
struct PreviewKey {
  int fontId = -1;
  int fontPointSize = -1;
  int screenMargin = -1;
  int textWidth = -1;
  float lineCompression = -1.0f;
  uint8_t alignment = 0xFF;
  bool extraParagraphSpacing = false;
  uint8_t paragraphIndentSpaces = 2;
  int8_t characterSpacing = 0;
  uint8_t wordSpacingPercent = 100;
  bool focusReading = false;
  bool hyphenation = false;
  bool cjkSample = false;
  bool operator==(const PreviewKey&) const = default;
};

// Cached engine preview lines + the key that produced them
struct PreviewLayout {
  PreviewLayout();
  ~PreviewLayout();

  std::vector<std::unique_ptr<TextBlock>> lines;
  PreviewKey key;
};

// Draws the sample-text pane via the reader engine, reusing layout across redraws.
// `fontId` is the caller's EFFECTIVE reader font, not SETTINGS.getReaderFontId(): for a book
// whose script the selected face cannot carry, the page renders with a substitute, and a preview
// that resolved its own font would promise a face and a size the page will not use.
// cjkSample: lay out a Japanese sample instead of the UI language's, for the CJK text profile.
void renderPreview(const GfxRenderer& renderer, PreviewLayout& layout, int previewPadding, int labelGap, int top,
                   int height, const char* familyName, const char* sizeName, int fontId, bool cjkSample = false);

}  // namespace textsettings
