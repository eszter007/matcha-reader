#include <Epub/Page.h>
#include <Epub/TokenBoundary.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageToFramebufferDecoder.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <GfxRenderer.h>

const char* lookupHtmlEntity(const char*, size_t) { return nullptr; }

#include <BidiUtils.h>

bool isExplicitHyphen(uint32_t) { return false; }
bool isSoftHyphen(uint32_t) { return false; }

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }

namespace BidiUtils {
bool startsWithRtl(const char*, int) { return false; }
bool computeVisualWordOrder(const std::vector<std::string>& words, bool, std::vector<uint16_t>& order) {
  order.resize(words.size());
  for (size_t index = 0; index < words.size(); ++index) order[index] = static_cast<uint16_t>(index);
  return true;
}
}  // namespace BidiUtils

// The real constructor flattens the per-word arrays into an arena; this double keeps them
// where a test can read them instead, since the arena accessors it would need belong to
// TextBlock.cpp (which cannot be linked here -- its render() calls a GfxRenderer far richer
// than the stub). Appended in construction order, which is the order lines are emitted.
std::vector<std::vector<std::string>> stubLineWords;
std::vector<std::vector<int16_t>> stubLineXPos;

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>&, const std::vector<uint8_t>&,
                     const std::vector<uint16_t>&, const BlockStyle& blockStyle, std::vector<std::string> rubyTexts,
                     const std::vector<int32_t>&, std::vector<LinkSpan> linkSpans)
    : blockStyle(blockStyle), rubyTexts(std::move(rubyTexts)), linkSpans(std::move(linkSpans)) {
  stubLineWords.push_back(words);
  stubLineXPos.push_back(wordXpos);
  // Mirror the real constructor's invariant: a block never holds an all-empty rubyTexts, so a
  // ruby-less line reports none. Tests assert on getRubyTexts(), so the double has to agree.
  if (!hasRuby()) this->rubyTexts = std::vector<std::string>{};
}

// Same rule the real one applies: ruby is present only if some entry is non-empty.
bool TextBlock::hasRuby() const {
  for (const auto& rt : rubyTexts) {
    if (!rt.empty()) return true;
  }
  return false;
}

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, int16_t width, int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageToFramebufferDecoder::validateAndStoreDimensions(int64_t, int64_t, ImageDimensions&, const char*) {
  return false;
}

void ImageBlock::render(GfxRenderer&, int, int) {}
void ImageBlock::renderPlaceholder(GfxRenderer&, int, int) const {}
bool ImageBlock::needsDecode() const { return false; }
bool ImageBlock::serialize(HalFile&) { return false; }
std::unique_ptr<ImageBlock> ImageBlock::deserialize(HalFile&) { return nullptr; }

// Page.cpp is linked for real, and its element renderers call into TextBlock. The block itself
// stays a double (see stubLineWords above), so these three round out its link surface; none of
// them is what the parser tests assert on.
void TextBlock::render(const GfxRenderer&, int, int, int, bool) const {}
bool TextBlock::serialize(HalFile&) const { return false; }
std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile&) { return nullptr; }
