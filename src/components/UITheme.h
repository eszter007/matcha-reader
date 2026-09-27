#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class CoverGridHomeUi;

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  enum class TextVerticalAlignment { TOP, CENTER, BOTTOM };

  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const;
  const BaseTheme& getTheme() const { return currentTheme ? *currentTheme : fallbackTheme; }
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  // Wraps only overflowing text, then aligns the complete line block within bounds.
  static void drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black = true,
                                      EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                      TextVerticalAlignment verticalAlignment = TextVerticalAlignment::CENTER);
  static bool supportsCoverGrid();
  static bool hasCoverGridHome();
  static void drawCoverGridHome(CoverGridHomeUi& home);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  // Any other thumb_<height>.bmp the same book already has, for when the requested size is
  // missing and cannot be regenerated. Empty when there is none. See drawCoverThumb().
  static std::string findSiblingCoverThumb(const std::string& missingThumbPath);
  // True for a concrete generated thumbnail (".../thumb_<height>.bmp"). Distinguishes one from a
  // RAW cover image, which callers must not draw scaled into a card. See HomeActivity.
  static bool isGeneratedThumbPath(const std::string& path);
  // Draws a cover thumbnail at (x, y), scaled to coverHeight. Handles both cover kinds:
  // the BMP thumbnails generated for EPUB/XTC and the JPG/PNG a manga carries as its own
  // first page. Themes that only opened the file as a Bitmap silently drew an empty frame
  // for manga; keeping the format check in one place keeps that from coming back.
  // boxWidth > 0 fits the cover into that width (cropX/cropY apply, BMP only); boxWidth <= 0
  // derives the width from the image's aspect ratio. Returns the drawn width, 0 on failure.
  static int drawCoverThumb(GfxRenderer& renderer, const std::string& coverThumbPath, int x, int y, int coverHeight,
                            int boxWidth = 0, float cropX = 0.0f, float cropY = 0.0f);
  // Source pixel size of a cover thumbnail, for themes that lay out from its aspect ratio
  // before drawing. Same format handling as drawCoverThumb().
  static bool getCoverThumbSize(const std::string& coverThumbPath, int* width, int* height);
  // Fills the box exactly, cropping the overflow instead of letterboxing. Manga covers are
  // raw page images with an arbitrary aspect ratio, so a grid cell wants this rather than the
  // fit-to-height form. Returns true when something was drawn.
  // allowRawDecode=false refuses JPG/PNG sources: decoding a full-size manga page during a
  // render costs ~10s (measured on device). Grids pass false and show the placeholder until
  // the background pass has produced the BMP thumbnail.
  static bool drawCoverThumbFilled(GfxRenderer& renderer, const std::string& coverThumbPath, int x, int y, int boxWidth,
                                   int boxHeight, bool allowRawDecode = true);
  // One book cover as the Library grid paints it: the thumb (or, when there is none, the book
  // icon over the wrapped title so a coverless book is still identifiable), a hairline outline,
  // the drop shadow, and the progress badge. Shared with the Home cover grid so a book looks the
  // same in both. `thumbPath` is already resolved for this box's height; progressPercent < 0
  // draws no badge.
  static void drawBookCover(GfxRenderer& renderer, Rect box, const std::string& thumbPath, const std::string& title,
                            int progressPercent);

  // The one focus treatment across every screen: a dithered grey ring just inside `box`. Grey
  // rather than black so it reads as "the cursor is here" next to content that is itself black,
  // and an outline rather than a wash so it never muddies a cover or a line of text.
  // Even, and it must stay even: the grey dither has period 2 in x and y, so an odd-width band
  // lights two pixel columns at one start parity and one at the other -- which read as a ring
  // thicker on two sides than on the other two.
  static constexpr int FOCUS_RING_WIDTH = 4;
  // Corner radius of that ring, outer edge; the inner edge curves with it. Matches the rounded
  // language the Cover Grid theme's pills and cards use.
  static constexpr int FOCUS_RING_RADIUS = 8;
  static void drawFocusRing(const GfxRenderer& renderer, Rect box, bool on = true);

  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

 private:
  BaseTheme fallbackTheme;
  const ThemeMetrics* currentMetrics = &BaseMetrics::values;
  std::unique_ptr<BaseTheme> currentTheme;
  mutable ThemeMetrics adjustedMetrics;
  mutable bool metricsValid = false;
  mutable bool metricsForTouch = false;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
