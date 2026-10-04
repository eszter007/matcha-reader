#pragma once

#include <ContentProtection.h>
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
  // Optional encrypted-entry accessor. Entries are decoded in memory and stay
  // encrypted at rest. Null when the accessor is not needed or unavailable.
  std::unique_ptr<freeink::content::ContentDecryptor> decryptor;
  // User-presentable reason the encrypted-entry accessor could not be opened.
  std::string protectionError;
  // Set by generateThumbBmp() via the converter's outUnsupported flag; see coverUnsupported().
  mutable bool coverUnsupported_ = false;

  // Two orthogonal sets of optional arguments meet here. shouldCancel/cancelCtx let a long parse
  // abort on a button press (the cover loader). metadataOnly/sharedZip let the library indexer
  // read title and author without the spine, reusing one open ZipFile across every book.
  bool findContentOpfFile(std::string* contentOpfFile, BmpConvertCancelFn shouldCancel = nullptr,
                          void* cancelCtx = nullptr, ZipFile* sharedZip = nullptr) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true,
                       BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr, bool metadataOnly = false,
                       ZipFile* sharedZip = nullptr);
  bool parseTocNcxFile(BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  bool parseTocNavFile(BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  // Extracted from generateThumbBmp() so the cover can also be rendered straight from a
  // parsed href. Cancellable like its caller: shouldCancel is polled during extraction and
  // decode, and partial files are removed on cancel.
  bool generateThumbBmpForCover(int height, const std::string& coverImageHref,
                                BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  bool openProtection();
  void discoverCssFilesFromZip();
  void parseCssFiles() const;

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub() = default;
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false, BmpConvertCancelFn shouldCancel = nullptr,
            void* cancelCtx = nullptr);
  bool loadMetadata(std::string& title, std::string& author);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  // Empty unless the encrypted-entry accessor failed to open.
  const std::string& getProtectionError() const { return protectionError; }
  // True when the last generateThumbBmp() failed because the cover image itself can never be
  // converted by this build (currently: beyond the JPEG decoder's dimension limits) -- as
  // opposed to a low-heap moment, a cancellation, or a decode error, which are all retryable.
  // Lets a caller record "no usable cover" instead of re-attempting it on every pass forever.
  bool coverUnsupported() const { return coverUnsupported_; }
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  // True when the OPF spine declares page-progression-direction="rtl": the publisher's layout
  // is right-to-left, i.e. vertical columns for a CJK book.
  bool pageProgressionRtl() const;
  std::string getCoverBmpPath(bool cropped = false, bool originalThresholds = false) const;
  bool generateCoverBmp(bool cropped = false, bool originalThresholds = false) const;
  std::string getThumbBmpPath() const;
  std::string getThumbBmpPath(int height) const;
  // Whether the book declares a cover image at all. Lets callers tell a permanent "there is
  // nothing to render" apart from a generateThumbBmp() that merely failed this time -- the two
  // deserve opposite handling, and conflating them costs a cover forever.
  bool hasCoverImage() const;
  // shouldCancel is polled during cover extraction and decode; on cancel partial files are
  // removed and the call returns false, so a long thumbnail generation can give way to input.
  bool generateThumbBmp(int height, BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  // Locate the cover without building spine, TOC, or reading caches.
  bool generateThumbBmpFromSource(int height);
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize, bool allowEarlyStop = false,
                                BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
  // Content-access read honouring allowEarlyStop the way the zip path does.
  bool readProtectedItemToStream(const std::string& path, Print& out, bool allowEarlyStop) const;
  // Extract an item to a file on SD. On failure the partial file is removed.
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath,
                         BmpConvertCancelFn shouldCancel = nullptr, void* cancelCtx = nullptr) const;
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
  // Whether the parsed-CSS cache exists (always true for TXT/MD, which have none to build);
  // load() rebuilds it when missing.
  bool hasCssCache() const;
  int resolveHrefToSpineIndex(const std::string& href) const;
};
