#include "ProgressMapper.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ChapterXPathResolver.h"
#include "Epub/Section.h"
#include "Epub/VisibleTextUtils.h"
#include "Epub/htmlEntities.h"
#include "Utf8.h"
#include "VisibleTextStreamer.h"

namespace {
using namespace koreader_sync;

bool streamSpine(const std::shared_ptr<Epub>& epub, int spineIndex, ParagraphStreamer& s) {
  const auto href = epub->getSpineItem(spineIndex).href;
  return !href.empty() && epub->readItemContentsToStream(href, s, 1024);
}
}  // namespace

SavedProgressPosition ProgressMapper::toSavedProgress(const std::shared_ptr<Epub>& epub,
                                                      const CrossPointPosition& pos) {
  SavedProgressPosition result;
  float intra =
      (pos.totalPages > 1) ? static_cast<float>(pos.pageNumber) / static_cast<float>(pos.totalPages - 1) : 0.0f;
  result.percentage = epub->calculateProgress(pos.spineIndex, intra);
  if (pos.hasVisibleTextOffset) {
    // Only when it fits: the full-ancestry anchor is the longest form, and one that the server
    // would drop is worse than the shorter paragraph anchor below, which often still fits.
    auto offsetXPath = ChapterXPathResolver::findXPathForVisibleTextOffset(epub, pos.spineIndex, pos.visibleTextOffset);
    if (offsetXPath.size() <= MAX_SYNC_XPATH_BYTES) {
      result.xpath = std::move(offsetXPath);
    }
  }
  if (result.xpath.empty() && pos.hasParagraphIndex && pos.paragraphIndex > 0) {
    // Same length gate: an over-long paragraph anchor is dropped by the client, and leaving it in
    // result.xpath would stop the shorter fallbacks below ever being tried.
    auto paragraphXPath = ChapterXPathResolver::findXPathForParagraph(epub, pos.spineIndex, pos.paragraphIndex);
    if (paragraphXPath.size() <= MAX_SYNC_XPATH_BYTES) {
      result.xpath = std::move(paragraphXPath);
    }
  }
  // Fall back to progress-based XPath, then synthetic progress mapping.
  if (result.xpath.empty()) {
    result.xpath = ChapterXPathResolver::findXPathForProgress(epub, pos.spineIndex, intra);
  }
  if (result.xpath.empty()) {
    result.xpath = generateXPath(epub, pos.spineIndex, intra);
  }
  LOG_DBG("PM", "-> Progress: spine=%d page=%d/%d %.2f%% %s", pos.spineIndex, pos.pageNumber, pos.totalPages,
          static_cast<double>(result.percentage * 100), result.xpath.c_str());
  return result;
}

std::optional<CrossPointPosition> ProgressMapper::fromRichPosition(const std::shared_ptr<Epub>& epub,
                                                                   const KOReaderRichPosition& rich,
                                                                   GfxRenderer& renderer, bool xpathAlreadyTried) {
  const int spineCount = epub->getSpineItemsCount();
  if (static_cast<int>(rich.spineIndex) >= spineCount) {
    LOG_DBG("PM", "Rich position spine %u out of range (%d spine items)", rich.spineIndex, spineCount);
    return std::nullopt;
  }

  CrossPointPosition result{};
  result.spineIndex = rich.spineIndex;
  result.hasResolvedSpineIndex = true;

  // The existing rich extension carries the same KOReader XPath as the standard
  // progress field. Resolve that content anchor first; remote page counts are
  // layout-dependent hints only. Skip it when the caller already resolved this exact
  // XPath -- re-streaming the same chapter for the same failure is pure waste.
  if (!xpathAlreadyTried && !rich.xpath.empty()) {
    SavedProgressPosition saved{rich.xpath, static_cast<float>(rich.pctQ) / 1000000.0f};
    auto contentMapped = toCrossPoint(epub, saved, renderer);
    if (contentMapped.hasVisibleTextOffset) {
      return contentMapped;
    }
  }

  Section tempSection(epub, result.spineIndex, renderer);
  const auto cachedCount = tempSection.getCachedPageCount();
  if (!cachedCount || *cachedCount <= 0) {
    // No local layout for the target spine yet; the percentage/xpath mapping
    // handles density estimation better than a blind copy of remote pages.
    LOG_DBG("PM", "Rich position spine %u has no cached page count", rich.spineIndex);
    return std::nullopt;
  }
  result.totalPages = *cachedCount;

  const int remotePages = rich.totalPages > 0 ? rich.totalPages : 1;
  if (result.totalPages == remotePages) {
    // Identical layout (same render settings) — the page transfers losslessly.
    result.pageNumber = std::min<int>(rich.pageNumber, result.totalPages - 1);
    result.hasMappedPage = true;
    LOG_DBG("PM", "Rich position exact: spine=%d page=%d/%d", result.spineIndex, result.pageNumber, result.totalPages);
    return result;
  }

  // Layout differs; the paragraph LUT is the most accurate anchor we have.
  if (rich.paragraphIndex.has_value()) {
    const auto lutPage = tempSection.getPageForParagraphIndex(*rich.paragraphIndex);
    if (lutPage.has_value()) {
      result.paragraphIndex = *rich.paragraphIndex;
      result.hasParagraphIndex = true;
      result.pageNumber = std::min<int>(*lutPage, result.totalPages - 1);
      result.hasMappedPage = true;
      LOG_DBG("PM", "Rich position para %u -> spine=%d page=%d/%d", *rich.paragraphIndex, result.spineIndex,
              result.pageNumber, result.totalPages);
      return result;
    }
  }

  // Fall back to the intra-spine page fraction.
  const float intra =
      (remotePages > 1) ? static_cast<float>(rich.pageNumber) / static_cast<float>(remotePages - 1) : 0.0f;
  result.pageNumber = std::max(
      0, std::min(static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));
  LOG_DBG("PM", "Rich position scaled: spine=%d remote %u/%d -> page=%d/%d", result.spineIndex, rich.pageNumber,
          remotePages, result.pageNumber, result.totalPages);
  return result;
}

CrossPointPosition ProgressMapper::toCrossPoint(const std::shared_ptr<Epub>& epub, const SavedProgressPosition& koPos,
                                                GfxRenderer& renderer, int currentSpineIndex,
                                                int totalPagesInCurrentSpine, int fallbackTotalPages) {
  CrossPointPosition result{};
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return result;

  const int spineCount = epub->getSpineItemsCount();
  const float clampedPercentage = std::max(0.0f, std::min(1.0f, koPos.percentage));
  const size_t targetBytes = static_cast<size_t>(static_cast<float>(bookSize) * clampedPercentage);

  const int docFrag = parseIndex(koPos.xpath, "/body/DocFragment[");
  const int xpathP = parseIndex(koPos.xpath, "/p[", true);
  const int xpathChar = parseCharOffset(koPos.xpath);
  const int xpathTextNode = parseTextNodeIndex(koPos.xpath);
  const int xpathSpine = (docFrag >= 1) ? (docFrag - 1) : -1;

  XPathStep xpathSteps[MAX_XPATH_DEPTH];
  const int xpathStepCount = parseXPathSteps(koPos.xpath, xpathSteps);
  // Use ancestry mode whenever the XPath has a structured path (always more accurate than global counting).
  const bool useAncestry = xpathStepCount > 0;
  const bool useBodyText = !useAncestry && isBodyTextXPath(koPos.xpath);

  if (xpathSpine >= 0 && xpathSpine < spineCount) {
    result.spineIndex = xpathSpine;
    result.hasResolvedSpineIndex = true;
  } else {
    for (int i = 0; i < spineCount; i++) {
      if (epub->getCumulativeSpineItemSize(i) >= targetBytes) {
        result.spineIndex = i;
        break;
      }
    }
  }

  const size_t prevCum = (result.spineIndex > 0) ? epub->getCumulativeSpineItemSize(result.spineIndex - 1) : 0;
  const size_t spineSize = epub->getCumulativeSpineItemSize(result.spineIndex) - prevCum;

  if (result.spineIndex == currentSpineIndex && totalPagesInCurrentSpine > 0) {
    result.totalPages = totalPagesInCurrentSpine;
  } else if (currentSpineIndex >= 0 && currentSpineIndex < spineCount && totalPagesInCurrentSpine > 0) {
    const size_t pc = (currentSpineIndex > 0) ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t cs = epub->getCumulativeSpineItemSize(currentSpineIndex) - pc;
    if (cs > 0)
      result.totalPages = std::max(
          1, static_cast<int>(totalPagesInCurrentSpine * static_cast<float>(spineSize) / static_cast<float>(cs)));
  }

  if (result.totalPages <= 0) {
    Section tempSection(epub, result.spineIndex, renderer);
    if (auto cachedCount = tempSection.getCachedPageCount()) {
      result.totalPages = *cachedCount;
    } else if (fallbackTotalPages > 0) {
      result.totalPages = fallbackTotalPages;
    } else {
      result.totalPages = 1;  // Prevent division by zero and give a fallback
    }
  }

  float intra = 0.0f;
  if (useAncestry) {
    const auto applyResolvedXPath = [&](const ParagraphStreamer& s) {
      result.visibleTextOffset =
          static_cast<uint32_t>(std::min<size_t>(s.getTargetVisChars(), static_cast<size_t>(UINT32_MAX)));
      result.hasVisibleTextOffset = true;
      const int pAtMatch = s.getParagraphAtMatch();
      if (pAtMatch > 0) {
        result.paragraphIndex = static_cast<uint16_t>(pAtMatch);
        result.hasParagraphIndex = true;
      }
      if (xpathStepCount > 0 && strcasecmp(xpathSteps[xpathStepCount - 1].tag, "li") == 0) {
        const int liAtMatch = s.getListItemAtMatch();
        if (liAtMatch > 0) {
          result.liIndex = static_cast<uint16_t>(liAtMatch);
          result.hasLiIndex = true;
        }
      }
      const char* anchorId = s.getCapturedAnchorId();
      if (anchorId) {
        strncpy(result.xpathAnchorId, anchorId, sizeof(result.xpathAnchorId) - 1);
      }
      LOG_DBG("PM", "XPath ancestry(%s[%d])/text()[%d]+%d -> %.1f%% (target=%zu total=%zu p~%d li~%d anchor=%s)",
              xpathSteps[xpathStepCount - 1].tag, xpathSteps[xpathStepCount - 1].siblingIndex, xpathTextNode, xpathChar,
              s.progress() * 100, s.getTargetVisChars(), s.getTotalVisChars(), pAtMatch,
              result.hasLiIndex ? static_cast<int>(result.liIndex) : 0, anchorId ? anchorId : "none");
    };

    ParagraphStreamer strict(xpathSteps, xpathStepCount, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, strict) && strict.found()) {
      applyResolvedXPath(strict);
    } else {
      // Some KOReader producers omit an unindexed wrapper from the ancestry
      // (the compatibility case covered by PR #2777). Retry only after the
      // structurally exact path fails, allowing the first step at any body depth.
      ParagraphStreamer relaxed(xpathSteps, xpathStepCount, xpathChar, xpathTextNode, true);
      if (streamSpine(epub, result.spineIndex, relaxed) && relaxed.found()) {
        applyResolvedXPath(relaxed);
      }
    }
  } else if (useBodyText) {
    ParagraphStreamer s(true, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      result.visibleTextOffset =
          static_cast<uint32_t>(std::min<size_t>(s.getTargetVisChars(), static_cast<size_t>(UINT32_MAX)));
      result.hasVisibleTextOffset = true;
      LOG_DBG("PM", "XPath body/text()[%d]+%d -> offset=%u", xpathTextNode, xpathChar, result.visibleTextOffset);
    }
  } else if (xpathP > 0) {
    ParagraphStreamer s(xpathP, xpathChar, xpathTextNode);
    if (streamSpine(epub, result.spineIndex, s) && s.found()) {
      result.visibleTextOffset =
          static_cast<uint32_t>(std::min<size_t>(s.getTargetVisChars(), static_cast<size_t>(UINT32_MAX)));
      result.hasVisibleTextOffset = true;
      LOG_DBG("PM", "XPath p[%d]/text()[%d]+%d -> %.1f%% (target=%zu total=%zu)", xpathP, xpathTextNode, xpathChar,
              s.progress() * 100, s.getTargetVisChars(), s.getTotalVisChars());
    }
  }
  if (!result.hasVisibleTextOffset && xpathSpine >= 0 && xpathSpine < spineCount && isChapterStartXPath(koPos.xpath)) {
    // Only fall back to "chapter start" when no intra-chapter offset was resolved above --
    // otherwise a resolved deep position (e.g. body/div[3]/text().0) would be clobbered to page 0.
    result.visibleTextOffset = 0;
    result.hasVisibleTextOffset = true;
    LOG_DBG("PM", "Chapter-start XPath %s -> spine=%d page start", koPos.xpath.c_str(), result.spineIndex);
  }
  if (result.hasVisibleTextOffset) {
    Section tempSection(epub, result.spineIndex, renderer);
    const bool imageAnchor = useAncestry && (strcasecmp(xpathSteps[xpathStepCount - 1].tag, "img") == 0 ||
                                             strcasecmp(xpathSteps[xpathStepCount - 1].tag, "image") == 0);
    if (const auto offsetPage = tempSection.getPageForVisibleTextOffset(result.visibleTextOffset, imageAnchor)) {
      result.pageNumber = *offsetPage;
      result.totalPages = std::max(result.totalPages, result.pageNumber + 1);
      result.hasMappedPage = true;
      LOG_DBG("PM", "XPath content offset %u -> spine=%d page=%d/%d", result.visibleTextOffset, result.spineIndex,
              result.pageNumber, result.totalPages);
      return result;
    }
    // A valid content anchor without a local pagination LUT cannot yet be turned
    // into a page. Retain it on the result, but use protocol percentage for the
    // immediate page fallback.
    LOG_DBG("PM", "No page-offset LUT for spine=%d offset=%u; using percentage fallback", result.spineIndex,
            result.visibleTextOffset);
  }
  const size_t bytesIn = (targetBytes > prevCum) ? (targetBytes - prevCum) : 0;
  intra = spineSize > 0 ? std::max(0.0f, std::min(1.0f, static_cast<float>(bytesIn) / static_cast<float>(spineSize)))
                        : 0.0f;

  result.pageNumber = std::max(
      0, std::min(static_cast<int>(intra * static_cast<float>(result.totalPages - 1) + 0.5f), result.totalPages - 1));
  LOG_DBG("PM", "<- Progress: %.2f%% %s -> spine=%d page=%d/%d", koPos.percentage * 100, koPos.xpath.c_str(),
          result.spineIndex, result.pageNumber, result.totalPages);

  // Refine page using section cache LUTs: li index, anchor, or paragraph index.
  if (result.hasLiIndex || result.xpathAnchorId[0] != '\0' || result.hasParagraphIndex) {
    Section tempSection(epub, result.spineIndex, renderer);
    bool refined = false;
    if (result.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(result.liIndex);
      if (liPage.has_value()) {
        LOG_DBG("PM", "Li index %u -> page %d (was %d)", result.liIndex, *liPage, result.pageNumber);
        result.pageNumber = *liPage;
        result.hasMappedPage = true;
        refined = true;
      } else {
        LOG_DBG("PM", "Li index %u not found in section LUT", result.liIndex);
      }
    }
    if (!refined && result.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(result.xpathAnchorId));
      if (anchorPage.has_value()) {
        LOG_DBG("PM", "Anchor '%s' -> page %d (was %d)", result.xpathAnchorId, *anchorPage, result.pageNumber);
        result.pageNumber = *anchorPage;
        result.hasMappedPage = true;
        refined = true;
      } else {
        LOG_DBG("PM", "Anchor '%s' not found in section cache", result.xpathAnchorId);
      }
    }
    if (!refined && result.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(result.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(result.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(result.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          // Only cap when the LUT span is >1. A span of 1 means the LUT granularity is too
          // coarse to trust over the intra-spine position (e.g. a stale cache where the paragraph
          // occupies different pages than at build time).
          if (lutSpan > 1 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        char nextParaBuf[8];
        if (nextParagraphPage.has_value())
          snprintf(nextParaBuf, sizeof(nextParaBuf), "%d", *nextParagraphPage);
        else
          snprintf(nextParaBuf, sizeof(nextParaBuf), "none");
        LOG_DBG("PM", "Paragraph %u -> LUT page %d, nextPara page %s, intra page %d, using %d", result.paragraphIndex,
                *paragraphPage, nextParaBuf, result.pageNumber, refinedPage);
        result.pageNumber = refinedPage;
        result.hasMappedPage = true;
      } else {
        LOG_DBG("PM", "Paragraph %u not found in section LUT", result.paragraphIndex);
      }
    }
  }
  return result;
}

std::string ProgressMapper::generateXPath(const std::shared_ptr<Epub>& epub, int spineIndex, float intra) {
  const std::string base = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
  if (intra <= 0.0f) return base;

  size_t spineSize = 0;
  const auto href = epub->getSpineItem(spineIndex).href;
  if (href.empty() || !epub->getItemSize(href, &spineSize) || spineSize == 0) return base;

  ParagraphStreamer s(static_cast<size_t>(spineSize * std::min(intra, 1.0f)));
  if (!streamSpine(epub, spineIndex, s)) return base;

  const int p = s.paragraphCount();
  return (p > 0) ? base + "/p[" + std::to_string(p) + "]" : base;
}
