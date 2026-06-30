#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

namespace manga {

static constexpr uint32_t FORMAT_VERSION = 2;  // v2 adds per-panel translation string

struct TextBlock {
  uint16_t x, y, w, h;
  std::string text;
};

struct Panel {
  uint16_t x, y, w, h;
  std::vector<TextBlock> textBlocks;
  std::string translation;  // pre-extracted English translation, may be empty
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

  bool loadPagePanels(uint32_t pageIndex, std::vector<Panel>& panels) const;
  uint16_t getPageImgWidth(uint32_t pageIndex) const;
  uint16_t getPageImgHeight(uint32_t pageIndex) const;

  std::string getPageImagePath(uint32_t pageIndex) const;

  std::string getCachePath() const;

  // Table of contents (toc.idx), optional -- empty when the manga folder
  // has no toc.idx (most don't; SELECT_CHAPTER falls back to percent jump).
  const std::vector<TocEntry>& getToc() const { return tocEntries; }
  bool hasToc() const { return !tocEntries.empty(); }

  static bool isMangaFolder(const std::string& folderPath);

 private:
  std::string folderPath;
  uint32_t pageCount = 0;
  std::vector<PageInfo> pageIndex;
  std::vector<std::string> imageFiles;
  std::vector<TocEntry> tocEntries;

  bool loadIndex();
  bool scanImages();
  void loadToc();
};

}  // namespace manga
