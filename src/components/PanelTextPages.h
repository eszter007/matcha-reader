#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

// Plain text paged to fit a floating panel's body: a greedy word-wrap into byte spans of the
// caller's string, so no per-line copies are held. The string must stay alive and unchanged
// between layout() and draw(). '\n' breaks lines (blank lines survive as paragraph spacing);
// a run without spaces, such as Japanese, breaks at the widest fitting codepoint.
class PanelTextPages {
 public:
  // Longest measurable/drawable span. Wrapped lines stay under the panel width (far below this);
  // only pathological unbreakable tokens are split at this cap.
  static constexpr size_t MAX_LINE_BYTES = 191;

  void layout(const GfxRenderer& renderer, int fontId, const std::string& text, int width, int height);
  void clear();

  int pageCount() const { return totalPages; }
  int lineCount() const { return static_cast<int>(lines.size()); }
  int visibleLines() const { return linesPerPage; }
  // Draws page `page` of `text` (the string layout() was given) with its top-left at x, y.
  void draw(const GfxRenderer& renderer, int fontId, int x, int y, const std::string& text, int page) const {
    drawLines(renderer, fontId, x, y, text, page * linesPerPage);
  }
  // Draws one panel's worth of lines starting at line `firstLine`, for a view that scrolls.
  void drawLines(const GfxRenderer& renderer, int fontId, int x, int y, const std::string& text, int firstLine) const;

 private:
  struct Line {
    uint32_t start;
    uint16_t len;
  };
  std::vector<Line> lines;
  int linesPerPage = 1;
  int totalPages = 1;
};
