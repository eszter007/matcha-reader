#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/css/CssStyle.h"

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  CssTextAlign alignment = CssTextAlign::Justify;

  // Spacing (in pixels)
  int16_t marginTop = 0;
  int16_t marginBottom = 0;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingTop = 0;     // treated same as margin for rendering
  int16_t paddingBottom = 0;  // treated same as margin for rendering
  int16_t paddingLeft = 0;    // treated same as margin for rendering
  int16_t paddingRight = 0;   // treated same as margin for rendering
  int16_t textIndent = 0;
  bool textIndentDefined = false;  // true if text-indent was explicitly set in CSS
  bool textAlignDefined = false;   // true if text-align was explicitly set in CSS
  bool isRtl = false;              // true if resolved direction is RTL
  bool directionDefined = false;   // true if direction was explicitly set in CSS/HTML

  // Combined insets (margin + padding)
  [[nodiscard]] int16_t leftInset() const { return marginLeft + paddingLeft; }
  [[nodiscard]] int16_t rightInset() const { return marginRight + paddingRight; }
  [[nodiscard]] int16_t totalHorizontalInset() const { return leftInset() + rightInset(); }
  [[nodiscard]] int16_t topInset() const { return marginTop + paddingTop; }
  [[nodiscard]] int16_t bottomInset() const { return marginBottom + paddingBottom; }

  // Return a copy with bottom margins/padding zeroed out.
  [[nodiscard]] BlockStyle withoutBottom() const {
    BlockStyle result = *this;
    result.marginBottom = 0;
    result.paddingBottom = 0;
    return result;
  }

  // Return a copy with bottom margins/padding collapsed (max) with the source's.
  // Uses CSS margin collapsing: adjacent parent-child margins resolve to the larger value.
  [[nodiscard]] BlockStyle addBottom(const BlockStyle& source) const {
    BlockStyle result = *this;
    result.marginBottom = std::max(marginBottom, source.marginBottom);
    result.paddingBottom = static_cast<int16_t>(paddingBottom + source.paddingBottom);
    return result;
  }

  enum class CombineAxis : uint8_t {
    Horizontal = 1,  // margins left/right, padding left/right, text-align, text-indent
    Vertical = 2,    // margins top/bottom, padding top/bottom
  };

  // Combine this style's properties with a child style along the specified axis.
  // Properties on the other axis are kept from the child unchanged.
  [[nodiscard]] BlockStyle getCombinedBlockStyle(const BlockStyle& child, CombineAxis axis) const {
    BlockStyle result = child;

    if (axis == CombineAxis::Horizontal) {
      result.marginLeft = static_cast<int16_t>(child.marginLeft + marginLeft);
      result.marginRight = static_cast<int16_t>(child.marginRight + marginRight);
      result.paddingLeft = static_cast<int16_t>(child.paddingLeft + paddingLeft);
      result.paddingRight = static_cast<int16_t>(child.paddingRight + paddingRight);
      if (!child.textIndentDefined && textIndentDefined) {
        result.textIndent = textIndent;
        result.textIndentDefined = true;
      }
      if (!child.textAlignDefined && textAlignDefined) {
        result.alignment = alignment;
        result.textAlignDefined = true;
      }
    } else {
      result.marginTop = std::max(child.marginTop, marginTop);
      result.marginBottom = std::max(child.marginBottom, marginBottom);
      result.paddingTop = static_cast<int16_t>(child.paddingTop + paddingTop);
      result.paddingBottom = static_cast<int16_t>(child.paddingBottom + paddingBottom);
    }

    // Direction is not axis-specific. Inherit from parent when child doesn't define it.
    if (!child.directionDefined && directionDefined) {
      result.isRtl = isRtl;
      result.directionDefined = true;
    }

    return result;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0) {
    BlockStyle blockStyle;
    const float vw = viewportWidth;
    // Vertical margins/padding are honored (paragraph spacing). Horizontal book
    // CSS insets are intentionally ignored so the text column's side margins come
    // ONLY from the reader's screenMargin setting. Otherwise nested wrappers
    // (body + div + p) stack their per-element margins into a narrow, over-
    // margined column the user can't control. Blockquote/list horizontal indents
    // are dropped as a result -- an acceptable trade-off on a small screen.
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, vw);
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, vw);
    blockStyle.marginLeft = 0;
    blockStyle.marginRight = 0;

    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, vw);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, vw);
    blockStyle.paddingLeft = 0;
    blockStyle.paddingRight = 0;

    // For textIndent: if it's a percentage we can't resolve (no viewport width),
    // leave textIndentDefined=false so the space-width fallback in resolveFirstLineIndent() is used
    if (cssStyle.hasTextIndent() && cssStyle.textIndent.isResolvable(vw)) {
      blockStyle.textIndent = cssStyle.textIndent.toPixelsInt16(emSize, vw);
      blockStyle.textIndentDefined = true;
    }
    blockStyle.textAlignDefined = cssStyle.hasTextAlign();
    // User setting overrides CSS, unless "Book's Style" alignment setting is selected
    if (paragraphAlignment == CssTextAlign::None) {
      blockStyle.alignment = blockStyle.textAlignDefined ? cssStyle.textAlign : CssTextAlign::Justify;
    } else {
      blockStyle.alignment = paragraphAlignment;
    }
    // RTL direction from CSS/HTML
    if (cssStyle.hasDirection()) {
      blockStyle.isRtl = (cssStyle.direction == CssTextDirection::Rtl);
      blockStyle.directionDefined = true;
    }
    return blockStyle;
  }
};
