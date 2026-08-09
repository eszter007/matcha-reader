#include "Epub.h"

#include <BmpToBmpConverter.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <Utf8.h>
#include <ZipFile.h>

#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {
class CancellablePrint final : public Print {
 public:
  CancellablePrint(Print& output, BmpConvertCancelFn shouldCancel, void* cancelCtx)
      : output(output), shouldCancel(shouldCancel), cancelCtx(cancelCtx) {}

  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* buffer, size_t size) override {
    if (shouldCancel && shouldCancel(cancelCtx)) return 0;
    return output.write(buffer, size);
  }

 private:
  Print& output;
  BmpConvertCancelFn shouldCancel;
  void* cancelCtx;
};
}  // namespace

bool Epub::findContentOpfFile(std::string* contentOpfFile, BmpConvertCancelFn shouldCancel, void* cancelCtx) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512, false, shouldCancel, cancelCtx)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries,
                           BmpConvertCancelFn shouldCancel, void* cancelCtx) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath, shouldCancel, cancelCtx)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024, false, shouldCancel, cancelCtx)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    if (shouldCancel && shouldCancel(cancelCtx)) return false;
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = std::string_view{coverPageHtml}.substr(pos, endPos - pos);
            // Cover BMP generation supports JPG/PNG only; skip GIF so an unsupported wrapper image
            // does not block a later supported cover reference.
            if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref)) {
              imageRef = ref;
              break;
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + imageRef));
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile(BmpConvertCancelFn shouldCancel, void* cancelCtx) const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  size_t ncxSize;
  if (!getItemSize(tocNcxItem, &ncxSize)) {
    LOG_ERR("EBP", "Could not get size of toc ncx file");
    return false;
  }

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  // Stream the decompressed NCX straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024, false, shouldCancel, cancelCtx)) {
    LOG_ERR("EBP", "Could not read toc ncx file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile(BmpConvertCancelFn shouldCancel, void* cancelCtx) const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  size_t navSize;
  if (!getItemSize(tocNavItem, &navSize)) {
    LOG_ERR("EBP", "Could not get size of toc nav file");
    return false;
  }

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  // Stream the decompressed nav document straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNavItem, navParser, 1024, false, shouldCancel, cancelCtx)) {
    LOG_ERR("EBP", "Could not read toc nav file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  ZipFile zf(filepath);

  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }

        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }

        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }

        LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
        cssFiles.push_back(std::string{filePath});
      })) {
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
  }
}

void Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  // See if we have a cached version of the CSS rules
  if (cssParser->hasCache()) {
    LOG_DBG("EBP", "CSS cache exists, skipping parseCssFiles");
    return;
  }

  // Some converters emit one byte-identical stylesheet per chapter (100+ .css
  // entries), and each parse costs a zip locate plus an SD extract round-trip.
  // Map every CSS path to its central-directory (CRC32, compressed size) in a
  // single scan and parse only the first of each identical pair. Rules merge
  // into one global set, so dropping exact duplicates cannot lose styles. A
  // path that never matches a directory entry keeps key 0 and always parses.
  std::vector<uint64_t> dedupKeys(cssFiles.size(), 0);
  if (cssFiles.size() > 1) {
    std::unordered_map<std::string, size_t> pathToIndex;
    pathToIndex.reserve(cssFiles.size());
    for (size_t i = 0; i < cssFiles.size(); i++) {
      pathToIndex.emplace(FsHelpers::normalisePath(cssFiles[i]), i);
    }
    ZipFile(filepath).enumerateFileEntries([&](std::string_view entryPath, uint32_t crc32, uint32_t compressedSize) {
      if (!FsHelpers::hasCssExtension(entryPath)) {
        return;
      }
      const auto it = pathToIndex.find(std::string{entryPath});
      if (it != pathToIndex.end()) {
        dedupKeys[it->second] = (static_cast<uint64_t>(crc32) << 32) | compressedSize;
      }
    });
  }
  std::vector<uint64_t> seenKeys;
  seenKeys.reserve(cssFiles.size());
  size_t skippedDuplicates = 0;

  // No cache yet - parse CSS files ONE AT A TIME, flushing each file's rules to the cache and
  // clearing the map before the next file. Keeping every parsed file's rules resident was what
  // starved the later files: one heavy 818-rule stylesheet left ~23KB free against the 64KB the
  // next parse needs, so the tail files were skipped on every single open and the cache -- being
  // partial -- was never written, re-triggering this whole parse at each book open.
  bool skippedFileForHeap = false;
  size_t totalRulesParsed = 0;
  bool cacheOk = cssParser->beginCacheAppend();
  if (!cacheOk) {
    LOG_ERR("EBP", "Could not open CSS cache for writing; parsing without caching");
  }
  // No cache yet - parse CSS files
  for (size_t cssIndex = 0; cssIndex < cssFiles.size(); cssIndex++) {
    const auto& cssPath = cssFiles[cssIndex];
    const uint64_t dedupKey = dedupKeys[cssIndex];
    if (dedupKey != 0) {
      if (std::find(seenKeys.begin(), seenKeys.end(), dedupKey) != seenKeys.end()) {
        skippedDuplicates++;
        continue;
      }
      seenKeys.push_back(dedupKey);
    }
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      skippedFileForHeap = true;
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    HalFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    // Explicitly close() file before reopening for reading
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    cssParser->loadFromStream(tempCssFile);
    // Explicitly close() file before calling Storage.remove()
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());

    // Flush this file's rules to the cache and free the map before the next file starts.
    totalRulesParsed += cssParser->ruleCount();
    if (cacheOk) cacheOk = cssParser->appendRulesToCache();
    cssParser->clear();
  }

  // A parse that skipped whole files (heap) or dropped selectors mid-file (wasHeapTruncated,
  // latched across files) is partial: caching it would freeze the degraded rule set permanently,
  // so discard and let the next open retry. Consumers (section builds) load rules from this
  // cache, so on discard this session's sections build with reduced styling -- same outcome as
  // the skipped files themselves, and self-healing once a later open completes the parse.
  const bool partial = skippedFileForHeap || cssParser->wasHeapTruncated();
  if (partial) {
    LOG_ERR("EBP", "CSS parse incomplete on low heap; discarding partial cache for retry next open");
  }
  if (!cssParser->endCacheAppend(/*discard=*/partial || !cacheOk) && !partial && cacheOk) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
  }

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files (%zu identical duplicates skipped)", cssParser->ruleCount(),
          cssFiles.size(), skippedDuplicates);
  cssParser->clear();
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss, BmpConvertCancelFn shouldCancel,
                void* cancelCtx) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());
  if (shouldCancel && shouldCancel(cancelCtx)) return false;

  // Open the optional content accessor before any parsing. Ships as a no-op
  // here, so this always succeeds with a null handle and costs one call.
  if (!contentaccess::open(filepath, &itemSource, &accessError)) return false;

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser.reset(new CssParser(cachePath));

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Rebuild CSS cache when missing or when cache version changed. validateCache streams
      // through the file WITHOUT materializing the rule map -- the old loadFromCache-as-check
      // built the full table (90KB+ for a heavy book) just to throw it away, which is exactly
      // what used to abort on the post-wake heap and cascade into delete/re-parse/section-nuke.
      // It removes the file itself on a version mismatch, like loadFromCache does.
      if (!cssParser->hasCache() || !cssParser->validateCache()) {
        LOG_DBG("EBP", "CSS rules cache missing or stale, attempting to parse CSS files");
        cssParser->deleteCache();

        BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
        if (!parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false, shouldCancel, cancelCtx)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        } else {
          discoverCssFilesFromZip();
        }
        bookMetadataCache.reset();
        parseCssFiles();
        bookMetadataCache.reset(new BookMetadataCache(cachePath));
        if (!bookMetadataCache->load()) {
          LOG_ERR("EBP", "Failed to reload cache after CSS rebuild");
          return false;
        }
        // Invalidate section caches so they are rebuilt with the new CSS
        Storage.removeDir((cachePath + "/sections").c_str());
      }
      // Nothing is kept resident here: validateCache never materializes rules, and
      // parseCssFiles clears the map after each per-file cache flush. Only horizontal section
      // BUILDS consume the table, and Section.cpp reloads it from the on-disk cache right
      // before parsing. (Historically a 608-rule table held here in thousands of small string
      // allocations fragmented the heap enough that vertical section builds lost their ~33KB
      // contiguous stream reserve and silently truncated long chapters into sparse pages.)
      cssParser->clear();
    }
    // Release the resolved CSS rule map: it is only needed transiently while building
    // section caches, and createSectionFile reloads it from cache on demand. Holding it
    // resident pins tens of KB for the whole reading session (more on warm resume into
    // an already-cached chapter, where createSectionFile never runs to clear it).
    cssParser->clear();
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }
  if (shouldCancel && shouldCancel(cancelCtx)) return false;

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata, true, shouldCancel, cancelCtx)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  if (!skipLoadingCss) discoverCssFilesFromZip();
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;
  if (shouldCancel && shouldCancel(cancelCtx)) return false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile(shouldCancel, cancelCtx);
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile(shouldCancel, cancelCtx);
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  if (shouldCancel && shouldCancel(cancelCtx)) return false;
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!skipLoadingCss) {
    // Parse CSS before reloading book.bin to leave more heap for CSS rule-table growth.
    bookMetadataCache.reset();
    parseCssFiles();
    Storage.removeDir((cachePath + "/sections").c_str());
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true. hasContent(), not exists(): every failure below removes the
  // partial file, but openFileForWrite() creates it before the conversion runs, so a reset or
  // power loss in that window leaves a 0-byte cover that exists() would trust forever.
  if (Storage.hasContent(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from JPG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    HalFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverJpg, 1024);
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }

    HalFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      return false;
    }
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    coverBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode)", cropped ? "cropped" : "fit");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    HalFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    readItemContentsToStream(coverImageHref, coverPng, 1024);
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      return false;
    }

    HalFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
      return false;
    }
    const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    coverBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath(cropped).c_str());
    }
    LOG_DBG("EBP", "Generated BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  if (FsHelpers::hasBmpExtension(coverImageHref)) {
    LOG_DBG("EBP", "Using BMP cover image directly");
    const std::string outPath = getCoverBmpPath(cropped);

    HalFile coverBmp;
    if (!Storage.openFileForWrite("EBP", outPath, coverBmp)) return false;
    const bool copied = readItemContentsToStream(coverImageHref, coverBmp, 1024);
    coverBmp.close();

    if (!copied) {
      Storage.remove(outPath.c_str());
      return false;
    }

    // Sanity-check header so we don't cache a non-BMP blob forever.
    HalFile verify;
    if (!Storage.openFileForRead("EBP", outPath, verify)) {
      Storage.remove(outPath.c_str());
      return false;
    }
    uint8_t sig[2];
    const bool ok = verify.read(sig, sizeof(sig)) == static_cast<int>(sizeof(sig)) && sig[0] == 'B' && sig[1] == 'M';
    verify.close();
    if (!ok) {
      LOG_ERR("EBP", "Cover item has .bmp extension but is not a BMP, skipping");
      Storage.remove(outPath.c_str());
      return false;
    }

    return true;
  }

  LOG_ERR("EBP", "Cover image is not a supported format, skipping");
  return false;
}

bool Epub::hasCoverImage() const {
  return bookMetadataCache && bookMetadataCache->isLoaded() && !bookMetadataCache->coreMetadata.coverItemHref.empty();
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Epub::generateThumbBmp(int height, BmpConvertCancelFn shouldCancel, void* cancelCtx) const {
  // Already generated, return true. hasContent(), not exists(): every branch below opens the
  // destination for writing before it knows the conversion will succeed, so a failed, cancelled
  // or power-interrupted run leaves a 0-byte file. Answering "already generated" for that husk
  // made the failure permanent AND invisible -- the caller drew a placeholder forever while
  // this function reported success and never attempted the cover again.
  if (Storage.hasContent(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  } else if (FsHelpers::hasJpgExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from JPG cover image");
    const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

    HalFile coverJpg;
    if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }
    CancellablePrint cancellableCover(coverJpg, shouldCancel, cancelCtx);
    if (!readItemContentsToStream(coverImageHref, cancellableCover, 1024)) {
      coverJpg.close();
      Storage.remove(coverJpgTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverJpg.close();

    if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
      return false;
    }

    HalFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      return false;
    }
    // Generate 1-bit BMP for fast home screen rendering (no gray passes needed).
    // Cover the 2:3 cover box and let the converter trim the overflow: fitting by height alone
    // left covers narrower than the box with a white strip beside them, and any leftover crop
    // at draw time costs the slow per-pixel path (device report).
    int THUMB_TARGET_WIDTH = (height * 2) / 3;
    int THUMB_TARGET_HEIGHT = height;
    const bool success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(
        coverJpg, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT, shouldCancel, cancelCtx);
    // Explicitly close() files before calling Storage.remove()
    coverJpg.close();
    thumbBmp.close();
    Storage.remove(coverJpgTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image");
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from JPG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else if (FsHelpers::hasPngExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
    const auto coverPngTempPath = getCachePath() + "/.cover.png";

    HalFile coverPng;
    if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
      return false;
    }
    CancellablePrint cancellableCover(coverPng, shouldCancel, cancelCtx);
    if (!readItemContentsToStream(coverImageHref, cancellableCover, 1024)) {
      coverPng.close();
      Storage.remove(coverPngTempPath.c_str());
      return false;
    }
    // Explicitly close() file before reopening for reading
    coverPng.close();

    if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
      return false;
    }

    HalFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      return false;
    }
    int THUMB_TARGET_WIDTH = (height * 2) / 3;  // 2:3 cover box, see the JPG branch above
    int THUMB_TARGET_HEIGHT = height;
    const bool success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(
        coverPng, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT, shouldCancel, cancelCtx);
    // Explicitly close() files before calling Storage.remove()
    coverPng.close();
    thumbBmp.close();
    Storage.remove(coverPngTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from PNG cover image, success: %s", success ? "yes" : "no");
    return success;
  } else if (FsHelpers::hasBmpExtension(coverImageHref)) {
    LOG_DBG("EBP", "Generating thumb BMP from BMP cover image");
    // The extracted source SURVIVES a failed attempt, and only a complete extraction is kept.
    // Unpacking a large cover costs seconds (device log: 7s for 35KB -> 43KB), and a cancelled
    // conversion used to delete it, so every retry paid that again and got interrupted at the
    // same point -- the cover could never be produced, however many times it was attempted.
    // Written via .part + rename so presence of the final path means "complete": a half-written
    // husk is never mistaken for a cached source, the same reason section builds use a tmp name.
    const auto sourcePath = getCachePath() + "/.cover-source.bmp";
    const auto sourceTmpPath = sourcePath + ".part";

    HalFile sourceBmp;
    if (!Storage.hasContent(sourcePath.c_str())) {
      if (!Storage.openFileForWrite("EBP", sourceTmpPath, sourceBmp)) return false;
      CancellablePrint cancellableCover(sourceBmp, shouldCancel, cancelCtx);
      const bool extracted = readItemContentsToStream(coverImageHref, cancellableCover, 1024);
      sourceBmp.close();
      if (!extracted) {
        Storage.remove(sourceTmpPath.c_str());
        return false;
      }
      Storage.remove(sourcePath.c_str());  // a stale 0-byte husk would block the rename
      if (!Storage.rename(sourceTmpPath.c_str(), sourcePath.c_str())) {
        Storage.remove(sourceTmpPath.c_str());
        return false;
      }
    }

    if (!Storage.openFileForRead("EBP", sourcePath, sourceBmp)) {
      Storage.remove(sourcePath.c_str());
      return false;
    }
    HalFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      sourceBmp.close();
      return false;
    }
    const bool success = BmpToBmpConverter::bmpFileTo1BitBmpStreamWithSize(sourceBmp, thumbBmp, (height * 2) / 3,
                                                                           height, shouldCancel, cancelCtx);
    sourceBmp.close();
    thumbBmp.close();

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from BMP cover image");
      Storage.remove(getThumbBmpPath(height).c_str());
      return false;  // keep the extracted source: the next attempt resumes from the costly part
    }
    Storage.remove(sourcePath.c_str());  // consumed
    return true;
  } else {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
  }

  // No sentinel file. This used to write an empty .bmp "to avoid generation attempts in the
  // future", which recorded a failure as a finished artifact -- the exact pattern the project
  // forbids. Whether there is anything to generate is a question hasCoverImage() answers
  // truthfully and for free; callers that want to stop retrying must ask it (see
  // HomeActivity::loadRecentCovers and RecentBooksActivity::runCoverJob) rather than reading a
  // fake file back.
  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  if (contentaccess::handles(itemSource, path)) {
    return contentaccess::readToBytes(itemSource, path, size, trailingNullByte);
  }

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                    const bool allowEarlyStop, BmpConvertCancelFn shouldCancel, void* cancelCtx) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  if (shouldCancel) {
    CancellablePrint cancellable(out, shouldCancel, cancelCtx);
    if (contentaccess::handles(itemSource, path)) {
      return contentaccess::readToStream(itemSource, path, cancellable);
    }
    return ZipFile(filepath).readFileToStream(path.c_str(), cancellable, chunkSize, allowEarlyStop);
  }

  if (contentaccess::handles(itemSource, path)) {
    return contentaccess::readToStream(itemSource, path, out);
  }

  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize, allowEarlyStop);
}

bool Epub::extractItemToFile(const std::string& itemHref, const std::string& destPath) const {
  HalFile out;
  if (!Storage.openFileForWrite("EBP", destPath, out)) {
    return false;
  }
  // Extraction targets are images (hundreds of KB): SD write throughput is dominated by
  // per-chunk latency, so 4KB chunks measured ~100KB/s on an X3 device log (9.3s for one
  // 928KB illustration). 16KB chunks cut the round-trips 4x. readFileToStream allocates
  // 2x chunkSize transiently, so gate on the heap and keep 4KB as the tight-heap fallback.
  const size_t chunkSize = ESP.getMaxAllocHeap() >= 96 * 1024 ? 16384 : 4096;
  const bool ok = readItemContentsToStream(itemHref, out, chunkSize);
  out.flush();
  out.close();
  if (!ok) {
    Storage.remove(destPath.c_str());
  }
  return ok;
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return getSpineItem(spineIndex).cumulativeSize; }

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
