#include "VerticalTextBlock.h"

#include <cstring>

#include "GfxRenderer.h"

namespace {
constexpr int kNoStyle = 0;

void encodeCodepoint(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

void drawGlyphs(GfxRenderer& renderer, const VerticalPage& page, int fontId, int offsetX, int offsetY, bool black) {
  for (const VerticalGlyph& g : page.glyphs) {
    const int dx = g.x + offsetX;
    const int dy = g.y + offsetY;

    if (g.rotated) {
      renderer.drawTextRotated90CW(fontId, dx, dy, g.rotatedRunText.c_str(), black,
                                    static_cast<EpdFontFamily::Style>(kNoStyle));
      continue;
    }

    std::string utf8Char;
    encodeCodepoint(g.codepoint, utf8Char);
    renderer.drawText(fontId, dx, dy, utf8Char.c_str(), black, static_cast<EpdFontFamily::Style>(kNoStyle));
  }
}

}  // namespace

void VerticalTextBlock::render(GfxRenderer& renderer, int fontId, int offsetX, int offsetY, bool black) const {
  drawGlyphs(renderer, page_, fontId, offsetX, offsetY, black);
}

void VerticalTextBlock::render(GfxRenderer& renderer, int fontId, int rubyFontId, int offsetX, int offsetY,
                                bool black) const {
  drawGlyphs(renderer, page_, fontId, offsetX, offsetY, black);

  const int rubyLineH = (renderer.getLineHeight(rubyFontId) + 1) / 2;
  const int rubyAscender = renderer.getFontAscenderSize(rubyFontId) / 2;
  const int baseLineH = renderer.getLineHeight(fontId);
  const auto rubyStyle = static_cast<EpdFontFamily::Style>(EpdFontFamily::SUP);

  for (const VerticalGlyph& g : page_.glyphs) {
    if (g.rubyText.empty() || g.rotated) continue;

    const int rubyX = g.x + offsetX + baseLineH + 1;

    size_t rubyCharCount = 0;
    {
      size_t ri = 0;
      while (ri < g.rubyText.size()) {
        const auto c0 = static_cast<unsigned char>(g.rubyText[ri]);
        if (c0 < 0x80) ri += 1;
        else if ((c0 & 0xE0) == 0xC0) ri += 2;
        else if ((c0 & 0xF0) == 0xE0) ri += 3;
        else ri += 4;
        rubyCharCount++;
      }
    }

    const int rubyBlockH = static_cast<int>(rubyCharCount) * rubyLineH;
    int rubyY = g.y + offsetY + (baseLineH - rubyBlockH) / 2 - rubyLineH;

    size_t ri = 0;
    while (ri < g.rubyText.size()) {
      const auto c0 = static_cast<unsigned char>(g.rubyText[ri]);
      size_t charLen = 1;
      if (c0 >= 0xF0) charLen = 4;
      else if (c0 >= 0xE0) charLen = 3;
      else if (c0 >= 0xC0) charLen = 2;

      if (ri + charLen > g.rubyText.size()) break;

      char buf[5];
      std::memcpy(buf, g.rubyText.data() + ri, charLen);
      buf[charLen] = '\0';

      renderer.drawText(rubyFontId, rubyX, rubyY, buf, black, rubyStyle);
      rubyY += rubyLineH;
      ri += charLen;
    }
  }
}
