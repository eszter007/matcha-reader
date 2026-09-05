#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

namespace BidiUtils {
// Paragraph base direction for the Unicode BiDi algorithm (UAX#9).
// AUTO: scan text for first strong directional character (P2/P3 rules)
// LTR:  force left-to-right paragraph embedding level
// RTL:  force right-to-left paragraph embedding level
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}  // namespace BidiUtils

class FontCacheManager;
class SdCardFont;

#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "Bitmap.h"

// Color representation: uint8_t mapped to 4x4 Bayer matrix dithering levels
// 0 = transparent, 1-16 = gray levels (white to black)
enum Color : uint8_t { Clear = 0x00, White = 0x01, LightGray = 0x05, DarkGray = 0x0A, Black = 0x10 };

class GfxRenderer {
 public:
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB };

  // Logical screen orientation from the perspective of callers
  enum Orientation {
    Portrait,                  // 480x800 logical coordinates (current default)
    LandscapeClockwise,        // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    PortraitInverted,          // 480x800 logical coordinates, inverted
    LandscapeCounterClockwise  // 800x480 logical coordinates, native panel orientation
  };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;  // 8KB chunks to allow for non-contiguous memory

  HalDisplay& display;
  RenderMode renderMode;
  Orientation orientation;
  bool fadingFix;
  uint8_t* frameBuffer = nullptr;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  std::map<int, EpdFontFamily> fontMap;
  // Mutable because ensureSdCardFontReady() is const (called from layout code
  // that holds a const GfxRenderer&) but triggers SD card reads and heap
  // allocation inside the SdCardFont objects. Same pragmatic compromise as
  // fontCacheManager_ below.
  mutable std::map<int, SdCardFont*> sdCardFonts_;
  // The SD font a string measured under `fontId` actually resolves to: that font, or the SD
  // fallback registered for a built-in primary. See the definition for why this matters.
  SdCardFont* sdFontForWarmup(int fontId) const;
  SdCardFont* fallbackSdFont_ = nullptr;
  mutable std::map<int, uint16_t> sdCardFontScales_;  // fontId -> 8.8 fixed point scale (256=1.0x)

  // Mutable because drawText() is const but needs to delegate scan-mode
  // recording to the (non-const) FontCacheManager. Same pragmatic compromise
  // as before, concentrated in a single pointer instead of four fields.
  mutable FontCacheManager* fontCacheManager_ = nullptr;

  // Panel-residue hint for the sleep path: FAST refreshes and grayscale plane
  // writes leave charge that ONE sleep-time HALF pass cannot fully scrub, and a
  // sleep screen freezes that residue on the glass for hours (grain across an
  // image page, the negative ghost of a popup). HALF/FULL passes reset it.
  // Tracked here because the display methods are the one place every refresh
  // funnels through; mutable for the same reason as the fields below (the
  // display path is const).
  mutable bool panelResidue_ = false;

  // Narrower than panelResidue_, and for a different failure: a grayscale pass leaves the
  // controller's RED RAM holding a gray PLANE instead of the previous B/W frame (the SDK tracks
  // the same thing as _redRamSynced). A FAST refresh is a differential update against that RAM,
  // so the next one diffs against a gray plane and the old picture survives underneath the new
  // one. Only a HALF/FULL pass, which drives every pixel to its target, restores a usable
  // baseline. panelResidue_ cannot answer this: any FAST refresh sets it, so it is true almost
  // always and would force a scrub on every image page.
  mutable bool grayPlanesResident_ = false;
  // True while the framebuffer's bytes do not hold what the panel is showing. A build lends those
  // bytes out as inflate scratch (releaseFrameBufferForBuild), so after the loan the e-ink still
  // displays the last page while the buffer behind it holds whatever the inflate left. Anything
  // that composites onto the current frame instead of repainting it has to check this first.
  mutable bool frameBufferContentsStale_ = false;
  // One-shot refresh promotion (see promoteNextRefresh). Mutable because
  // displayBuffer() is const but must consume the flag.
  mutable bool promotedRefreshPending_ = false;
  mutable HalDisplay::RefreshMode promotedRefresh_ = HalDisplay::FAST_REFRESH;
  // Swap in (and clear) the promoted mode, if one is pending.
  HalDisplay::RefreshMode applyPromotedRefresh(HalDisplay::RefreshMode refreshMode) const;

  // Tiled grayscale strip target. When active, drawPixel()/clearScreen()
  // operate on a caller-owned scratch holding one horizontal band of physical
  // rows [_stripY0, _stripY0 + _stripRows) (panelWidthBytes wide) instead of
  // the shared framebuffer, clipping pixels outside the band. Lets grayscale
  // planes render band-by-band straight to the controller without destroying
  // the BW framebuffer (no storeBwBuffer). Mutable because the render path is
  // const. See beginStripTarget()/endStripTarget().
  mutable uint8_t* _stripBuf = nullptr;
  mutable int _stripY0 = 0;
  mutable int _stripRows = 0;
  mutable bool _stripActive = false;

  // CJK UI font fallback map: primary (built-in, Latin-only) UI font id -> a
  // size-matched SD-card font id that carries CJK glyphs. When a string drawn
  // or measured with a mapped primary font contains a CJK codepoint the primary
  // cannot render, the whole string is routed to the mapped fallback so it
  // appears at the same point size as the surrounding UI text. Populated by the
  // app-level SD font setup when an SD family is loaded. See resolveTextFontId().
  std::map<int, int> fallbackFontMap_;

  // If `text` contains a CJK codepoint that `fontId` cannot render and `fontId`
  // has a registered fallback, returns the fallback id; otherwise returns
  // fontId unchanged. The whole string is routed as a unit so each draw/measure
  // call stays single-font (consistent bit depth, metrics, wrapping).
  int resolveTextFontId(int fontId, const char* text, EpdFontFamily::Style style) const;

  // Batch-load `text`'s glyphs into an SD-card font's resident mini tables
  // before a per-glyph measure/draw loop runs. Called when resolveTextFontId
  // redirected a string to the SD fallback: UI screens (file browser, home)
  // draw those strings without the reader's PrewarmScope, and every glyph
  // would otherwise fault through SdCardFont::onGlyphMiss — one .cpfont file
  // open + seek + read per glyph, per redraw, through an 8-slot overflow ring
  // (#2725). One prewarm per string costs a single file open; re-measuring or
  // re-drawing resident glyphs is a RAM-only subset check. No-op for built-in
  // fonts.
  void ensureSdGlyphsResident(int fontId, const char* text, EpdFontFamily::Style style, bool metadataOnly) const;

  void renderChar(const EpdFontFamily& fontFamily, uint32_t cp, int* x, int* y, bool pixelState,
                  EpdFontFamily::Style style) const;
  void freeBwBufferChunks();
  template <Color color>
  void drawPixelDither(int x, int y) const;
  template <Color color>
  void fillArc(int maxRadius, int cx, int cy, int xDir, int yDir) const;
  // Byte-aligned, orientation-specialized rectangle fill. Rotates the rect's
  // two opposing corners into physical-framebuffer space once, then walks each
  // physical row with head-mask / middle memset / tail-mask byte writes — no
  // per-pixel rotation, no per-pixel RMW.
  template <Color color>
  void fillRectImpl(int x, int y, int width, int height) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay)
      : display(halDisplay), renderMode(BW), orientation(Portrait), fadingFix(false) {}
  ~GfxRenderer() { freeBwBufferChunks(); }

  // Setup
  void begin();  // must be called right after display.begin()
  void insertFont(int fontId, EpdFontFamily font);
  // Clears both the flash-font map and any SD-font registration for fontId.
  // Coupled to avoid dangling SdCardFont* in sdCardFonts_ when callers free
  // the underlying SdCardFont and forget the SD-side unregister.
  void removeFont(int fontId) {
    fontMap.erase(fontId);
    sdCardFonts_.erase(fontId);
    sdCardFontScales_.erase(fontId);
  }

  // Prewarm a string through the SAME resolution drawText uses. Prewarming the requested id
  // warms a font the string will never be drawn with: a CJK title asked for at UI_12 draws
  // from the SD font registered for that size, so warming UI_12 leaves every kanji cold and
  // the render then pulls them one at a time through the on-demand miss handler (~16ms per
  // glyph on device, and the 8-slot overflow ring evicts them again within the same title).
  void prewarmText(int fontId, const char* text, uint8_t styleMask) const;

  void setFontCacheManager(FontCacheManager* m) { fontCacheManager_ = m; }
  FontCacheManager* getFontCacheManager() const { return fontCacheManager_; }
  // Batch-prewarm CJK fallback glyphs for a screenful of static strings in ONE
  // SD pass. List screens redraw every visible row on each repaint; without an
  // up-front batch each row's draw prewarms per-string, and under heap
  // pressure (union merge disabled) each string evicts the previous one — SD
  // reads on every repaint forever. Call once when the screen's strings are
  // known (data load); later measures/draws become RAM-only subset hits.
  // No-op when nothing routes to an SD fallback.
  // The getter form fetches strings one at a time (allocation-free — callers
  // must NOT build a concatenated std::string: its bare-new growth aborts on
  // the heap-tight screens this exists for). A null getter result skips that
  // index.
  using TextGetter = const char* (*)(const void* ctx, uint32_t index);
  void prewarmFallbackText(int fontId, TextGetter getter, const void* ctx, uint32_t textCount,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void prewarmFallbackText(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  bool isFontCacheScanning() const;
  const std::map<int, EpdFontFamily>& getFontMap() const { return fontMap; }
  // Point one registered family's glyph fallback at another. Used to give an SD font loaded
  // for a UI size a SIZE-MATCHED next stop (the built-in family of that size) instead of
  // letting it fall through to the global fallback, which is the reader-size companion.
  void setFamilyFallback(int fontId, const EpdFontFamily* fallback) {
    const auto it = fontMap.find(fontId);
    if (it != fontMap.end()) it->second.setFallback(fallback);
  }
  void registerSdCardFont(int fontId, SdCardFont* font) { sdCardFonts_[fontId] = font; }

  // The companion/fallback SD font's advance table (nullptr when none). Measurement paths use
  // it to price glyphs the selected font lacks WITHOUT loading their bitmaps from SD -- the
  // on-demand glyphMissHandler path made indexing do a seek+read per missing glyph.
  void setFallbackSdFont(SdCardFont* font) { fallbackSdFont_ = font; }
  void unregisterSdCardFont(int fontId) { removeFont(fontId); }
  void clearSdCardFonts() {
    sdCardFonts_.clear();
    sdCardFontScales_.clear();
  }
  void registerSdCardFontScale(int fontId, uint16_t scale) { sdCardFontScales_[fontId] = scale; }
  void clearSdCardFontScales() { sdCardFontScales_.clear(); }
  uint16_t getSdCardFontScale(int fontId) const {
    auto it = sdCardFontScales_.find(fontId);
    return (it != sdCardFontScales_.end()) ? it->second : 256;
  }
  const std::map<int, SdCardFont*>& getSdCardFonts() const { return sdCardFonts_; }
  bool isSdCardFont(int fontId) const { return sdCardFonts_.count(fontId) > 0; }
  // Register/clear size-matched CJK UI fallbacks (see fallbackFontMap_).
  // setFallbackFont maps a primary UI font id to an SD font id of the same size.
  void setFallbackFont(int primaryFontId, int fallbackFontId) { fallbackFontMap_[primaryFontId] = fallbackFontId; }
  void clearFallbackFonts() { fallbackFontMap_.clear(); }
  // Ensure SD card font glyph data is loaded for the given text. Called from layout code
  // (which holds a const GfxRenderer&) before measuring word widths. Safe to call on non-SD fonts (no-op).
  // styleMask: bitmask of styles to prepare (bit 0=regular, 1=bold, 2=italic, 3=bold-italic).
  void ensureSdCardFontReady(int fontId, const char* utf8Text, uint8_t styleMask = 0x0F) const;
  void ensureSdCardFontReady(int fontId, const std::deque<std::string>& words, bool includeHyphen,
                             uint8_t styleMask = 0x0F) const;

  // Orientation control (affects logical width/height and coordinate transforms)
  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  // Fading fix control
  void setFadingFix(const bool enabled) { fadingFix = enabled; }

  // Screen ops
  int getScreenWidth() const;
  int getScreenHeight() const;
  void tapToLogical(float nx, float ny, int& outX, int& outY) const;
  void displayBuffer(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  // True when the panel may carry refresh residue (FAST turns, grayscale plane
  // writes) that a static screen -- the sleep image -- would freeze in place.
  // Cleared by any HALF or FULL pass. See panelResidue_.
  [[nodiscard]] bool panelHasResidue() const { return panelResidue_; }
  // True while the controller's RED RAM still holds a grayscale plane rather than a B/W
  // baseline, so a FAST (differential) refresh would diff against it. Callers that are about to
  // put a fresh full-screen image up must take a HALF pass instead. See grayPlanesResident_.
  [[nodiscard]] bool panelHasGrayPlanes() const { return grayPlanesResident_; }
  // One-shot: the next displayBuffer()/displayBufferAsync() call uses `mode`
  // instead of what its caller asked for, then the override clears itself.
  // Lets a closing overlay (the control center's refresh tile) hand a
  // ghost-cleanup waveform to the repaint of whatever screen is underneath,
  // which it cannot reach directly.
  void promoteNextRefresh(const HalDisplay::RefreshMode mode) const {
    promotedRefreshPending_ = true;
    promotedRefresh_ = mode;
  }
  // Non-blocking refresh: starts the waveform and returns so CPU work (e.g.
  // grayscale strip rendering) can overlap the panel's refresh time. The
  // framebuffer must stay untouched until waitRefreshComplete(). Falls back to
  // a blocking refresh when fadingFix is enabled or the panel lacks deferral
  // support. See HalDisplay::displayBufferAsync for the baseline contract.
  void displayBufferAsync(HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  void waitRefreshComplete() const;
  // True when displayBufferAsync() genuinely overlaps: panel defers and
  // fadingFix isn't forcing the blocking path. Callers can skip overlap
  // scaffolding (e.g. whole-plane grayscale buffers) when false.
  bool supportsAsyncRefresh() const;
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  // void displayWindow(int x, int y, int width, int height) const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  // Tiled grayscale strip target. While active, drawPixel() and clearScreen()
  // operate on `scratch` (panelWidthBytes * stripRows bytes, holding physical
  // rows [stripY0, stripY0 + stripRows)) instead of the framebuffer; pixels
  // whose physical row falls outside the band are clipped. The clip is applied
  // after the orientation rotate, so it is orientation-agnostic. Used to render
  // grayscale planes band-by-band without a full second buffer.
  void beginStripTarget(uint8_t* scratch, int stripY0, int stripRows) const;
  void endStripTarget() const;

  // Band culling for tiled grayscale. Takes a glyph bounding box in logical
  // screen coords and returns false only when a strip is active AND the box's
  // physical y-extent lies entirely outside the active band, letting callers
  // skip an expensive bitmap decode. Returns true when no strip is active.
  // Corners are rotated to physical, so it is orientation-aware.
  bool glyphIntersectsStrip(int x0, int y0, int x1, int y1) const;

  // Active pixel-write target for raw writers (DirectPixelWriter) that bypass
  // drawPixel for speed. When a strip target is active these return the band
  // scratch plus its physical-row origin and extent; otherwise the full
  // framebuffer ([0, panelHeight)). Writers subtract the origin and clip to the
  // extent, so they honor tiled-grayscale banding without per-pixel method calls.
  uint8_t* getWriteTarget() const { return _stripActive ? _stripBuf : frameBuffer; }
  int getWriteOriginY() const { return _stripActive ? _stripY0 : 0; }
  int getWriteRows() const { return _stripActive ? _stripRows : panelHeight; }

  // Drawing
  void drawPixel(int x, int y, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const;
  void drawLine(int x1, int y1, int x2, int y2, int lineWidth, bool state) const;
  void drawArc(int maxRadius, int cx, int cy, int xDir, int yDir, int lineWidth, bool state) const;
  void drawRect(int x, int y, int width, int height, bool state = true) const;
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const;
  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool roundTopLeft,
                       bool roundTopRight, bool roundBottomLeft, bool roundBottomRight, bool state) const;
  void maskRoundedRectOutsideCorners(int x, int y, int width, int height, int radius, Color color = Color::White) const;
  void fillRect(int x, int y, int width, int height, bool state = true) const;
  // XOR-invert exactly the given logical rectangle, leaving what is under it legible in reverse
  // video. Self-inverse: inverting the same rectangle a second time restores the original pixels
  // bit for bit, so a moving selection cursor needs no saved copy of what it covered and no
  // re-render of the content beneath it. Edge-exact (head/tail bit masks), unlike the 8px-aligned
  // readFramebufferRegion/writeFramebufferRegion pair.
  void invertRect(int x, int y, int width, int height) const;
  void fillRectDither(int x, int y, int width, int height, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, Color color) const;
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, bool roundTopLeft, bool roundTopRight,
                       bool roundBottomLeft, bool roundBottomRight, Color color) const;
  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height) const;
  // allowUpscale: by default the image only shrinks to fit maxWidth x maxHeight (covers and sleep
  // screens rely on this); pass true to also grow a source smaller than the box up to fill it
  // (manga panel zoom). Only wired through the 1-bit path -- the grayscale path always shrink-fits.
  void drawIcon(const uint8_t bitmap[], int x, int y, int size) const;
  void drawBitmap(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, float cropX = 0, float cropY = 0,
                  bool allowUpscale = false) const;
  void drawBitmap1Bit(const Bitmap& bitmap, int x, int y, int maxWidth, int maxHeight, bool allowUpscale = false) const;
  // Counter-invert content images in the logical framebuffer so output-level
  // dark mode leaves their original polarity unchanged.
  void preserveImagePolarity(int x, int y, int width, int height) const;
  void fillPolygon(const int* xPoints, const int* yPoints, int numPoints, bool state = true) const;

  // Snapshot / restore a screen-coordinate framebuffer region (byte-aligned in
  // panel memory). readFramebufferRegion returns the bytes written to dst, or
  // 0 when the region is empty, offscreen, or exceeds dstCapacity. Pass the
  // same rectangle to writeFramebufferRegion to restore the saved pixels.
  // Enables partial-repaint patterns (e.g. moving a selection highlight)
  // without re-rendering the whole page.
  size_t readFramebufferRegion(int x, int y, int w, int h, uint8_t* dst, size_t dstCapacity) const;
  void writeFramebufferRegion(int x, int y, int w, int h, const uint8_t* src);

  // Text
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                   BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t letterSpacing = 0) const;
  int getTextWidthScaled(int fontId, const char* text, uint16_t scale,
                         EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                         BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t letterSpacing = 0) const;
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO) const;
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t letterSpacing = 0) const;
  void drawTextScaled(int fontId, int x, int y, const char* text, uint16_t scale, bool black = true,
                      EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                      BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO, int8_t letterSpacing = 0) const;
  int getSpaceWidth(int fontId, EpdFontFamily::Style style = EpdFontFamily::REGULAR, int8_t letterSpacing = 0) const;
  /// Returns the total inter-word advance: fp4::toPixel(spaceAdvance + kern(leftCp,' ') + kern(' ',rightCp)).
  /// Using a single snap avoids the +/-1 px rounding error that arises when space advance and kern are
  /// snapped separately and then added as integers.
  int getSpaceAdvance(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style,
                      int8_t letterSpacing = 0) const;
  /// Returns the kerning adjustment between two adjacent codepoints.
  int getKerning(int fontId, uint32_t leftCp, uint32_t rightCp, EpdFontFamily::Style style) const;
  int getTextAdvanceX(int fontId, const char* text, EpdFontFamily::Style style, int8_t letterSpacing = 0) const;
  // Render-truth advance for SHORT strings: resolves glyphs exactly as the draw loops do
  // (on-demand SD load), so layout reservations match drawn ink. See implementation note.
  int getRenderAdvanceX(int fontId, const char* text, EpdFontFamily::Style style) const;
  bool getGlyphMetrics(int fontId, uint32_t cp, EpdFontFamily::Style style, int* left, int* width, int* top,
                       int* height) const;
  int getFontAscenderSize(int fontId) const;
  // The y drawText() will put `text`'s baseline at, given the same `y`. Text containing CJK is
  // routed to a registered SD fallback whose metrics differ from fontId's, and drawText adds a
  // line-height correction on top -- so a caller that has to place something on the SAME line as
  // drawn text (vertical-text ruby aligning a rotated mark with its upright neighbours) cannot
  // get there from getFontAscenderSize(fontId) alone.
  int textBaselineOffset(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int getLineHeight(int fontId) const;
  int getLineHeightScaled(int fontId, uint16_t scale) const;
  int getLineHeight(int fontId, float compression) const;
  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  /// Word-wrap \p text into at most \p maxLines lines, each no wider than
  /// \p maxWidth pixels. Overflowing words and excess lines are UTF-8-safely
  /// truncated with an ellipsis (U+2026).
  std::vector<std::string> wrappedText(int fontId, const char* text, int maxWidth, int maxLines,
                                       EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;

  // Helper for drawing rotated text (90 degrees clockwise, for side buttons)
  void drawTextRotated90CW(int fontId, int x, int y, const char* text, bool black = true,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  void drawTextRotated90CCW(int fontId, int x, int y, const char* text, bool black = true,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  // Renders one codepoint magnified by an integer factor, its INK box placed at
  // (inkLeftX, inkTopY): the glyph bitmap's top-left lands exactly there, and the glyph's own
  // left/top bearings are deliberately NOT added. The caller is positioning the visible mark
  // directly, so a caller that wants the glyph on a baseline applies the bearings itself (see
  // ParsedText::prepareDropCap, which derives inkTop from the ascender and the top bearing) --
  // adding them here as well would shift every drop cap by its own bearing twice.
  //
  // Integer nearest-neighbour block replication: each source pixel becomes a scale x scale
  // square. It needs no intermediate buffer (a 4x 18pt capital would be a ~5KB one) and, on a
  // 1-bit panel, cannot introduce the half-lit edge pixels a resampling filter would, which
  // would just be dithered back to hard black or white anyway.
  //
  // Reports the ink size actually drawn so layout can reserve exactly that column; drawing is
  // skipped and false returned when the font or the glyph is missing.
  bool drawCharUpscaled(int fontId, uint32_t cp, int scale, int inkLeftX, int inkTopY, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR, int* inkWidthOut = nullptr,
                        int* inkHeightOut = nullptr) const;

  // Renders a single upright codepoint flush to the top-right corner of the
  // cell box [cellLeftX, cellLeftX+cellSize] × [cellTopY, cellTopY+cellSize],
  // using the glyph's own metrics. Used for vertical-text small kana.
  void drawCharVerticalCornerTopRight(int fontId, int cellLeftX, int cellTopY, int cellSize, uint32_t cp,
                                      bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  // Renders a single codepoint rotated 90° CCW and positioned inside a vertical
  // cell. shiftType is from Kinsoku::verticalShiftType():
  // 0=no bias, 2=closing bracket, 3=opening bracket, 4=dash/choonpu.
  // Reports where drawCharVerticalRotatedInCell() would put this glyph's ink (top y and
  // height, in cellTopY's coordinate space) without drawing. Vertical layout uses it so a
  // rotated Latin run keeps clear of the brackets it sits between.
  bool verticalPunctInkBox(int fontId, uint32_t cp, EpdFontFamily::Style style, int cellTopY, int cellSize,
                           int shiftType, int* inkTop, int* inkHeight) const;
  void drawCharVerticalRotatedInCell(int fontId, int cellLeftX, int cellTopY, int cellSize, uint32_t cp, int shiftType,
                                     bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                     int* inkTopOut = nullptr, int* inkHeightOut = nullptr) const;
  int getTextHeight(int fontId) const;

  // Grayscale functions
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }
  // Grayscale preconditioning settle pass (no-op on X4). The rect overload
  // takes the gray region in LOGICAL screen coordinates and rotates it to the
  // panel; the no-arg overload settles the full frame. Call after the BW base
  // frame is displayed and before the grayscale planes are written.
  void preconditionGrayscale() const;
  void preconditionGrayscale(int x, int y, int w, int h) const;
  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows (X3: OEM differential base waveform; others: plain display with
  // `fallback`).
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH) const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer() const;

  // Tiled grayscale (X4): stream one band of a plane straight to controller RAM
  // from `scratch` (panelWidthBytes * numRows, physical rows [yStart, yStart+
  // numRows)), bypassing the framebuffer. supportsStripGrayscale() gates use.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* scratch, int yStart, int numRows) const;
  bool supportsStripGrayscale() const;
  // Paper Mono: the base activation is deferred so base + gray planes go out
  // as one waveform. Route the base through displayGrayscaleBase() when true.
  bool combinesGrayscaleBase() const;
  bool storeBwBuffer();  // Returns true if buffer was stored successfully
  // Restore and free the stored buffer. resyncPanelBaseline rewrites the
  // controller's differential baseline to the restored frame — correct after
  // a grayscale render (the glass matches the stored BW plane), WRONG when
  // the glass shows content painted after the store (overlay chrome): the
  // next differential would treat that content as already erased and leave
  // it on the glass. Such callers pass false so the baseline keeps tracking
  // what was last pushed.
  void restoreBwBuffer(bool resyncPanelBaseline = true);
  // Free a stored buffer without restoring it (the page under it changed).
  void discardStoredBwBuffer() { freeBwBufferChunks(); }
  void cleanupGrayscaleWithFrameBuffer() const;

  // Font helpers
  const uint8_t* getGlyphBitmap(const EpdFontData* fontData, const EpdGlyph* glyph) const;

  // Lend the 48 KB framebuffer's bytes to a memory-hungry phase (chapter
  // builds) WITHOUT freeing the allocation, so it never moves and repeated
  // loans cannot fragment the heap. Between release and restore NOTHING may
  // draw or display — the panel keeps showing its last refreshed image. The
  // lent bytes are published via buildscratch::claim() for consumers like
  // InflateStream. restore returns the buffer white, so the caller must
  // redraw the full screen; it cannot fail (no allocation involved).
  void releaseFrameBufferForBuild();
  bool restoreFrameBufferAfterBuild();
  bool hasFrameBuffer() const { return frameBuffer != nullptr; }
  // Whether the framebuffer's CONTENTS can be trusted -- hasFrameBuffer() only answers for the
  // pointer, which comes back intact after a build loan even though the pixels did not.
  bool frameBufferContentsStale() const { return frameBufferContentsStale_; }

  // RAII form of the loan above, for blocking build regions with early-return
  // error paths: restores on scope exit (or explicitly via end()). Display the
  // popup/screen the panel should hold BEFORE constructing one. Constructing
  // while the framebuffer is already lent yields an inert loan (nesting-safe).
  class FrameBufferLoan {
   public:
    explicit FrameBufferLoan(GfxRenderer& renderer);
    ~FrameBufferLoan() { end(); }
    void end();
    FrameBufferLoan(const FrameBufferLoan&) = delete;
    FrameBufferLoan& operator=(const FrameBufferLoan&) = delete;

   private:
    GfxRenderer& renderer_;
    bool active_ = false;
  };

  // Low level functions
  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  uint16_t getDisplayWidth() const { return panelWidth; }
  uint16_t getDisplayHeight() const { return panelHeight; }
  uint16_t getDisplayWidthBytes() const { return panelWidthBytes; }

  // Region cache: take a logical (orientation-aware) rect, hit the framebuffer
  // bytes that the rect can have touched, and pump them in or out of a caller-
  // supplied buffer. Used by HomeActivity to snapshot just the cover tile
  // (~16 KB in Portrait) instead of cloning the entire 48 KB framebuffer.
  //
  // getRegionByteSize: required buffer length for the rect at current orientation.
  // copyRegionToBuffer / copyBufferToRegion: false if `bufSize` is smaller than that.
  size_t getRegionByteSize(int logicalX, int logicalY, int logicalW, int logicalH) const;
  bool copyRegionToBuffer(int logicalX, int logicalY, int logicalW, int logicalH, uint8_t* buf, size_t bufSize) const;
  bool copyBufferToRegion(int logicalX, int logicalY, int logicalW, int logicalH, const uint8_t* buf,
                          size_t bufSize) const;
};
