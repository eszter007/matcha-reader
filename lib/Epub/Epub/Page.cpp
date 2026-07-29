#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <cstdlib>
#include <new>

#include "css/CssStyle.h"

namespace {

void drawPatternedLine(GfxRenderer& renderer, const int x1, const int y1, const int x2, const int y2,
                       const uint8_t borderSpec) {
  const uint8_t thickness = CssStyle::lineWidthOf(borderSpec);
  const CssBorderLineStyle style = CssStyle::lineStyleOf(borderSpec);
  if (style == CssBorderLineStyle::Solid || style == CssBorderLineStyle::Double) {
    renderer.drawLine(x1, y1, x2, y2, style == CssBorderLineStyle::Double ? 1 : thickness, true);
    return;
  }

  const bool horizontal = y1 == y2;
  const int length = horizontal ? std::abs(x2 - x1) + 1 : std::abs(y2 - y1) + 1;
  const int on = style == CssBorderLineStyle::Dotted ? thickness : thickness * 3;
  const int gap = style == CssBorderLineStyle::Dotted ? thickness : thickness * 2;
  for (int offset = 0; offset < length; offset += on + gap) {
    const int end = std::min(length - 1, offset + on - 1);
    if (horizontal) {
      renderer.drawLine(x1 + offset, y1, x1 + end, y1, thickness, true);
    } else {
      renderer.drawLine(x1, y1 + offset, x1, y1 + end, thickness, true);
    }
  }
}

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::shared_ptr<PageElement>>& elements, GfxRenderer& renderer,
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

  auto* line = new (std::nothrow) PageLine(std::move(tb), xPos, yPos);
  if (!line) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return std::unique_ptr<PageLine>(line);
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
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

void PageBox::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width <= 0 || height <= 0) return;
  const int x = xPos + xOffset;
  const int y = yPos + yOffset;
  // Fill first: any border edges are drawn over it, and the block's text is drawn over both
  // because the page renders its elements in insertion order.
  if (filled) renderer.fillRect(x, y, width, height, true);
  const uint8_t edges = CssStyle::edgeMaskOf(borderSpec);
  if (edges & CssStyle::BORDER_TOP) drawPatternedLine(renderer, x, y, x + width, y, borderSpec);
  if (edges & CssStyle::BORDER_BOTTOM) {
    drawPatternedLine(renderer, x, y + height, x + width, y + height, borderSpec);
  }
  if (edges & CssStyle::BORDER_LEFT) drawPatternedLine(renderer, x, y, x, y + height, borderSpec);
  if (edges & CssStyle::BORDER_RIGHT) {
    drawPatternedLine(renderer, x + width, y, x + width, y + height, borderSpec);
  }
  if (CssStyle::lineStyleOf(borderSpec) == CssBorderLineStyle::Double) {
    constexpr int inset = 2;
    if (edges & CssStyle::BORDER_TOP) drawPatternedLine(renderer, x, y + inset, x + width, y + inset, borderSpec);
    if (edges & CssStyle::BORDER_BOTTOM) {
      drawPatternedLine(renderer, x, y + height - inset, x + width, y + height - inset, borderSpec);
    }
    if (edges & CssStyle::BORDER_LEFT) drawPatternedLine(renderer, x + inset, y, x + inset, y + height, borderSpec);
    if (edges & CssStyle::BORDER_RIGHT) {
      drawPatternedLine(renderer, x + width - inset, y, x + width - inset, y + height, borderSpec);
    }
  }
}

bool PageBox::serialize(HalFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, width);
  serialization::writePod(file, height);
  serialization::writePod(file, borderSpec);
  serialization::writePod(file, filled);
  return true;
}

std::unique_ptr<PageBox> PageBox::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  int16_t width = 0;
  int16_t height = 0;
  uint8_t borderSpec = 0;
  bool filled = false;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, width);
  serialization::readPod(file, height);
  serialization::readPod(file, borderSpec);
  serialization::readPod(file, filled);
  if (width <= 0 || height <= 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid box metadata (w=%d h=%d)", width, height);
    return nullptr;
  }
  return std::unique_ptr<PageBox>(new (std::nothrow) PageBox(width, height, borderSpec, xPos, yPos, filled));
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

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
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
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

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
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  return page;
}
