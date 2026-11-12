#pragma once

#include <Epub/Page.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class GfxRenderer;

// Lays out one side of an Anki card -- an XHTML document on the SD card, with the deck's CSS in
// a separate file and its images beside it -- into pages of viewportWidth x viewportHeight, the
// way the reader lays out a chapter. Draw a page with Page::render at any origin. False when
// the page cannot be laid out (low heap, unreadable file); pagesOut is then empty.
bool buildCardHtmlPages(GfxRenderer& renderer, const std::string& xhtmlPath, const std::string& cssPath,
                        uint16_t viewportWidth, uint16_t viewportHeight, int fontId, bool furigana,
                        std::vector<std::unique_ptr<Page>>& pagesOut);
