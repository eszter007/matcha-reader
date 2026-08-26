#pragma once

#include <ContentAccess.h>
#include <JpegToBmpConverter.h>  // BmpConvertCancelFn
#include <Print.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;

class Epub {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Uniq cache key based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  // Optional alternative item source, set in load(). Null unless a build
  // provides one, in which case every item is read from the ZIP.
  contentaccess::HandlePtr itemSource;
  // User-presentable reason load() refused the book (empty otherwise).
  std::string accessError;
  // Set by generateThumbBmp() via the converter's outUnsupported flag; see coverUnsupported().
  mutable bool coverUnsupported_ = false;

  bool findContentOpfFile(std::string* contentOpfFile, BmpConvertCancelFn shouldCancel = nullptr,
                          void* cancelCtx = nullptr) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true,
                       BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr);
  bool parseTocNcxFile(BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  bool parseTocNavFile(BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  void discoverCssFilesFromZip();
  CssParser::ParseResult parseCssFiles(CssParser::CacheStatus existingCacheStatus) const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
    // create a cache key based on the filepath
    cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
  }
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false, BmpConvertCancelFn shouldCancel = nullptr,
            void* cancelCtx = nullptr);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  // Empty unless load() refused the book because its content is not readable here.
  const std::string& getAccessError() const { return accessError; }
  // True when the last generateThumbBmp() failed because the cover image itself can never be
  // converted by this build (currently: beyond the JPEG decoder's dimension limits) -- as
  // opposed to a low-heap moment, a cancellation, or a decode error, which are all retryable.
  // Lets a caller record "no usable cover" instead of re-attempting it on every pass forever.
  bool coverUnsupported() const { return coverUnsupported_; }
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  // Whether the book declares a cover image at all. Lets callers tell a permanent "there is
  // nothing to render" apart from a generateThumbBmp() that merely failed this time -- the two
  // deserve opposite handling, and conflating them costs a cover forever.
  bool hasCoverImage() const;
  // shouldCancel is polled during cover extraction and decode; on cancel partial files are
  // removed and the call returns false, so a long thumbnail generation can give way to input.
  bool generateThumbBmp(int height, BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize, bool allowEarlyStop = false,
                                BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;
};
