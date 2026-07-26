#include "PageTextExtractor.h"

#include "Page.h"
#include "VerticalParsedText.h"

namespace {

void encodeUtf8(uint32_t cp, std::string& out) {
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

}  // namespace

std::string PageTextExtractor::fromVerticalPage(const VerticalPage& page) {
  std::string text;
  text.reserve(page.glyphs.size() * 3);

  uint32_t prevParagraph = UINT32_MAX;
  for (const auto& g : page.glyphs) {
    if (g.paragraphIndex != prevParagraph) {
      if (prevParagraph != UINT32_MAX) {
        text.push_back('\n');
      }
      prevParagraph = g.paragraphIndex;
    }

    // BOTH run kinds store their text in rotatedRunText and leave codepoint == 0. Letting
    // UprightRun (tate-chu-yoko digits) fall through to encodeUtf8(0) embedded a NUL byte in
    // the returned string -- every c_str() consumer (the render prewarm!) silently truncated
    // there, so most of the page missed the selected font's prewarm and fell back to the
    // companion font mid-page (confirmed on-device: Kyokasho page flipping to NotoSans after
    // the first tate-chu-yoko number).
    if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
      if (!g.rotatedRunText.empty()) {
        text += g.rotatedRunText;
      }
      continue;
    }
    if (g.codepoint == 0) continue;  // never emit NUL into a string with c_str() consumers

    encodeUtf8(g.codepoint, text);

    // Furigana readings render as real glyphs (smaller font, same family) -- include them so
    // the prewarm covers them too; otherwise every ruby character is an on-demand SD read
    // through the 8-slot overflow ring on EVERY page render (measured: 120 reads, ~750ms/page
    // on a furigana-dense book).
    if (!g.rubyText.empty()) {
      text += g.rubyText;
    }
  }

  return text;
}

std::string PageTextExtractor::fromHorizontalPage(const Page& page) {
  std::string text;
  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (!line.getBlock()) continue;
    const auto& words = line.getBlock()->getWords();
    for (const auto& w : words) {
      if (!text.empty()) text += " ";
      text += w;
    }
  }
  return text;
}
