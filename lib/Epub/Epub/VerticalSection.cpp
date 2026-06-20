#include "VerticalSection.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <cstring>
#include <string>

#include "GfxRenderer.h"

namespace {

constexpr uint8_t VSECTION_FILE_VERSION = 2;
constexpr size_t PARSE_BUFFER_SIZE = 1024;

using RubyRun = VerticalParsedText::RubyRun;

struct TextExtractor {
  // Each paragraph is a sequence of RubyRun entries. Unannotated text has
  // empty ruby; annotated text (<ruby>base<rt>reading</rt></ruby>) maps
  // base -> rubyText.
  std::vector<std::vector<RubyRun>> paragraphs;
  std::vector<RubyRun> currentRuns;
  std::string currentText;
  int blockDepth = 0;
  int skipDepth = -1;

  // Ruby parsing state
  bool inRuby = false;
  bool inRt = false;
  bool inRp = false;
  std::string rubyBase;
  std::string rubyAnnotation;

  static bool isSkipTag(const char* name) {
    return strcasecmp(name, "head") == 0 || strcasecmp(name, "style") == 0 || strcasecmp(name, "script") == 0;
  }

  void flushCurrentText() {
    if (!currentText.empty()) {
      currentRuns.push_back(RubyRun{std::move(currentText), {}});
      currentText.clear();
    }
  }

  void flushParagraph() {
    flushCurrentText();
    if (!currentRuns.empty()) {
      paragraphs.push_back(std::move(currentRuns));
      currentRuns.clear();
    }
  }

  static bool isBlockTag(const char* name) {
    static constexpr const char* blockTags[] = {"p",  "div", "h1", "h2",   "h3",  "h4",
                                                "h5", "h6",  "li", "blockquote", "section", "article"};
    for (const auto* tag : blockTags) {
      if (strcasecmp(name, tag) == 0) return true;
    }
    return false;
  }

  static void XMLCALL startElement(void* userData, const char* name, const char** /*atts*/) {
    auto* self = static_cast<TextExtractor*>(userData);
    if (self->skipDepth >= 0) {
      self->skipDepth++;
      return;
    }
    if (isSkipTag(name)) {
      self->skipDepth = 1;
      return;
    }
    if (isBlockTag(name)) {
      if (self->blockDepth == 0) {
        self->flushParagraph();
      }
      self->blockDepth++;
    }
    if (strcasecmp(name, "ruby") == 0) {
      self->flushCurrentText();
      self->inRuby = true;
      self->rubyBase.clear();
      self->rubyAnnotation.clear();
    } else if (strcasecmp(name, "rt") == 0) {
      self->inRt = true;
      self->rubyAnnotation.clear();
    } else if (strcasecmp(name, "rp") == 0) {
      self->inRp = true;
    }
    if (strcasecmp(name, "br") == 0 || strcasecmp(name, "br/") == 0) {
      if (!self->inRuby) {
        self->currentText.push_back('\n');
      }
    }
  }

  static void XMLCALL endElement(void* userData, const char* name) {
    auto* self = static_cast<TextExtractor*>(userData);
    if (self->skipDepth > 0) {
      self->skipDepth--;
      if (self->skipDepth == 0) self->skipDepth = -1;
      return;
    }
    if (strcasecmp(name, "rp") == 0) {
      self->inRp = false;
      return;
    }
    if (strcasecmp(name, "rt") == 0) {
      self->inRt = false;
      // Emit a RubyRun for the base text accumulated so far with this annotation.
      if (!self->rubyBase.empty()) {
        self->currentRuns.push_back(RubyRun{std::move(self->rubyBase), std::move(self->rubyAnnotation)});
        self->rubyBase.clear();
      }
      self->rubyAnnotation.clear();
      return;
    }
    if (strcasecmp(name, "ruby") == 0) {
      // Flush any remaining base text that had no <rt> (malformed markup).
      if (!self->rubyBase.empty()) {
        self->currentRuns.push_back(RubyRun{std::move(self->rubyBase), {}});
        self->rubyBase.clear();
      }
      self->inRuby = false;
      return;
    }
    if (isBlockTag(name)) {
      self->blockDepth--;
      if (self->blockDepth <= 0) {
        self->blockDepth = 0;
        self->flushParagraph();
      }
    }
  }

  static void XMLCALL characterData(void* userData, const char* s, int len) {
    auto* self = static_cast<TextExtractor*>(userData);
    if (self->skipDepth >= 0) return;
    if (self->inRp) return;
    if (self->inRt) {
      self->rubyAnnotation.append(s, static_cast<size_t>(len));
    } else if (self->inRuby) {
      self->rubyBase.append(s, static_cast<size_t>(len));
    } else {
      self->currentText.append(s, static_cast<size_t>(len));
    }
  }

  static void XMLCALL defaultHandler(void* userData, const char* s, int len) {
    if (len >= 4 && s[0] == '&') {
      auto* self = static_cast<TextExtractor*>(userData);
      std::string entity(s, static_cast<size_t>(len));
      std::string resolved;
      if (entity == "&nbsp;") {
        resolved = " ";
      } else if (entity == "&mdash;") {
        resolved = "\xe2\x80\x94";
      } else if (entity == "&ndash;") {
        resolved = "\xe2\x80\x93";
      } else if (entity == "&hellip;") {
        resolved = "\xe2\x80\xa6";
      } else if (entity == "&amp;") {
        resolved = "&";
      } else if (entity == "&lt;") {
        resolved = "<";
      } else if (entity == "&gt;") {
        resolved = ">";
      } else if (entity == "&quot;") {
        resolved = "\"";
      } else if (entity == "&apos;") {
        resolved = "'";
      } else {
        return;
      }

      if (self->inRp) return;
      if (self->inRt) {
        self->rubyAnnotation.append(resolved);
      } else if (self->inRuby) {
        self->rubyBase.append(resolved);
      } else {
        self->currentText.append(resolved);
      }
    }
  }
};

}  // namespace

bool VerticalSection::extractParagraphsAndLayout(const int fontId, const uint16_t viewportWidth,
                                                  const uint16_t viewportHeight) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_v" + std::to_string(spineIndex) + ".html";

  bool success = false;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      delay(50);
    }
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
    HalFile tmpHtml;
    if (!Storage.openFileForWrite("VSC", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, PARSE_BUFFER_SIZE);
    tmpHtml.close();
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  }

  if (!success) {
    LOG_ERR("VSC", "Failed to stream chapter HTML");
    return false;
  }

  TextExtractor extractor;
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("VSC", "OOM: XML parser");
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  XML_SetDefaultHandlerExpand(parser, TextExtractor::defaultHandler);
  XML_SetUserData(parser, &extractor);
  XML_SetElementHandler(parser, TextExtractor::startElement, TextExtractor::endElement);
  XML_SetCharacterDataHandler(parser, TextExtractor::characterData);

  HalFile htmlFile;
  if (!Storage.openFileForRead("VSC", tmpHtmlPath, htmlFile)) {
    destroyXmlParser(parser);
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  bool parseOk = true;
  int done;
  do {
    void* const buf = XML_GetBuffer(parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("VSC", "OOM: parse buffer");
      parseOk = false;
      break;
    }
    const size_t len = htmlFile.read(buf, PARSE_BUFFER_SIZE);
    if (len == 0 && htmlFile.available() > 0) {
      LOG_ERR("VSC", "File read error");
      parseOk = false;
      break;
    }
    done = htmlFile.available() == 0;
    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      LOG_ERR("VSC", "XML parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
      break;
    }
  } while (!done);

  htmlFile.close();
  destroyXmlParser(parser);
  Storage.remove(tmpHtmlPath.c_str());

  if (!parseOk) return false;

  extractor.flushParagraph();

  if (extractor.paragraphs.empty()) {
    pages.clear();
    pageCount = 0;
    return true;
  }

  VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);
  bool hasRuby = false;
  for (const auto& para : extractor.paragraphs) {
    for (const auto& run : para) {
      if (!run.rubyText.empty()) { hasRuby = true; break; }
    }
    if (hasRuby) break;
  }
  if (hasRuby) {
    layout.setColumnGapPx(renderer.getLineHeight(fontId) * 2 / 3);
  }
  for (const auto& para : extractor.paragraphs) {
    layout.addAnnotatedParagraph(para);
  }
  pages = layout.layoutPages();
  pageCount = static_cast<uint16_t>(pages.size());
  LOG_DBG("VSC", "Laid out %zu paragraphs into %u pages", extractor.paragraphs.size(), pageCount);
  return true;
}

bool VerticalSection::saveToCache(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
  const auto vsectionsDir = epub->getCachePath() + "/vsections";
  Storage.mkdir(vsectionsDir.c_str());

  HalFile file;
  if (!Storage.openFileForWrite("VSC", filePath, file)) {
    return false;
  }

  serialization::writePod(file, VSECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, pageCount);

  for (const auto& page : pages) {
    const auto glyphCount = static_cast<uint32_t>(page.glyphs.size());
    serialization::writePod(file, glyphCount);
    serialization::writePod(file, page.columnCount);
    serialization::writePod(file, page.rowsPerColumn);

    for (const auto& g : page.glyphs) {
      serialization::writePod(file, g.codepoint);
      serialization::writePod(file, g.column);
      serialization::writePod(file, g.row);
      serialization::writePod(file, g.x);
      serialization::writePod(file, g.y);
      serialization::writePod(file, g.paragraphIndex);
      serialization::writePod(file, g.byteOffset);
      serialization::writePod(file, g.rotated);

      if (g.rotated) {
        const auto runLen = static_cast<uint16_t>(g.rotatedRunText.size());
        serialization::writePod(file, runLen);
        if (runLen > 0) {
          file.write(reinterpret_cast<const uint8_t*>(g.rotatedRunText.data()), runLen);
        }
      }

      const auto rubyLen = static_cast<uint16_t>(g.rubyText.size());
      serialization::writePod(file, rubyLen);
      if (rubyLen > 0) {
        file.write(reinterpret_cast<const uint8_t*>(g.rubyText.data()), rubyLen);
      }
    }
  }

  file.close();
  LOG_DBG("VSC", "Cached %u vertical pages", pageCount);
  return true;
}

bool VerticalSection::loadFromCache(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
  HalFile file;
  if (!Storage.openFileForRead("VSC", filePath, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != VSECTION_FILE_VERSION) {
    file.close();
    LOG_DBG("VSC", "Version mismatch: %u vs %u", version, VSECTION_FILE_VERSION);
    clearCache();
    return false;
  }

  int cachedFontId;
  uint16_t cachedWidth, cachedHeight;
  serialization::readPod(file, cachedFontId);
  serialization::readPod(file, cachedWidth);
  serialization::readPod(file, cachedHeight);

  if (cachedFontId != fontId || cachedWidth != viewportWidth || cachedHeight != viewportHeight) {
    file.close();
    LOG_DBG("VSC", "Parameter mismatch, clearing cache");
    clearCache();
    return false;
  }

  uint16_t cachedPageCount;
  serialization::readPod(file, cachedPageCount);

  pages.clear();
  pages.reserve(cachedPageCount);

  for (uint16_t p = 0; p < cachedPageCount; p++) {
    VerticalPage page;
    uint32_t glyphCount;
    serialization::readPod(file, glyphCount);
    serialization::readPod(file, page.columnCount);
    serialization::readPod(file, page.rowsPerColumn);

    page.glyphs.reserve(glyphCount);
    for (uint32_t gi = 0; gi < glyphCount; gi++) {
      VerticalGlyph g;
      serialization::readPod(file, g.codepoint);
      serialization::readPod(file, g.column);
      serialization::readPod(file, g.row);
      serialization::readPod(file, g.x);
      serialization::readPod(file, g.y);
      serialization::readPod(file, g.paragraphIndex);
      serialization::readPod(file, g.byteOffset);
      serialization::readPod(file, g.rotated);

      if (g.rotated) {
        uint16_t runLen;
        serialization::readPod(file, runLen);
        if (runLen > 0) {
          g.rotatedRunText.resize(runLen);
          file.read(reinterpret_cast<uint8_t*>(g.rotatedRunText.data()), runLen);
        }
      }

      uint16_t rubyLen;
      serialization::readPod(file, rubyLen);
      if (rubyLen > 0) {
        g.rubyText.resize(rubyLen);
        file.read(reinterpret_cast<uint8_t*>(g.rubyText.data()), rubyLen);
      }
      page.glyphs.push_back(std::move(g));
    }
    pages.push_back(std::move(page));
  }

  file.close();
  pageCount = cachedPageCount;
  LOG_DBG("VSC", "Loaded %u vertical pages from cache", pageCount);
  return true;
}

bool VerticalSection::loadSectionFile(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
  return loadFromCache(fontId, viewportWidth, viewportHeight);
}

bool VerticalSection::createSectionFile(const int fontId, const uint16_t viewportWidth,
                                         const uint16_t viewportHeight) {
  if (!extractParagraphsAndLayout(fontId, viewportWidth, viewportHeight)) {
    return false;
  }
  return saveToCache(fontId, viewportWidth, viewportHeight);
}

bool VerticalSection::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    return true;
  }
  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("VSC", "Failed to clear cache");
    return false;
  }
  LOG_DBG("VSC", "Cache cleared");
  return true;
}

const VerticalPage* VerticalSection::getPage() const {
  if (currentPage < 0 || currentPage >= static_cast<int>(pages.size())) {
    return nullptr;
  }
  return &pages[currentPage];
}
