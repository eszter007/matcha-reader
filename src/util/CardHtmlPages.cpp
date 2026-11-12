#include "CardHtmlPages.h"

#include <Arduino.h>
#include <Epub/css/CssParser.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include "CrossPointSettings.h"

namespace {
// Same gates as the dictionary's styled pages (DictHtmlPages.cpp), which lay out the same way:
// room to start, and room left after each kept page.
constexpr size_t MIN_FREE_HEAP = 40 * 1024;
constexpr size_t MIN_MAX_ALLOC = 20 * 1024;
constexpr size_t MIN_RETAIN_HEAP = 16 * 1024;
constexpr size_t MIN_RETAIN_ALLOC = 8 * 1024;
// A card side longer than this is cut: one face of a flashcard, not a chapter.
constexpr size_t MAX_PAGES = 16;
constexpr size_t MAX_ELEMENTS = 512;
}  // namespace

bool buildCardHtmlPages(GfxRenderer& renderer, const std::string& xhtmlPath, const std::string& cssPath,
                        const uint16_t viewportWidth, const uint16_t viewportHeight, const int fontId,
                        const bool furigana, std::vector<std::unique_ptr<Page>>& pagesOut) {
  pagesOut.clear();
  if (ESP.getFreeHeap() < MIN_FREE_HEAP || ESP.getMaxAllocHeap() < MIN_MAX_ALLOC) {
    LOG_ERR("CARD", "Low heap for a card page (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  pagesOut.reserve(MAX_PAGES);

  // The deck's CSS from its own file: loadFromStream has no <style> size cap.
  CssParser css{""};
  if (!cssPath.empty() && Storage.exists(cssPath.c_str())) {
    HalFile file;
    if (Storage.openFileForRead("CARD", cssPath, file)) css.loadFromStream(file);
  }

  bool ok = false;
  bool limitHit = false;
  size_t elements = 0;
  {
    // The parser keeps references to these strings.
    const std::string path = xhtmlPath;
    const size_t slash = path.rfind('/');
    const std::string base = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    const std::string imageBase = base;
    auto parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
        // The deck's CSS decides alignment, nested blocks inheriting it; the reader's own
        // alignment setting is for books.
        nullptr, path, renderer, fontId, SETTINGS.getReaderLineCompression(), /*extraParagraphSpacing=*/0,
        static_cast<uint8_t>(CssTextAlign::None), viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
        /*focusReading=*/false, furigana,
        [&pagesOut, &limitHit, &elements](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
          if (limitHit) return;
          const size_t count = page->elements.size();
          if (pagesOut.size() >= MAX_PAGES || count > MAX_ELEMENTS - elements ||
              ESP.getFreeHeap() < MIN_RETAIN_HEAP || ESP.getMaxAllocHeap() < MIN_RETAIN_ALLOC) {
            // Keep what fits: the start of a long card beats no card.
            LOG_ERR("CARD", "Card cut at %u pages (free=%u)", static_cast<unsigned>(pagesOut.size()),
                    ESP.getFreeHeap());
            limitHit = true;
            return;
          }
          elements += count;
          pagesOut.push_back(std::move(page));
        },
        /*embeddedStyle=*/true, base, imageBase, /*imageRendering=*/0, std::vector<std::string>{},
        /*popupFn=*/nullptr, &css);
    if (!parser) {
      LOG_ERR("CARD", "OOM: ChapterHtmlSlimParser");
    } else {
      parser->setTextSpacing(SETTINGS.getCharacterSpacing(), SETTINGS.wordSpacing);
      parser->setParagraphIndentSpaces(0);
      ok = parser->parseAndBuildPages();
    }
  }
  css.releasePools();
  if (!ok && !limitHit) {
    LOG_ERR("CARD", "Could not lay out %s", xhtmlPath.c_str());
    pagesOut.clear();
    return false;
  }
  return !pagesOut.empty();
}
