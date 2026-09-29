#include "PanelTextPages.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstring>

namespace {
int measureSpan(const GfxRenderer& renderer, const int fontId, const char* text, size_t len) {
  char buf[PanelTextPages::MAX_LINE_BYTES + 1];
  len = std::min(len, PanelTextPages::MAX_LINE_BYTES);
  memcpy(buf, text, len);
  buf[len] = '\0';
  return renderer.getTextAdvanceX(fontId, buf, EpdFontFamily::REGULAR);
}
}  // namespace

void PanelTextPages::clear() {
  lines.clear();
  linesPerPage = 1;
  totalPages = 1;
}

// Greedy word-wrap into byte spans. '\r' is dropped by treating it as a space at a token edge.
void PanelTextPages::layout(const GfxRenderer& renderer, const int fontId, const std::string& text, const int width,
                            const int height) {
  lines.clear();
  lines.reserve(text.size() / 32 + 8);
  // SD-card fonts: merge every codepoint into the persistent advance table up front. Otherwise
  // each unseen codepoint measured below falls back to an on-demand glyph load from SD.
  renderer.ensureSdCardFontReady(fontId, text.c_str(), 0x01 /* REGULAR */);

  const int maxWidth = width;
  const int spaceWidth = renderer.getSpaceWidth(fontId, EpdFontFamily::REGULAR);
  const int lineHeight = renderer.getLineHeight(fontId);
  linesPerPage = std::max(1, height / lineHeight);

  const char* s = text.c_str();
  const uint32_t n = static_cast<uint32_t>(text.size());
  uint32_t lineStart = 0;
  uint32_t lineEnd = 0;  // one past the last token byte on the current line
  int lineWidth = 0;

  const auto flushLine = [&](uint32_t nextStart) {
    lines.push_back({lineStart, static_cast<uint16_t>(lineEnd - lineStart)});
    lineStart = nextStart;
    lineEnd = nextStart;
    lineWidth = 0;
  };

  uint32_t i = 0;
  while (i < n) {
    const char c = s[i];
    if (c == '\n' || c == '\0') {
      flushLine(i + 1);
      i++;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r') {
      i++;
      continue;
    }

    // Token: run of non-whitespace bytes, capped at the measure buffer.
    const uint32_t tokenStart = i;
    while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n' && s[i] != '\0' &&
           i - tokenStart < MAX_LINE_BYTES) {
      i++;
    }
    // If the byte cap cut the token mid-UTF-8-sequence, back off to the last complete codepoint
    // so measure/draw never see a partial sequence.
    while (i - tokenStart > 1 && (s[i] & 0xC0) == 0x80) i--;
    const uint32_t tokenLen = i - tokenStart;
    const int tokenWidth = measureSpan(renderer, fontId, s + tokenStart, tokenLen);

    if (lineEnd == lineStart) {
      lineStart = tokenStart;
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    } else if (lineWidth + spaceWidth + tokenWidth <= maxWidth &&
               tokenStart + tokenLen - lineStart <= UINT16_MAX) {  // span len must fit Line::len
      lineEnd = tokenStart + tokenLen;
      lineWidth += spaceWidth + tokenWidth;
    } else {
      flushLine(tokenStart);
      lineEnd = tokenStart + tokenLen;
      lineWidth = tokenWidth;
    }

    // An unbreakable token wider than the panel is now alone on the line: split it at the
    // widest fitting UTF-8 boundary and carry the remainder forward.
    while (lineWidth > maxWidth && lineEnd - lineStart > 1) {
      const uint32_t len = lineEnd - lineStart;
      uint32_t lastFit = 0;
      for (uint32_t f = 1; f <= len; f++) {
        if (f == len || (s[lineStart + f] & 0xC0) != 0x80) {  // codepoint boundary
          if (measureSpan(renderer, fontId, s + lineStart, f) > maxWidth) break;
          lastFit = f;
        }
      }
      if (lastFit == 0) {
        // Even a single over-wide glyph must make progress; consume its whole UTF-8 sequence.
        lastFit = 1;
        while (lastFit < len && (s[lineStart + lastFit] & 0xC0) == 0x80) lastFit++;
      }
      const uint32_t rest = lineStart + lastFit;
      lineEnd = rest;
      flushLine(rest);
      lineEnd = rest + (len - lastFit);
      lineWidth = measureSpan(renderer, fontId, s + lineStart, lineEnd - lineStart);
    }
  }
  if (lineEnd > lineStart) flushLine(n);

  // Trim trailing blank lines so the last page is not empty padding.
  while (!lines.empty() && lines.back().len == 0) lines.pop_back();

  totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
}

void PanelTextPages::drawLines(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                               const std::string& text, const int firstLine) const {
  const int lineHeight = renderer.getLineHeight(fontId);
  char buf[MAX_LINE_BYTES + 1];
  const int lastLine = std::min(firstLine + linesPerPage, static_cast<int>(lines.size()));
  for (int i = firstLine; i < lastLine; i++) {
    if (lines[i].len == 0) continue;
    if (lines[i].start + lines[i].len > text.size()) break;  // stale layout; never read past the string
    const size_t len = std::min(static_cast<size_t>(lines[i].len), MAX_LINE_BYTES);
    memcpy(buf, text.c_str() + lines[i].start, len);
    buf[len] = '\0';
    renderer.drawText(fontId, x, y + (i - firstLine) * lineHeight, buf);
  }
}
