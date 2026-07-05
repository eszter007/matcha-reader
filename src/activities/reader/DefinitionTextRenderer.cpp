#include "DefinitionTextRenderer.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>

namespace DefinitionText {

namespace {
// One rendered line is bounded by screen width (~25-40 CJK chars); 512 bytes is generous.
// Remainders longer than this skip the "fits on one line" fast path -- they could never fit.
constexpr size_t LINE_BUF_CAP = 512;

inline size_t utf8CharLen(const unsigned char c0) {
  if (c0 >= 0xF0) return 4;
  if (c0 >= 0xE0) return 3;
  if (c0 >= 0xC0) return 2;
  return 1;
}
}  // namespace

WrapResult DrawWrappedImpl(GfxRenderer& renderer, const int fontId, const std::string& text, const int textX,
                           const int startY, const int lineHeight, const int maxWidth, const int maxY,
                           const int scrollOffset, std::string& lineBuf) {
  WrapResult out;
  int defY = startY;
  int lineIndex = 0;

  size_t nlPos = 0;
  while (nlPos <= text.size()) {
    const size_t nextNl = text.find('\n', nlPos);
    const size_t paraEnd = nextNl == std::string::npos ? text.size() : nextNl;
    size_t remStart = nlPos;
    const size_t remEndFixed = paraEnd;
    nlPos = nextNl == std::string::npos ? text.size() + 1 : nextNl + 1;

    if (remStart == remEndFixed) {
      lineIndex++;
      if (lineIndex > scrollOffset) defY += lineHeight / 2;
      continue;
    }

    while (remStart < remEndFixed) {
      const size_t remLen = remEndFixed - remStart;

      // Whole remainder fits on one line? (Only measurable when it fits the buffer; a longer
      // remainder could never fit one line anyway.)
      if (remLen < LINE_BUF_CAP) {
        lineBuf.assign(text, remStart, remLen);
        if (renderer.getTextWidth(fontId, lineBuf.c_str()) <= maxWidth) {
          lineIndex++;
          if (lineIndex > scrollOffset && defY + lineHeight <= maxY) {
            renderer.drawText(fontId, textX, defY, lineBuf.c_str(), true);
            defY += lineHeight;
            out.linesDrawn++;
          }
          break;
        }
      }

      // Accumulate characters into the (never-growing) line buffer until too wide.
      lineBuf.clear();
      size_t lastSpaceBreak = std::string::npos;  // bytes of accepted prefix ending after a space
      size_t pos = remStart;
      while (pos < remEndFixed) {
        const auto c0 = static_cast<unsigned char>(text[pos]);
        size_t charLen = utf8CharLen(c0);
        if (charLen > remEndFixed - pos) charLen = remEndFixed - pos;
        if (lineBuf.size() + charLen >= LINE_BUF_CAP) break;  // buffer full -> hard break, no realloc
        lineBuf.append(text, pos, charLen);
        if (renderer.getTextWidth(fontId, lineBuf.c_str()) > maxWidth) {
          // Never orphan sentence-ending punctuation at the start of the next line.
          uint32_t cp = 0;
          if (charLen == 3) {
            cp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[pos + 2]) & 0x3F);
          } else if (charLen == 1) {
            cp = c0;
          }
          const bool keepPunct = cp == 0x3002 || cp == 0x3001 || cp == 0xFF01 || cp == 0xFF1F || cp == '.' ||
                                 cp == ',' || cp == '!' || cp == '?';
          if (keepPunct) {
            pos += charLen;  // punctuation stays on this line
          } else {
            lineBuf.resize(lineBuf.size() - charLen);  // reject the overflowing character
          }
          break;
        }
        if (text[pos] == ' ') lastSpaceBreak = lineBuf.size();
        pos += charLen;
      }

      if (lineBuf.empty()) {
        // Single character wider than maxWidth -- force it onto its own line.
        size_t cl = utf8CharLen(static_cast<unsigned char>(text[remStart]));
        if (cl > remLen) cl = remLen;
        lineBuf.assign(text, remStart, cl);
        remStart += cl;
      } else if (lastSpaceBreak != std::string::npos && lastSpaceBreak > 0) {
        // Break at the last space to keep Latin words intact.
        remStart += lastSpaceBreak;
        lineBuf.resize(lastSpaceBreak);
        if (remStart < remEndFixed && text[remStart] == ' ') remStart++;
      } else {
        remStart += lineBuf.size();
      }

      lineIndex++;
      if (lineIndex > scrollOffset && defY + lineHeight <= maxY) {
        renderer.drawText(fontId, textX, defY, lineBuf.c_str(), true);
        defY += lineHeight;
        out.linesDrawn++;
      }
    }
  }

  out.totalLines = lineIndex;
  return out;
}

WrapResult drawWrapped(GfxRenderer& renderer, const int fontId, const std::string& text, const int textX,
                       const int startY, const int lineHeight, const int maxWidth, const int maxY,
                       const int scrollOffset) {
  // The single allocation of this whole function, guarded: if even one line buffer doesn't
  // fit, drawing text under a heap this starved would abort() inside the renderer anyway --
  // show nothing (header/word still render) rather than crash. -fno-exceptions makes an
  // unguarded reserve() an abort, not an error.
  if (ESP.getMaxAllocHeap() < LINE_BUF_CAP + 4 * 1024) {
    LOG_ERR("DEFTXT", "Skipping definition render, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
    return {};
  }
  std::string lineBuf;
  lineBuf.reserve(LINE_BUF_CAP);
  return DrawWrappedImpl(renderer, fontId, text, textX, startY, lineHeight, maxWidth, maxY, scrollOffset, lineBuf);
}

}  // namespace DefinitionText
