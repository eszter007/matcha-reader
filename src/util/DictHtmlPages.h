#pragma once

#include <Epub/Page.h>

#include <memory>
#include <string>
#include <vector>

class GfxRenderer;

// Lays out a StarDict HTML definition through the EPUB chapter parser,
// yielding the same styled, paginated Pages the reader renders. Returns false
// when the fragment cannot be normalized into parseable XHTML (or on OOM) —
// the caller falls back to the plain-text viewer path.
// `fontId` is the font the caller will DRAW the pages with; layout must use the same one or the
// measured lines disagree with the pixels (see the reader's effectiveReaderFontId).
bool buildDictionaryHtmlPages(GfxRenderer& renderer, const std::string& definition, uint16_t viewportWidth,
                              uint16_t viewportHeight, int fontId, std::vector<std::unique_ptr<Page>>& pagesOut);
