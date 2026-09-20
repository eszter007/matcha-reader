#pragma once

#include <HalStorage.h>
#include <JpegToBmpConverter.h>  // BmpConvertCancelFn

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace manga {

static constexpr uint32_t FORMAT_VERSION = 3;      // v2: per-panel translation. v3: per-block line boxes
static constexpr uint32_t MIN_FORMAT_VERSION = 2;  // volumes converted before line boxes still load

// One printed line of a text block -- a column, for vertical text -- in page-image pixels.
struct LineBox {
  uint16_t x, y, w, h;
};

struct TextBlock {
  uint16_t x, y, w, h;  // only meaningful from v3: earlier converters wrote the far corner here
  std::string text;
  // v3: one box per '\n'-separated segment of `text`, in order. Empty for v2 volumes, and for a
  // block whose OCR gave no usable line geometry -- lookups then fall back to the whole block.
  std::vector<LineBox> lines;
  bool vertical = false;
};

struct Panel {
  uint16_t x, y, w, h;
  std::vector<TextBlock> textBlocks;
  std::string translation;  // pre-extracted English translation, may be empty
  // v3: the page region the panel's crop image shows (the panel plus the converter's margin), for
  // mapping a point on a zoomed panel back onto the page. cropW == 0 when unknown (v2).
  uint16_t cropX = 0, cropY = 0, cropW = 0, cropH = 0;
};

struct PageInfo {
  uint32_t dataOffset;
  uint32_t dataLength;
  uint16_t imgWidth;
  uint16_t imgHeight;
};

struct TocEntry {
  uint32_t pageIndex;
  std::string title;
};

// True for panel-zoom crop filenames (p<page>_<panel>.jpg, e.g. "p3_1.jpg")
// produced by tools/manga_convert/convert_manga.py. These live alongside
// the real page_NNNN.<ext> images in a manga folder and must be excluded
// wherever code scans that folder for actual PAGE images (e.g. picking a
// library cover) -- panel crop names sort alphabetically before
// "page_NNNN" ('0' < 'a'), so a naive "first image" scan picks one of these
// fragments instead of the real first page.
bool isPanelCropFile(const char* name);

class MangaBook {
 public:
  explicit MangaBook(std::string folderPath) : folderPath(std::move(folderPath)) {}

  bool load();

  uint32_t getPageCount() const { return pageCount; }
  const std::string& getFolder() const { return folderPath; }
  std::string getTitle() const;
  const std::string& getAuthor() const { return author; }
  // BCP-47/ISO-639 tag from meta.bin's optional trailer ("ja", "en", ...). Empty when the
  // folder was converted before convert_manga.py wrote one, or the source carried no language.
  const std::string& getLanguage() const { return language; }

  bool loadPagePanels(uint32_t pageIndex, std::vector<Panel>& panels) const;
  // panels.idx is loaded once on open. Any non-empty record means this book has real panel
  // data, even when its current page is a cover/splash that intentionally has no crop file.
  bool hasPanelCropCapability() const {
    return std::any_of(pageIndex.begin(), pageIndex.end(), [](const PageInfo& page) { return page.dataLength != 0; });
  }
  uint16_t getPageImgWidth(uint32_t pageIndex) const;
  uint16_t getPageImgHeight(uint32_t pageIndex) const;

  std::string getPageImagePath(uint32_t pageIndex) const;

  std::string getCachePath() const;

  // Cover thumbnails, mirroring Epub::getThumbBmpPath()/generateThumbBmp() so every book type
  // offers the home screen and the Library the same cheap 1-bit BMP instead of a live page
  // decode. Without one, a manga cover costs a full JPEG decode into the framebuffer on EVERY
  // render (~466ms measured) -- and on an X3, whose larger framebuffer leaves less heap, the
  // 20KB JPEGDEC does not fit at all and the cover silently does not appear.
  std::string getThumbBmpPath() const;  // [HEIGHT]-templated, for UITheme::getCoverThumbPath()
  std::string getThumbBmpPath(int height) const;
  // Renders the cover page into a 2:3 box `height` tall. Caller should free what heap it can
  // first (the converter needs ~52KB); returns false and leaves no file behind on failure.
  bool generateThumbBmp(int height, BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;

  // The folder's cover page: first page_NNNN, else the first non-panel-crop image. Empty when
  // the folder holds no usable image.
  static std::string findCoverImage(const std::string& folderPath, BmpConvertCancelFn shouldCancel = nullptr,
                                    void* cancelCtx = nullptr);

  // Table of contents (toc.idx), optional -- empty when the manga folder
  // has no toc.idx (most don't; SELECT_CHAPTER falls back to percent jump).
  const std::vector<TocEntry>& getToc() const { return tocEntries; }
  bool hasToc() const { return !tocEntries.empty(); }

  static bool isMangaFolder(const std::string& folderPath);

 private:
  std::string folderPath;
  std::string metaTitle;
  std::string author;
  std::string language;
  uint32_t pageCount = 0;
  uint32_t formatVersion = 0;
  std::vector<PageInfo> pageIndex;
  std::vector<std::string> imageFiles;
  std::vector<TocEntry> tocEntries;

  bool loadIndex();
  bool scanImages();
  // Fast path for scanImages(): derives page filenames from pageCount + the converter's canonical
  // page_NNNN.<ext> naming, so opening a book costs two probes instead of a directory walk whose
  // price scales with everything else in the folder. False when the layout is not canonical.
  bool buildCanonicalPageList();
  void loadToc();
  void loadMeta();
};

}  // namespace manga
