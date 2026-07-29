#pragma once

#include <algorithm>
#include <cstdint>

#include "Epub/ReaderFontScale.h"
#include "Epub/css/CssStyle.h"

/**
 * BlockStyle - Block-level styling properties
 */
struct BlockStyle {
  // Upper bound (in em) for any single side's horizontal margin or padding when
  // book CSS insets are honored. Some EPUBs apply huge em-based insets to
  // chapter-opener classes; without a cap, effectiveWidth collapses to 1-2 words
  // per line and justification dumps the remaining space into a single gap.
  static constexpr float MAX_HORIZONTAL_INSET_EM = 2.0f;

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

  // Per-block font from CSS font-size, 0 = "the reader's font" (font ids are never 0, see
  // the sentinel assertions in fontIds.h). Resolved ONCE, at parse time, by cssBlockFontId().
  //
  // Everything that touches this block's geometry must go through resolveFontId() with the
  // reader's font as the base: measurement (ParsedText::layoutAndExtractLines), line height
  // (ChapterHtmlSlimParser::addLineToPage) and drawing (TextBlock::render). A path that
  // measures with one font and draws with another silently mis-positions every word on the
  // line -- the reader-font-substitution bug, one layer down.
  int32_t fontId = 0;

  // Ink polarity from CSS color/background-color (see CssInkMode). Inverted means this block's
  // lines each paint a black panel across the block's PADDING box and draw their text white --
  // the panel rect is emitted by ChapterHtmlSlimParser::addLineToPage, which is the only place
  // that knows the column width and the line height; TextBlock::render only flips the ink.
  CssInkMode inkMode = CssInkMode::Normal;
  bool inkModeDefined = false;  // distinguishes "unstyled" from "explicitly Normal" during cascade

  [[nodiscard]] bool isInverted() const { return inkMode == CssInkMode::Inverted; }

  // CSS page-break-{before,after,inside}, packed exactly as CssStyle::pageBreaks (2 bits each).
  // Only the `avoid` values are read from here, by ChapterHtmlSlimParser::makePages:
  //   - inside:avoid -> lay the block out as a unit if it fits on a page by itself
  //   - after:avoid  -> keep one line of room after it so the next block starts on the same page
  // Deliberately NOT inherited by getCombinedBlockStyle: a nested <p> keeps its own byte, so a
  // container's `avoid` reaches only the blocks the container itself opens (its bare text), not
  // every paragraph inside it. The block, not the container, is this engine's unit of pagination.
  //
  // The `always` values are NOT taken from a block style at all: a container's accumulated style
  // is reused for every text block it opens (blockStyleStack), so a forced break read from here
  // would fire once per paragraph instead of once per element. Those are raised as a one-shot
  // pending break when the element opens/closes -- see ChapterHtmlSlimParser::pendingForcedBreak.
  uint8_t pageBreaks = 0;

  [[nodiscard]] bool keepTogether() const {
    return cssPageBreakGet(pageBreaks, CssPageBreakSlot::Inside) == CssPageBreak::Avoid;
  }
  [[nodiscard]] bool keepWithNext() const {
    return cssPageBreakGet(pageBreaks, CssPageBreakSlot::After) == CssPageBreak::Avoid;
  }

  // Set when this block was created by a <br> element. Used by startNewTextBlock to inject
  // a full line-height gap when the <br> block stays empty (section-break use case).
  // NOT propagated through getCombinedBlockStyle so it can't leak into sibling blocks.
  bool fromBrElement = false;

  // The font this block is laid out AND drawn with. Single source of truth; see fontId.
  [[nodiscard]] int resolveFontId(const int baseFontId) const { return fontId != 0 ? fontId : baseFontId; }

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
      // font-size is inherited in CSS: a <p> inside a container that sets font-size renders at
      // the container's size unless it sets its own. Deliberately only on the Horizontal
      // (parent -> child nesting) axis: the Vertical combine is also used to merge a CLOSED
      // block's style onto the next one, where inheriting would leak a heading's font onto the
      // text that follows it.
      if (child.fontId == 0) {
        result.fontId = fontId;
      }
      // Ink polarity inherits like colour does, and on the same axis and for the same reason as
      // font-size: a <span> inside an h1 panel must stay white-on-black, but a closed panel must
      // not leak onto the paragraph that follows it (that merge uses the Vertical axis).
      if (!child.inkModeDefined && inkModeDefined) {
        result.inkMode = inkMode;
        result.inkModeDefined = true;
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

    // fromBrElement is consumed by startNewTextBlock when an empty <br> block
    // is merged with the following paragraph; never propagate it further.
    result.fromBrElement = false;
    return result;
  }

  // Create a BlockStyle from CSS style properties, resolving CssLength values to pixels
  // emSize is the current font line height, used for em/rem unit conversion
  // paragraphAlignment is the user's paragraphAlignment setting preference
  // honorHorizontalInsets: when false (the "Book side margins" setting is off), the
  // book's horizontal CSS margins/padding are zeroed so the text column's side
  // margins come only from the reader's screenMargin setting -- nested wrappers
  // (body + div + p) otherwise stack per-element insets into a narrow column the
  // user can't control (blockquote/list indents are dropped too). When true, each
  // side is honored but clamped to MAX_HORIZONTAL_INSET_EM per element. Vertical
  // margins/padding (paragraph spacing) are always honored.
  // baseFontId is the reader's font; the block's own font is derived from it and the CSS
  // font-size (0 keeps the reader's font). Passing 0 disables per-block sizing entirely.
  static BlockStyle fromCssStyle(const CssStyle& cssStyle, const float emSize, const CssTextAlign paragraphAlignment,
                                 const uint16_t viewportWidth = 0, const bool honorHorizontalInsets = false,
                                 const int baseFontId = 0) {
    BlockStyle blockStyle;
    blockStyle.fontId = baseFontId != 0 ? cssBlockFontId(cssStyle, baseFontId) : 0;
    const float vw = viewportWidth;
    blockStyle.marginTop = cssStyle.marginTop.toPixelsInt16(emSize, vw);
    blockStyle.marginBottom = cssStyle.marginBottom.toPixelsInt16(emSize, vw);
    blockStyle.paddingTop = cssStyle.paddingTop.toPixelsInt16(emSize, vw);
    blockStyle.paddingBottom = cssStyle.paddingBottom.toPixelsInt16(emSize, vw);

    if (honorHorizontalInsets) {
      const auto maxHorizontalInsetPx = static_cast<int16_t>(emSize * MAX_HORIZONTAL_INSET_EM);
      blockStyle.marginLeft = std::min(cssStyle.marginLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
      blockStyle.marginRight = std::min(cssStyle.marginRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
      blockStyle.paddingLeft = std::min(cssStyle.paddingLeft.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
      blockStyle.paddingRight = std::min(cssStyle.paddingRight.toPixelsInt16(emSize, vw), maxHorizontalInsetPx);
    }

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
    if (cssStyle.hasInkMode()) {
      blockStyle.inkMode = cssStyle.inkMode;
      blockStyle.inkModeDefined = true;
    }
    blockStyle.pageBreaks = cssStyle.pageBreaks;
    // RTL direction from CSS/HTML
    if (cssStyle.hasDirection()) {
      blockStyle.isRtl = (cssStyle.direction == CssTextDirection::Rtl);
      blockStyle.directionDefined = true;
    }
    return blockStyle;
  }
};
