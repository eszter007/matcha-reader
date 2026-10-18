#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <cstdlib>
#include <new>

#include "FootnoteHrefIo.h"
#include "css/CssStyle.h"

namespace {

// GfxRenderer::drawLine's width stacks lines downward, so a vertical edge is drawn as parallel
// lines instead, thickening to the right just as a horizontal one thickens downward.
void drawPatternedLine(GfxRenderer& renderer, const int x1, const int y1, const int x2, const int y2,
                       const uint8_t borderSpec) {
  const uint8_t thickness = CssStyle::lineWidthOf(borderSpec);
  const CssBorderLineStyle style = CssStyle::lineStyleOf(borderSpec);
  const bool horizontal = y1 == y2;
  const int width = style == CssBorderLineStyle::Double ? 1 : thickness;
  const auto segment = [&](const int a, const int b) {
    if (horizontal) {
      renderer.drawLine(x1 + a, y1, x1 + b, y1, width, true);
      return;
    }
    for (int i = 0; i < width; ++i) renderer.drawLine(x1 + i, y1 + a, x1 + i, y1 + b, 1, true);
  };
  const int length = horizontal ? std::abs(x2 - x1) + 1 : std::abs(y2 - y1) + 1;
  if (style == CssBorderLineStyle::Solid || style == CssBorderLineStyle::Double) {
    segment(0, length - 1);
    return;
  }
  const int on = style == CssBorderLineStyle::Dotted ? thickness : thickness * 3;
  const int gap = style == CssBorderLineStyle::Dotted ? thickness : thickness * 2;
  for (int offset = 0; offset < length; offset += on + gap) {
    segment(offset, std::min(length - 1, offset + on - 1));
  }
}

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::unique_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: null TextBlock");
    return nullptr;
  }

  auto line = makeUniqueNoThrow<PageLine>(std::move(tb), xPos, yPos);
  if (!line) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return line;
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    LOG_ERR("PGE", "Deserialization failed: null ImageBlock");
    return nullptr;
  }
  auto image = makeUniqueNoThrow<PageImage>(std::move(ib), xPos, yPos);
  if (!image) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageImage");
    return nullptr;
  }
  return image;
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

PageBox::PageBox(const int16_t width, const int16_t height, const uint8_t borderSpec, const int16_t xPos,
                 const int16_t yPos, const uint8_t fill)
    : PageElement(xPos, yPos), width(width), height(height), sides{}, fill(fill) {
  for (size_t i = 0; i < 4; ++i) {
    const uint8_t bit = CssStyle::BORDER_SIDE_BITS[i];
    if (borderSpec & bit) {
      sides[i] = CssStyle::makeBorderSpec(bit, CssStyle::lineStyleOf(borderSpec), CssStyle::lineWidthOf(borderSpec));
    }
  }
}

void PageBox::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width <= 0 || height <= 0) return;
  const int x = xPos + xOffset;
  const int y = yPos + yOffset;
  // Fill first: any border edges are drawn over it, and the block's text is drawn over both
  // because the page renders its elements in insertion order.
  if (fill == FILL_BLACK) {
    renderer.fillRect(x, y, width, height, true);
  } else if (fill == FILL_LIGHT_GRAY) {
    renderer.fillRectDither(x, y, width, height, Color::LightGray);
  }
  // Each edge in its own style; a double edge adds a second line just inside the first.
  const int right = x + width;
  const int bottom = y + height;
  constexpr int doubleInset = 2;
  for (size_t i = 0; i < 4; ++i) {
    const uint8_t spec = sides[i];
    if (spec == 0) continue;
    const int inset = CssStyle::lineStyleOf(spec) == CssBorderLineStyle::Double ? doubleInset : -1;
    for (int pass = 0; pass < (inset > 0 ? 2 : 1); ++pass) {
      const int d = pass == 0 ? 0 : inset;
      switch (i) {
        case 0:
          drawPatternedLine(renderer, x, y + d, right, y + d, spec);
          break;
        case 1:
          drawPatternedLine(renderer, right - d, y, right - d, bottom, spec);
          break;
        case 2:
          drawPatternedLine(renderer, x, bottom - d, right, bottom - d, spec);
          break;
        default:
          drawPatternedLine(renderer, x + d, y, x + d, bottom, spec);
          break;
      }
    }
  }
}

bool PageBox::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  for (const uint8_t side : sides) serialization::writePod(file, side);
  serialization::writePod(file, fill);
  return true;
}

std::unique_ptr<PageBox> PageBox::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  int16_t width = 0;
  int16_t height = 0;
  uint8_t sides[4] = {};
  uint8_t fill = FILL_NONE;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  serialization::readPod(file, height);
  for (uint8_t& side : sides) serialization::readPod(file, side);
  serialization::readPod(file, fill);
  if (fill > FILL_LIGHT_GRAY) fill = FILL_NONE;
  if (width <= 0 || height <= 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid box metadata (w=%d h=%d)", width, height);
    return nullptr;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!(sides[i] & CssStyle::BORDER_SIDE_BITS[i])) sides[i] = 0;
  }
  auto box = std::unique_ptr<PageBox>(new (std::nothrow) PageBox(width, height, sides, xPos, yPos));
  if (!box) {
    LOG_ERR("PGE", "OOM: PageBox");
    return nullptr;
  }
  box->fill = fill;
  return box;
}

bool PageHorizontalRule::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, thickness);
  return true;
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  serialization::readPod(file, thickness);

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto rule = makeUniqueNoThrow<PageHorizontalRule>(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return rule;
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                  const bool skipRuby) const {
  for (auto& element : elements) {
    if (skipRuby && element->getTag() == TAG_PageLine) {
      auto* line = static_cast<PageLine*>(element.get());
      line->getBlock()->render(renderer, fontId, line->xPos + xOffset, line->yPos + yOffset, true);
    } else {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const int fontId, const int xOffset,
                                       const int yOffset) const {
  for (const auto& element : elements) {
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).renderPlaceholder(renderer, xOffset, yOffset);
    } else {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

bool Page::serialize(HalFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  serialization::writePod(file, fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) || !writeFootnoteHref(file, fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  const uint16_t linkCount = std::min<uint16_t>(links.size(), MAX_LINKS_PER_PAGE);
  serialization::writePod(file, linkCount);
  for (uint16_t i = 0; i < linkCount; i++) {
    const auto& link = links[i];
    if (file.write(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to write link %u", i);
      return false;
    }
    serialization::writePod(file, link.x);
    serialization::writePod(file, link.y);
    serialization::writePod(file, link.width);
    serialization::writePod(file, link.height);
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  auto page = makeUniqueNoThrow<Page>();
  if (!page) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate Page");
    return nullptr;
  }

  uint16_t count;
  serialization::readPod(file, count);

  // Reserve up front so a page load costs one allocation for the element vector
  // instead of a grow-copy-free cycle every doubling. `count` is untrusted (it
  // comes straight off the SD cache), so clamp it: a real page holds a few dozen
  // elements, while a corrupt header could ask for 65535 * sizeof(unique_ptr) and
  // abort() on the failed allocation (vector's operator new is throwing, and this
  // firmware builds with -fno-exceptions). Under-reserving is harmless -- the
  // push_back path below still grows normally.
  static constexpr uint16_t RESERVE_CAP = 256;
  page->elements.reserve(std::min(count, RESERVE_CAP));

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else if (tag == TAG_PageBox) {
      auto box = PageBox::deserialize(file);
      if (!box) {
        return nullptr;
      }
      page->elements.push_back(std::move(box));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  serialization::readPod(file, fnCount);
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) || !readFootnoteHref(file, entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
  }

  uint16_t linkCount;
  // readPod zeroes on a short read, so an unchecked result would turn a file truncated
  // at this field into a valid page carrying no links at all.
  if (!serialization::readPod(file, linkCount)) {
    LOG_ERR("PGE", "Failed to read link count");
    return nullptr;
  }
  if (linkCount > MAX_LINKS_PER_PAGE) {
    LOG_ERR("PGE", "Invalid link count %u", linkCount);
    return nullptr;
  }
  page->links.resize(linkCount);
  for (uint16_t i = 0; i < linkCount; i++) {
    auto& link = page->links[i];
    if (file.read(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to read link %u", i);
      return nullptr;
    }
    link.href[sizeof(link.href) - 1] = '\0';
    serialization::readPod(file, link.x);
    serialization::readPod(file, link.y);
    serialization::readPod(file, link.width);
    serialization::readPod(file, link.height);
    if (link.href[0] == '\0' || link.width <= 0 || link.height <= 0) {
      LOG_ERR("PGE", "Invalid link geometry %u", i);
      return nullptr;
    }
  }

  return page;
}
