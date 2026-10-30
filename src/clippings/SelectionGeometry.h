#pragma once

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "components/themes/BaseTheme.h"

namespace selectionGeometry {
// Consume press edges only: display refresh can outlast the input's held-state snapshot.
inline int horizontalIndex(const int selected, const int count, const bool leftPressed, const bool rightPressed,
                           const bool readingOrderRtl = false) {
  if (count <= 0) return -1;
  const int direction = static_cast<int>(rightPressed) - static_cast<int>(leftPressed);
  return std::clamp(selected + (readingOrderRtl ? -direction : direction), 0, count - 1);
}

inline int keepVisible(const int position, const int extent, const int start, const int size) {
  return position < start ? start - position : std::min(0, start + size - position - extent);
}

inline bool atBottomEdge(const Rect safe, const int lineHeight, const int x, const int y) {
  return x >= safe.x && x < safe.x + safe.width && y >= safe.y + safe.height - lineHeight;
}

inline bool nearerWord(const Rect candidate, const Rect current, const int x, const int y) {
  const auto distance = [x, y](const Rect word) {
    return std::pair{std::abs(word.y + word.height / 2 - y), std::max({word.x - x, 0, x - (word.x + word.width)})};
  };
  return distance(candidate) < distance(current);
}

inline bool actionsFitAbove(const Rect safe, const int textTop, const int height, const int gap) {
  return textTop - height - gap >= safe.y;
}

// The bar sits above the selection, or below it (textBottom) when there is no room above.
inline Rect actions(const Rect safe, const int textTop, const int textBottom, const int height, const int gap,
                    const int preferredWidth = 0, const int anchorX = 0) {
  const int width = preferredWidth > 0 ? std::min(safe.width, preferredWidth) : safe.width;
  const int y = actionsFitAbove(safe, textTop, height, gap)
                    ? textTop - height - gap
                    : std::max(safe.y, std::min(textBottom + gap, safe.y + safe.height - height));
  return Rect{std::clamp(anchorX, safe.x, safe.x + safe.width - width), y, width, height};
}

// The action bar's slots are centred an equal pitch apart: the widest slot plus `gap`, so the
// icons are evenly spaced. The card ends `edge` past the first and last slots' own widths.
inline int actionsWidth(const BaseTheme::SelectionActionWidths& widths, const int gap, const int edge) {
  constexpr int count = BaseTheme::SELECTION_ACTION_COUNT;
  const int widest = *std::max_element(widths.begin(), widths.end());
  return 2 * edge + (widths[0] + widths[count - 1]) / 2 + (count - 1) * (widest + gap);
}

// Distance between slot centres in a bar `width` wide: the widest slot plus `gap`, less when the
// bar is too narrow for that.
inline int actionsPitch(const int width, const BaseTheme::SelectionActionWidths& widths, const int gap) {
  constexpr int count = BaseTheme::SELECTION_ACTION_COUNT;
  const int widest = *std::max_element(widths.begin(), widths.end());
  const int room = (width - (widths[0] + widths[count - 1]) / 2) / (count - 1);
  return std::max(0, std::min(widest + gap, room));
}

// Slot `index`'s content box, centred in the bar so both ends get the same air.
inline Rect slot(const Rect actions, const BaseTheme::SelectionActionWidths& widths, const int gap, const int index) {
  constexpr int count = BaseTheme::SELECTION_ACTION_COUNT;
  const int pitch = actionsPitch(actions.width, widths, gap);
  const int span = (widths[0] + widths[count - 1]) / 2 + (count - 1) * pitch;
  const int centre = actions.x + (actions.width - span) / 2 + widths[0] / 2 + index * pitch;
  return Rect{centre - widths[index] / 2, actions.y, widths[index], actions.height};
}

// Slot `index`'s hit area: one pitch wide around its centre, so a tap between two labels goes to
// the nearer one.
inline Rect button(const Rect actions, const BaseTheme::SelectionActionWidths& widths, const int gap, const int index) {
  const Rect content = slot(actions, widths, gap, index);
  const int pitch = actionsPitch(actions.width, widths, gap);
  return Rect{content.x + content.width / 2 - pitch / 2, actions.y, pitch, actions.height};
}

// A selection handle beside a word box: under its leading (start) or trailing (end) edge on a
// line, above the first or below the last cell of a column. Clip selection and the lookup panels
// both place their handles here. A handle always touches its word on the side away from the text,
// even where that runs past the screen edge: pulled inside, it would sit on the word itself (the
// first cell of a column). Across that direction it stays inside the safe area.
inline Rect handle(const Rect word, const bool start, const bool vertical, const bool rtl, const Rect safe,
                   const int size) {
  if (vertical) {
    // The pointed corner meets the highlight's corner, as on a line: the start handle's (bottom
    // right) its top-right corner, the end handle's (top left) its bottom-left one.
    return Rect{std::clamp(start ? word.x + word.width - size : word.x, safe.x, safe.x + safe.width - size),
                start ? word.y - size : word.y + word.height, size, size};
  }
  const bool left = start != rtl;
  return Rect{left ? word.x - size : word.x + word.width, word.y + word.height, size, size};
}

inline BaseTheme::HandleCorner handleCorner(const bool start, const bool vertical, const bool rtl) {
  if (vertical) return start ? BaseTheme::HandleCorner::BottomRight : BaseTheme::HandleCorner::TopLeft;
  return start != rtl ? BaseTheme::HandleCorner::TopRight : BaseTheme::HandleCorner::TopLeft;
}

inline bool contains(const Rect rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

inline int actionAt(const Rect actions, const BaseTheme::SelectionActionWidths& widths, const int gap, const int x,
                    const int y) {
  for (int i = 0; i < BaseTheme::SELECTION_ACTION_COUNT; ++i) {
    if (contains(button(actions, widths, gap, i), x, y)) return i;
  }
  return -1;
}
}  // namespace selectionGeometry
