#pragma once

#include <cstdint>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
// Number is the UNITLESS form ("line-height: 1.4"), which CSS defines as a multiple of the
// element's own font size. It exists because tryInterpretLength() has no unit to key on and
// falls back to Pixels, which is right for every other property but would turn `line-height:
// 1.4` into 1.4 PIXELS. It resolves exactly like Em in toPixels(); the distinction is kept
// because only line-height produces it and a bare number is not a length.
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4, Number = 5 };
enum class CssTextDirection : uint8_t { Ltr = 0, Rtl = 1 };

// Represents a CSS length value with its unit, allowing deferred resolution to pixels
struct CssLength {
  float value = 0.0f;
  CssUnit unit = CssUnit::Pixels;

  CssLength() = default;
  CssLength(const float v, const CssUnit u) : value(v), unit(u) {}

  // Convenience constructor for pixel values (most common case)
  explicit CssLength(const float pixels) : value(pixels) {}

  // Returns true if this length can be resolved to pixels with the given context.
  // Percentage units require a non-zero containerWidth to resolve.
  [[nodiscard]] bool isResolvable(const float containerWidth = 0) const {
    return unit != CssUnit::Percent || containerWidth > 0;
  }

  // Resolve to pixels given the current em size (font line height)
  // containerWidth is needed for percentage units (e.g. viewport width)
  [[nodiscard]] float toPixels(const float emSize, const float containerWidth = 0) const {
    switch (unit) {
      case CssUnit::Em:
      case CssUnit::Rem:
      case CssUnit::Number:
        return value * emSize;
      case CssUnit::Points:
        return value * 1.33f;  // Approximate pt to px conversion
      case CssUnit::Percent:
        return value * containerWidth / 100.0f;
      default:
        return value;
    }
  }

  // Resolve to int16_t pixels (for BlockStyle fields)
  [[nodiscard]] int16_t toPixelsInt16(const float emSize, const float containerWidth = 0) const {
    return static_cast<int16_t>(toPixels(emSize, containerWidth));
  }
};

// Font style options matching CSS font-style property
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1 };

// Text decoration options. Values are bit flags so CSS can combine multiple line decorations.
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1, LineThrough = 2 };

constexpr CssTextDecoration operator|(const CssTextDecoration a, const CssTextDecoration b) {
  return static_cast<CssTextDecoration>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr CssTextDecoration operator&(const CssTextDecoration a, const CssTextDecoration b) {
  return static_cast<CssTextDecoration>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr uint8_t CSS_TEXT_DECORATION_MASK =
    static_cast<uint8_t>(CssTextDecoration::Underline) | static_cast<uint8_t>(CssTextDecoration::LineThrough);

// Display options - only None and Block are relevant for e-ink rendering
enum class CssDisplay : uint8_t { Block = 0, None = 1 };

// Vertical alignment options for inline elements (e.g. superscript/subscript)
enum class CssVerticalAlign : uint8_t { Baseline = 0, Super = 1, Sub = 2 };

// text-emphasis-style (JP bouten/圏点). Fill x shape collapsed into one enum;
// "none" resets an inherited mark. Parsed from text-emphasis(-style) and the
// -epub-/-webkit- prefixed forms EPUB templates ship.
enum class CssTextEmphasis : uint8_t {
  None = 0,
  FilledDot = 1,
  OpenDot = 2,
  FilledCircle = 3,
  OpenCircle = 4,
  FilledSesame = 5,
  OpenSesame = 6,
  FilledTriangle = 7,
  OpenTriangle = 8,
  FilledDoubleCircle = 9,
  OpenDoubleCircle = 10,
};

// font-variant(-caps): only small-caps is rendering-relevant on e-ink
enum class CssFontVariant : uint8_t { Normal = 0, SmallCaps = 1 };

// list-style-type: alphabetic/roman ordered types are approximated as Decimal
enum class CssListStyleType : uint8_t { Disc = 0, Circle = 1, Square = 2, Decimal = 3, NoMarker = 4 };

// Ink polarity for a block, distilled from `color` + `background-color` at PARSE time.
//
// The panel is 1-bit: there is no colour to reproduce, only a contrast POLARITY. So the two
// declarations collapse into this single byte and no RGB ever reaches the rule map or the cache
// -- one real trade book carries 415 rules, and anything stored per rule multiplies by that.
//
// Normal is also the safe fallback in every ambiguous case: it draws the text BLACK regardless of
// what the CSS asked for, so a misjudged rule can never make text invisible. Inverted is only
// chosen when the book explicitly asks for light-on-dark, where dropping the background WOULD
// make the text invisible (the h1 trap: color:#fff on background-color:#a7a9ac).
enum class CssInkMode : uint8_t {
  Normal = 0,    // dark ink straight onto the page
  Inverted = 1,  // light ink on a filled dark panel
};

// page-break-before / -after / -inside, and the modern break-before / -after / -inside aliases.
//
// Three properties, two bits each, packed into ONE byte on CssStyle (see pageBreaks). A rule map
// holds hundreds of rules in RAM during a parse -- 415 on one real trade book -- so a byte per
// property would cost three times as much for no extra information. `auto` is the zero value, so
// an unstyled rule's byte is 0 and the packing needs no separate "unset" encoding per property.
//
// `page` (and the CSS3 spread keywords) map to Always: this reader has one page per screen, so
// "start a new page on the left/right/recto/verso" can only mean "start a new page".
enum class CssPageBreak : uint8_t { Auto = 0, Always = 1, Avoid = 2 };

// Bit offset of each property inside the packed byte. Bits 6-7 are unused.
enum class CssPageBreakSlot : uint8_t { Before = 0, After = 2, Inside = 4 };

// text-transform. Applied to the TEXT at parse time (ChapterHtmlSlimParser::flushPartWordBuffer),
// so nothing about it reaches the layout or the section cache -- what is cached is the transformed
// word. Capitalize acts on the first letter of a word, which is exactly the unit the parser
// flushes, so it needs no word-boundary scan of its own.
enum class CssTextTransform : uint8_t { None = 0, Uppercase = 1, Lowercase = 2, Capitalize = 3 };

constexpr CssPageBreak cssPageBreakGet(const uint8_t packed, const CssPageBreakSlot slot) {
  return static_cast<CssPageBreak>((packed >> static_cast<uint8_t>(slot)) & 0x03u);
}

constexpr uint8_t cssPageBreakWith(const uint8_t packed, const CssPageBreakSlot slot, const CssPageBreak value) {
  const auto shift = static_cast<uint8_t>(slot);
  return static_cast<uint8_t>((packed & ~(0x03u << shift)) | (static_cast<uint8_t>(value) << shift));
}

// Layout of CssStyle::textFlags. Two properties in ONE byte, exactly like CssStyle::pageBreaks and
// for the same reason: a heavy book holds hundreds of rules in RAM during a parse (415 observed),
// so a byte per property costs twice as much for no extra information.
//   bits 0-1  CssTextTransform
//   bit  2    hyphens: none  (the ONLY hyphens value that changes anything -- see hyphensNone())
constexpr uint8_t CSS_TEXT_TRANSFORM_MASK = 0x03;
constexpr uint8_t CSS_HYPHENS_NONE_BIT = 1 << 2;

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint16_t textAlign : 1;
  uint16_t fontStyle : 1;
  uint16_t fontWeight : 1;
  uint16_t textDecoration : 1;
  uint16_t textIndent : 1;
  uint16_t marginTop : 1;
  uint16_t marginBottom : 1;
  uint16_t marginLeft : 1;
  uint16_t marginRight : 1;
  uint16_t paddingTop : 1;
  uint16_t paddingBottom : 1;
  uint16_t paddingLeft : 1;
  uint16_t paddingRight : 1;
  uint16_t imageHeight : 1;
  uint16_t imageWidth : 1;
  uint16_t display : 1;
  uint16_t direction : 1;
  uint16_t verticalAlign : 1;
  uint16_t border : 1;
  uint16_t textEmphasis : 1;
  uint16_t fontVariant : 1;
  uint16_t listStyleType : 1;
  uint16_t fontSize : 1;
  uint16_t inkMode : 1;
  uint16_t pageBreak : 1;
  uint16_t lineHeight : 1;
  uint16_t textTransform : 1;
  uint16_t hyphens : 1;
  uint16_t letterSpacing : 1;

  CssPropertyFlags()
      : textAlign(0),
        fontStyle(0),
        fontWeight(0),
        textDecoration(0),
        textIndent(0),
        marginTop(0),
        marginBottom(0),
        marginLeft(0),
        marginRight(0),
        paddingTop(0),
        paddingBottom(0),
        paddingLeft(0),
        paddingRight(0),
        imageHeight(0),
        imageWidth(0),
        display(0),
        direction(0),
        verticalAlign(0),
        border(0),
        textEmphasis(0),
        fontVariant(0),
        listStyleType(0),
        fontSize(0),
        inkMode(0),
        pageBreak(0),
        lineHeight(0),
        textTransform(0),
        hyphens(0),
        letterSpacing(0) {}

  [[nodiscard]] bool anySet() const {
    return textAlign || fontStyle || fontWeight || textDecoration || textIndent || marginTop || marginBottom ||
           marginLeft || marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight || imageHeight ||
           imageWidth || display || direction || verticalAlign || textEmphasis || fontVariant || listStyleType ||
           fontSize || inkMode || pageBreak || lineHeight || textTransform || hyphens || letterSpacing;
  }

  void clearAll() {
    textAlign = fontStyle = fontWeight = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = direction = verticalAlign = 0;
    textEmphasis = fontVariant = listStyleType = fontSize = inkMode = pageBreak = lineHeight = 0;
    textTransform = hyphens = letterSpacing = 0;
  }
};

// Cache serializes defined flags as uint32_t with bit indices 0..28.
static_assert(sizeof(CssPropertyFlags) <= sizeof(uint32_t),
              "CssPropertyFlags exceeds 32 bits; update cache read/write in CssParser.cpp");

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
struct CssStyle {
  CssTextAlign textAlign = CssTextAlign::Left;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration textDecoration = CssTextDecoration::None;
  CssTextDirection direction = CssTextDirection::Ltr;

  CssLength textIndent;     // First-line indent (deferred resolution)
  CssLength marginTop;      // Vertical spacing before block
  CssLength marginBottom;   // Vertical spacing after block
  CssLength marginLeft;     // Horizontal spacing left of block
  CssLength marginRight;    // Horizontal spacing right of block
  CssLength paddingTop;     // Padding before
  CssLength paddingBottom;  // Padding after
  CssLength paddingLeft;    // Padding left
  CssLength paddingRight;   // Padding right
  CssLength imageHeight;    // Height for img (e.g. 2em) – width derived from aspect ratio when only height set
  CssLength imageWidth;     // Width for img when both or only width set
  // font-size, kept as a raw length: the em base is the READER's font size (not a fixed 16px),
  // and the usable sizes depend on the loaded family, so resolution happens at layout time in
  // cssBlockFontId() (Epub/ReaderFontScale.h). Keyword values are stored as em multiples.
  CssLength fontSize;
  // line-height, kept as a raw length for the same reason font-size is: its em base is the
  // BLOCK's own font size, which is only known once font-size has been resolved to a font id.
  // A unitless number carries CssUnit::Number. Turned into a percentage of the reader's
  // computed leading at layout time by cssLineHeightPercent() (Epub/ReaderFontScale.h), where
  // the user's Line Spacing setting clamps it.
  CssLength lineHeight;
  // letter-spacing (tracking), kept as a raw length like the two above: `0.05em` has to resolve
  // against the BLOCK's own font size, which only exists once font-size has become a font id.
  // Turned into a whole-pixel per-glyph delta at layout time by cssLetterSpacingPx()
  // (Epub/ReaderFontScale.h). `normal` leaves the property unset, which is the same as 0.
  CssLength letterSpacing;
  CssDisplay display = CssDisplay::Block;                       // display property (Block or None)
  CssVerticalAlign verticalAlign = CssVerticalAlign::Baseline;  // vertical-align (super/sub positioning)
  // Border edges bitmask (TOP/RIGHT/BOTTOM/LEFT). A full 4-side mask is a boxed/kakomi block;
  // a TOP-only mask is a separator rule above the block (EBPAJ .k-solid-top). See the
  // border-style parsing in CssParser.cpp.
  static constexpr uint8_t BORDER_TOP = 1 << 0;
  static constexpr uint8_t BORDER_RIGHT = 1 << 1;
  static constexpr uint8_t BORDER_BOTTOM = 1 << 2;
  static constexpr uint8_t BORDER_LEFT = 1 << 3;
  static constexpr uint8_t BORDER_ALL = 0x0F;
  uint8_t borderEdges = 0;
  [[nodiscard]] bool isFullBorderBox() const { return defined.border && borderEdges == BORDER_ALL; }

  CssTextEmphasis textEmphasis = CssTextEmphasis::None;     // JP bouten marks
  CssFontVariant fontVariant = CssFontVariant::Normal;      // small-caps
  CssListStyleType listStyleType = CssListStyleType::Disc;  // list markers
  // color + background-color, distilled to a polarity. See CssInkMode; derived in
  // CssParser::resolveInkMode() from the two colours' luma, which are never stored.
  CssInkMode inkMode = CssInkMode::Normal;
  // page-break-{before,after,inside} packed two bits per property. See CssPageBreak.
  uint8_t pageBreaks = 0;

  [[nodiscard]] CssPageBreak pageBreakBefore() const { return cssPageBreakGet(pageBreaks, CssPageBreakSlot::Before); }
  [[nodiscard]] CssPageBreak pageBreakAfter() const { return cssPageBreakGet(pageBreaks, CssPageBreakSlot::After); }
  [[nodiscard]] CssPageBreak pageBreakInside() const { return cssPageBreakGet(pageBreaks, CssPageBreakSlot::Inside); }
  void setPageBreak(const CssPageBreakSlot slot, const CssPageBreak value) {
    pageBreaks = cssPageBreakWith(pageBreaks, slot, value);
  }

  // text-transform + hyphens, packed. See CSS_TEXT_TRANSFORM_MASK.
  uint8_t textFlags = 0;

  [[nodiscard]] CssTextTransform textTransform() const {
    return static_cast<CssTextTransform>(textFlags & CSS_TEXT_TRANSFORM_MASK);
  }
  void setTextTransform(const CssTextTransform value) {
    textFlags = static_cast<uint8_t>((textFlags & ~CSS_TEXT_TRANSFORM_MASK) | static_cast<uint8_t>(value));
  }
  // Only `hyphens: none` is recorded. `auto`/`manual` are the states the reader is already in
  // (the user's global Hyphenation setting decides), so storing them would be recording a
  // request the book cannot make: a book may SUPPRESS hyphenation, never force it on.
  [[nodiscard]] bool hyphensNone() const { return (textFlags & CSS_HYPHENS_NONE_BIT) != 0; }
  void setHyphensNone(const bool value) {
    textFlags = static_cast<uint8_t>(value ? (textFlags | CSS_HYPHENS_NONE_BIT)
                                           : (textFlags & static_cast<uint8_t>(~CSS_HYPHENS_NONE_BIT)));
  }

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined
  void applyOver(const CssStyle& base) {
    if (base.hasTextAlign()) {
      textAlign = base.textAlign;
      defined.textAlign = 1;
    }
    if (base.hasFontStyle()) {
      fontStyle = base.fontStyle;
      defined.fontStyle = 1;
    }
    if (base.hasFontWeight()) {
      fontWeight = base.fontWeight;
      defined.fontWeight = 1;
    }
    if (base.hasTextDecoration()) {
      textDecoration = base.textDecoration;
      defined.textDecoration = 1;
    }
    if (base.hasTextIndent()) {
      textIndent = base.textIndent;
      defined.textIndent = 1;
    }
    if (base.hasMarginTop()) {
      marginTop = base.marginTop;
      defined.marginTop = 1;
    }
    if (base.hasMarginBottom()) {
      marginBottom = base.marginBottom;
      defined.marginBottom = 1;
    }
    if (base.hasMarginLeft()) {
      marginLeft = base.marginLeft;
      defined.marginLeft = 1;
    }
    if (base.hasMarginRight()) {
      marginRight = base.marginRight;
      defined.marginRight = 1;
    }
    if (base.hasPaddingTop()) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.hasPaddingBottom()) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
    }
    if (base.hasPaddingLeft()) {
      paddingLeft = base.paddingLeft;
      defined.paddingLeft = 1;
    }
    if (base.hasPaddingRight()) {
      paddingRight = base.paddingRight;
      defined.paddingRight = 1;
    }
    if (base.hasImageHeight()) {
      imageHeight = base.imageHeight;
      defined.imageHeight = 1;
    }
    if (base.hasImageWidth()) {
      imageWidth = base.imageWidth;
      defined.imageWidth = 1;
    }
    if (base.hasDisplay()) {
      display = base.display;
      defined.display = 1;
    }
    if (base.hasDirection()) {
      direction = base.direction;
      defined.direction = 1;
    }
    if (base.hasVerticalAlign()) {
      verticalAlign = base.verticalAlign;
      defined.verticalAlign = 1;
    }
    if (base.hasBorder()) {
      borderEdges = base.borderEdges;
      defined.border = 1;
    }
    if (base.hasTextEmphasis()) {
      textEmphasis = base.textEmphasis;
      defined.textEmphasis = 1;
    }
    if (base.hasFontVariant()) {
      fontVariant = base.fontVariant;
      defined.fontVariant = 1;
    }
    if (base.hasListStyleType()) {
      listStyleType = base.listStyleType;
      defined.listStyleType = 1;
    }
    if (base.hasFontSize()) {
      fontSize = base.fontSize;
      defined.fontSize = 1;
    }
    if (base.hasLineHeight()) {
      lineHeight = base.lineHeight;
      defined.lineHeight = 1;
    }
    if (base.hasLetterSpacing()) {
      letterSpacing = base.letterSpacing;
      defined.letterSpacing = 1;
    }
    // Per property, not a whole-byte copy, for the same reason pageBreaks merges per slot: two
    // stylesheets can style one selector, one with text-transform and one with hyphens, and a
    // byte copy would let the later record silently drop the earlier property.
    if (base.hasTextTransform()) {
      setTextTransform(base.textTransform());
      defined.textTransform = 1;
    }
    if (base.hasHyphens()) {
      setHyphensNone(base.hyphensNone());
      defined.hyphens = 1;
    }
    if (base.hasInkMode()) {
      inkMode = base.inkMode;
      defined.inkMode = 1;
    }
    if (base.hasPageBreak()) {
      // Per property, not a whole-byte copy: two files can style the same selector, one with
      // `page-break-after: avoid` and one with `page-break-inside: avoid`, and a byte copy would
      // let the later record silently drop the earlier property. Auto never overwrites, which is
      // also what makes it safe to merge without a defined bit per property.
      if (base.pageBreakBefore() != CssPageBreak::Auto) setPageBreak(CssPageBreakSlot::Before, base.pageBreakBefore());
      if (base.pageBreakAfter() != CssPageBreak::Auto) setPageBreak(CssPageBreakSlot::After, base.pageBreakAfter());
      if (base.pageBreakInside() != CssPageBreak::Auto) setPageBreak(CssPageBreakSlot::Inside, base.pageBreakInside());
      defined.pageBreak = 1;
    }
  }

  [[nodiscard]] bool hasTextAlign() const { return defined.textAlign; }
  [[nodiscard]] bool hasFontStyle() const { return defined.fontStyle; }
  [[nodiscard]] bool hasFontWeight() const { return defined.fontWeight; }
  [[nodiscard]] bool hasTextDecoration() const { return defined.textDecoration; }
  [[nodiscard]] bool hasTextIndent() const { return defined.textIndent; }
  [[nodiscard]] bool hasMarginTop() const { return defined.marginTop; }
  [[nodiscard]] bool hasMarginBottom() const { return defined.marginBottom; }
  [[nodiscard]] bool hasMarginLeft() const { return defined.marginLeft; }
  [[nodiscard]] bool hasMarginRight() const { return defined.marginRight; }
  [[nodiscard]] bool hasPaddingTop() const { return defined.paddingTop; }
  [[nodiscard]] bool hasPaddingBottom() const { return defined.paddingBottom; }
  [[nodiscard]] bool hasPaddingLeft() const { return defined.paddingLeft; }
  [[nodiscard]] bool hasPaddingRight() const { return defined.paddingRight; }
  [[nodiscard]] bool hasImageHeight() const { return defined.imageHeight; }
  [[nodiscard]] bool hasImageWidth() const { return defined.imageWidth; }
  [[nodiscard]] bool hasDisplay() const { return defined.display; }
  [[nodiscard]] bool hasDirection() const { return defined.direction; }
  [[nodiscard]] bool hasVerticalAlign() const { return defined.verticalAlign; }
  [[nodiscard]] bool hasBorder() const { return defined.border; }
  [[nodiscard]] bool hasTextEmphasis() const { return defined.textEmphasis; }
  [[nodiscard]] bool hasFontVariant() const { return defined.fontVariant; }
  [[nodiscard]] bool hasListStyleType() const { return defined.listStyleType; }
  [[nodiscard]] bool hasFontSize() const { return defined.fontSize; }
  // `line-height: normal` (and inherit/initial) leaves this clear, so the block keeps the
  // reader's own leading rather than an assertion the book never made.
  [[nodiscard]] bool hasLineHeight() const { return defined.lineHeight; }
  // Distinguishes "the book said nothing about colour" from "the book set colours and they came
  // out Normal" -- an explicit Normal must be able to cancel an inherited Inverted panel.
  [[nodiscard]] bool hasInkMode() const { return defined.inkMode; }
  // Set when ANY of the three page-break properties resolved to something other than auto; the
  // packed byte then says which. An all-auto declaration leaves the flag clear so it cannot make
  // an otherwise empty rule look worth keeping.
  [[nodiscard]] bool hasPageBreak() const { return defined.pageBreak; }
  // Set only when the declaration named a transform this engine can apply -- `none` included,
  // because an explicit `none` must be able to cancel an ancestor's `uppercase`.
  [[nodiscard]] bool hasTextTransform() const { return defined.textTransform; }
  // Set by any recognised `hyphens` value, so `hyphens: auto` on a child cancels an ancestor's
  // `hyphens: none` instead of inheriting it. The stored bit still says only "suppress or not".
  [[nodiscard]] bool hasHyphens() const { return defined.hyphens; }
  // `letter-spacing: normal` leaves this clear; 0 and unset render identically, but the flag
  // still lets an explicit `normal` stop the cascade from inheriting a parent's tracking.
  [[nodiscard]] bool hasLetterSpacing() const { return defined.letterSpacing; }

  void reset() {
    textAlign = CssTextAlign::Left;
    fontStyle = CssFontStyle::Normal;
    fontWeight = CssFontWeight::Normal;
    textDecoration = CssTextDecoration::None;
    direction = CssTextDirection::Ltr;
    textIndent = CssLength{};
    marginTop = marginBottom = marginLeft = marginRight = CssLength{};
    paddingTop = paddingBottom = paddingLeft = paddingRight = CssLength{};
    imageHeight = imageWidth = CssLength{};
    fontSize = CssLength{};
    lineHeight = CssLength{};
    letterSpacing = CssLength{};
    display = CssDisplay::Block;
    verticalAlign = CssVerticalAlign::Baseline;
    textEmphasis = CssTextEmphasis::None;
    fontVariant = CssFontVariant::Normal;
    listStyleType = CssListStyleType::Disc;
    inkMode = CssInkMode::Normal;
    pageBreaks = 0;
    textFlags = 0;
    defined.clearAll();
  }
};
