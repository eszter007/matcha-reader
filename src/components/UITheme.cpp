#include "UITheme.h"

#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <memory>
#include <numeric>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

UITheme UITheme::instance;

UITheme::UITheme() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::ROUNDEDRAFF:
      LOG_DBG("UI", "Using RoundedRaff theme");
      currentTheme = std::make_unique<RoundedRaffTheme>();
      currentMetrics = &RoundedRaffMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
  }
  metricsValid = false;
}

const ThemeMetrics& UITheme::getMetrics() const {
  // hasTouch() can flip once touch init completes after static construction, so the
  // cached copy is refreshed when the flag differs instead of copying the struct per call.
  const bool touch = gpio.hasTouch();
  if (!metricsValid || touch != metricsForTouch) {
    adjustedMetrics = *currentMetrics;
    if (touch) {
      adjustedMetrics.buttonHintsHeight = 0;
    }
    metricsForTouch = touch;
    metricsValid = true;
  }
  return adjustedMetrics;
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight) {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  auto orientation = renderer.getOrientation();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  if (hasTabBar) reservedHeight += metrics.tabBarHeight;
  if (hasButtonHints && orientation != GfxRenderer::Orientation::LandscapeClockwise &&
      orientation != GfxRenderer::Orientation::LandscapeCounterClockwise) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight - extraReservedHeight;
  return UITheme::getInstance().getTheme().getListPageItems(availableHeight, hasSubtitle);
}

// Screen area excluding the button hints
Rect UITheme::getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints, bool hasSideButtonHints) {
  auto orientation = renderer.getOrientation();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect safeArea = Rect{0, 0, screenWidth, screenHeight};
  const ThemeMetrics metrics = getMetrics();
  switch (orientation) {
    case GfxRenderer::Orientation::Portrait:
      if (hasFrontButtonHints) {
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      if (hasFrontButtonHints) {
        safeArea.x += metrics.buttonHintsHeight;
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      if (hasFrontButtonHints) {
        safeArea.y += metrics.buttonHintsHeight;
        safeArea.height -= metrics.buttonHintsHeight;
      }
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      if (hasFrontButtonHints) {
        safeArea.width -= metrics.buttonHintsHeight;
      }
      break;
  }
  return safeArea;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

// Raw covers (a manga's own page image) go through the framebuffer decoder's 4-level Bayer
// screen, which reads darker on the 1-bit panel than the Atkinson dithering baked into the
// cached BMP thumbnails. Lift the midtones so the same cover looks the same on the home screen
// and in the Library (device report: home was noticeably darker on the first pass).
constexpr uint8_t COVER_RAW_LIGHTEN = 48;

bool UITheme::getCoverThumbSize(const std::string& coverThumbPath, int* width, int* height) {
  if (coverThumbPath.empty() || !width || !height) return false;

  if (FsHelpers::hasJpgExtension(coverThumbPath) || FsHelpers::hasPngExtension(coverThumbPath)) {
    const ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(coverThumbPath);
    ImageDimensions dims = {0, 0};
    if (!decoder || !decoder->getDimensions(coverThumbPath, dims) || dims.width <= 0 || dims.height <= 0) return false;
    *width = dims.width;
    *height = dims.height;
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverThumbPath, file)) return false;
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return false;
  *width = bitmap.getWidth();
  *height = bitmap.getHeight();
  return *width > 0 && *height > 0;
}

bool UITheme::drawCoverThumbFilled(GfxRenderer& renderer, const std::string& coverThumbPath, const int x, const int y,
                                   const int boxWidth, const int boxHeight, const bool allowRawDecode) {
  if (coverThumbPath.empty() || boxWidth <= 0 || boxHeight <= 0) return false;

  if (FsHelpers::hasJpgExtension(coverThumbPath) || FsHelpers::hasPngExtension(coverThumbPath)) {
    if (!allowRawDecode) return false;
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(coverThumbPath);
    if (!decoder) return false;
    ImageDimensions dims = {0, 0};
    if (!decoder->getDimensions(coverThumbPath, dims) || dims.width <= 0 || dims.height <= 0) return false;
    RenderConfig config;
    config.x = x;
    config.y = y;
    config.maxWidth = boxWidth;
    config.maxHeight = boxHeight;
    config.useGrayscale = false;
    config.useDithering = true;
    config.lightenBy = COVER_RAW_LIGHTEN;
    config.fillCrop = true;
    config.cropWidth = boxWidth;
    config.cropHeight = boxHeight;
    return decoder->decodeToFramebuffer(coverThumbPath, renderer, config);
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverThumbPath, file)) {
    // Same fallback as drawCoverThumb(): reuse a thumbnail this book has at another height rather
    // than drawing a placeholder for artwork that exists. This is the LIBRARY's draw path -- the
    // grid calls here, the home cards call drawCoverThumb(), and only fixing one of them leaves
    // the cover showing on one screen and missing on the other, which is the reported symptom.
    const std::string sibling = findSiblingCoverThumb(coverThumbPath);
    if (sibling.empty() || !Storage.openFileForRead("HOME", sibling, file)) return false;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getHeight() <= 0) return false;
  // Crop the longer axis so the short one fills the box.
  const float bmpRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
  const float boxRatio = static_cast<float>(boxWidth) / static_cast<float>(boxHeight);
  float cropX = 0.0f, cropY = 0.0f;
  if (bmpRatio > boxRatio) {
    cropX = 1.0f - (boxRatio / bmpRatio);
  } else {
    cropY = 1.0f - (bmpRatio / boxRatio);
  }
  // A crop of a few pixels is not worth the generic (per-pixel, soft-scaled) path: that costs
  // ~250ms per cover against a few ms for the packed 1-bit fast path, which only runs when
  // nothing is cropped. Thumbs are generated at the cell height, so the mismatch is tiny.
  constexpr float NEGLIGIBLE_CROP = 0.03f;
  if (cropX < NEGLIGIBLE_CROP && cropY < NEGLIGIBLE_CROP) {
    cropX = 0.0f;
    cropY = 0.0f;
  }
  // allowUpscale: thumbnails smaller than the cell (small covers, or a cell bigger than the
  // generated size) must grow into it, or the cell shows white strips.
  // A failed decode must reach the caller, or the placeholder is suppressed and the cell shows
  // a blank or half-drawn cover.
  return renderer.drawBitmap(bitmap, x, y, boxWidth, boxHeight, cropX, cropY, /*allowUpscale=*/true);
}

namespace {
// The height in a generated thumbnail's file NAME ("thumb_<height>.bmp"), or -1 when the name is
// not one. The single place that decides what a generated thumbnail is called: the sibling scan
// needs the number and the raw-image guard only needs the verdict, and the two must not drift.
//
// At most 5 digits, so a date-like "thumb_20240101.bmp" is not one of ours and the accumulation
// stays far from overflowing. "thumb_0.bmp" is the shortest name the shape allows, at 11 chars.
long thumbHeightOfName(const std::string& name) {
  if (name.rfind("thumb_", 0) != 0) return -1;
  if (name.size() < 11 || name.compare(name.size() - 4, 4, ".bmp") != 0) return -1;
  const std::string digits = name.substr(6, name.size() - 10);
  if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) return -1;
  // std::accumulate rather than a raw loop: cppcheck's useStlAlgorithm fails the build on the
  // loop, and `pio check` treats every defect as fatal.
  return std::accumulate(digits.begin(), digits.end(), 0L, [](long acc, char c) { return acc * 10 + (c - '0'); });
}
}  // namespace

bool UITheme::isGeneratedThumbPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return thumbHeightOfName(slash == std::string::npos ? path : path.substr(slash + 1)) >= 0;
}

// The LARGEST other thumb_<height>.bmp this book already has, or empty when there is none.
//
// Largest, not first: directory iteration order is not guaranteed, so taking the first match could
// pick thumb_54 over thumb_226 on one boot and the reverse on the next -- inconsistent output from
// the same files, and needless upscaling when a better source was sitting right there. Scaling
// down is also the cheaper and better-looking direction.
//
// Directory scan, so it only runs on the miss path -- the hit path never opens the directory.
std::string UITheme::findSiblingCoverThumb(const std::string& missingThumbPath) {
  // Only a generated-thumb path has siblings worth looking for. Without this a raw cover image,
  // or any other path that happens to reach here, would open and walk a directory for nothing.
  if (!isGeneratedThumbPath(missingThumbPath)) return "";

  const size_t slash = missingThumbPath.find_last_of('/');
  if (slash == std::string::npos) return "";
  // slash == 0 means the file sits at the root: the directory is "/", not the empty string that
  // substr would give, which Storage.open() cannot resolve.
  const std::string dir = slash == 0 ? "/" : missingThumbPath.substr(0, slash);
  const std::string prefix = dir == "/" ? dir : dir + "/";
  const std::string wanted = missingThumbPath.substr(slash + 1);

  // Storage.open(), not openFileForRead(): the latter is for files and fails on a directory
  // (device log: "[HOME] Failed to open file for reading: /.crosspoint/epub_...").
  auto folder = Storage.open(dir.c_str());
  if (!folder || !folder.isDirectory()) return "";
  folder.rewindDirectory();
  char name[64];
  std::string best;
  // -1, not 0: thumbHeightOfName() accepts thumb_0.bmp as a generated thumbnail, so the scan has to
  // be able to pick it when it is the only sibling rather than silently reporting none.
  long bestHeight = -1;
  for (HalFile entry = folder.openNextFile(); entry; entry = folder.openNextFile()) {
    if (entry.isDirectory()) continue;
    name[0] = '\0';
    entry.getName(name, sizeof(name));
    if (name[0] == '\0') continue;
    const std::string candidate(name);
    if (candidate == wanted) continue;  // the one we already know is missing
    const long height = thumbHeightOfName(candidate);
    if (height > bestHeight) {
      bestHeight = height;
      best = prefix + candidate;
    }
  }
  return best;
}

int UITheme::drawCoverThumb(GfxRenderer& renderer, const std::string& coverThumbPath, const int x, const int y,
                            const int coverHeight, const int boxWidth, const float cropX, const float cropY) {
  if (coverThumbPath.empty() || coverHeight <= 0) return 0;

  if (FsHelpers::hasJpgExtension(coverThumbPath) || FsHelpers::hasPngExtension(coverThumbPath)) {
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(coverThumbPath);
    if (!decoder) return 0;
    ImageDimensions dims = {0, 0};
    if (!decoder->getDimensions(coverThumbPath, dims) || dims.width <= 0 || dims.height <= 0) return 0;
    int drawWidth = coverHeight * dims.width / dims.height;
    if (boxWidth > 0 && drawWidth > boxWidth) drawWidth = boxWidth;
    RenderConfig config;
    config.x = x;
    config.y = y;
    config.maxWidth = drawWidth;
    config.maxHeight = coverHeight;
    config.useGrayscale = false;
    config.useDithering = true;
    config.lightenBy = COVER_RAW_LIGHTEN;
    return decoder->decodeToFramebuffer(coverThumbPath, renderer, config) ? drawWidth : 0;
  }

  HalFile file;
  if (!Storage.openFileForRead("HOME", coverThumbPath, file)) {
    // Nothing at this size -- reuse a thumbnail this book already has at another one. drawBitmap
    // rescales (allowUpscale), so a sibling renders correctly at the requested height.
    //
    // Worth doing because a cover can be convertible ONCE and not again: a book whose cover image
    // this build cannot decode still shows artwork wherever an older thumbnail survives, and
    // without this the other screens draw a placeholder for a cover that is sitting right beside
    // the one they asked for. Device report: home drew thumb_226.bmp while the library asked for
    // thumb_207/54 and re-decoded the (undecodable) source on every pass.
    const std::string sibling = findSiblingCoverThumb(coverThumbPath);
    // A sibling is chosen by its name alone, so verify the file is a whole BMP before trusting it.
    if (sibling.empty() || !FsHelpers::hasCompleteBmp("HOME", sibling) ||
        !Storage.openFileForRead("HOME", sibling, file))
      return 0;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return 0;
  const int drawWidth = (boxWidth > 0) ? boxWidth : bitmap.getWidth();
  if (!renderer.drawBitmap(bitmap, x, y, drawWidth, coverHeight, cropX, cropY, /*allowUpscale=*/true)) return 0;
  return drawWidth;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();

  // Layout reservation is hardware-agnostic: pass clockAvailable=true so the
  // reserved height does not depend on whether an RTC is present.
  return (sb.textLaneVisible(true) ? (metrics.statusBarVerticalMargin) : 0) +
         (sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics metrics = UITheme::getInstance().getMetrics();
  const auto sb = SETTINGS.statusBarSpec();
  return sb.showsProgressBar() ? (sb.progressBarHeightPx + metrics.progressBarMarginTop) : 0;
}

// Centered text implementation that takes the safe area into account
void UITheme::drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black, EpdFontFamily::Style style) {
  const int x = screen.x + (screen.width - renderer.getTextWidth(fontId, text, style)) / 2;
  renderer.drawText(fontId, x, y, text, black, style);
}

void UITheme::drawCenteredWrappedText(const GfxRenderer& renderer, Rect bounds, int fontId, const char* text,
                                      int maxLines, bool black, EpdFontFamily::Style style,
                                      TextVerticalAlignment verticalAlignment) {
  if (!text || *text == '\0' || bounds.width <= 0 || bounds.height <= 0 || maxLines <= 0) return;

  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return;

  const int lineLimit = std::min(maxLines, bounds.height / lineHeight);
  if (lineLimit <= 0) return;

  const auto alignedTop = [&](const int textHeight) {
    switch (verticalAlignment) {
      case TextVerticalAlignment::CENTER:
        return bounds.y + (bounds.height - textHeight) / 2;
      case TextVerticalAlignment::BOTTOM:
        return bounds.y + bounds.height - textHeight;
      case TextVerticalAlignment::TOP:
      default:
        return bounds.y;
    }
  };

  if (renderer.getTextWidth(fontId, text, style) <= bounds.width) {
    drawCenteredText(renderer, bounds, fontId, alignedTop(lineHeight), text, black, style);
    return;
  }

  const auto lines = renderer.wrappedText(fontId, text, bounds.width, lineLimit, style);
  int y = alignedTop(static_cast<int>(lines.size()) * lineHeight);
  for (const auto& line : lines) {
    drawCenteredText(renderer, bounds, fontId, y, line.c_str(), black, style);
    y += lineHeight;
  }
}
