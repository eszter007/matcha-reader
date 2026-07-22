#pragma once
#include <HalStorage.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(const std::string& imagePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  // When set, the image is rendered rotated 90° (for aspect-mismatched images
  // on a dedicated page). reserveMargin is inset on all edges so it never
  // overlaps the status bar regardless of which edge it maps to after rotation.
  void setRotated(bool r, int16_t reserveMargin) {
    rotated = r;
    reserveMargin_ = reserveMargin;
  }
  bool isRotated() const { return rotated; }

  bool imageExists() const;

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  // Scale w x h down (never up) to fit inside availW x availH, preserving aspect ratio and
  // clamping to >= 1. Shared by the rotated render path, the vertical reader's image-page fit,
  // and warmCache() -- one implementation so a background warm computes EXACTLY the dimensions
  // the later render will expect from the pixel cache.
  static void fitWithin(int availW, int availH, int& w, int& h);

  enum class WarmResult : uint8_t {
    Warmed,         // cache written
    AlreadyWarm,    // cache exists with matching dimensions
    NotApplicable,  // format has no pixel cache to warm (BMP); nothing to do, not an error
    Cancelled,      // shouldCancel fired mid-decode; partial cache dropped, safe to retry later
    Failed          // decode/setup failure; caller should not retry this image
  };

  // Background-warm this image's .pxc pixel cache (cacheOnly decode -- no framebuffer access)
  // so a later render() is a pure cache read instead of a multi-second JPEG/PNG decode.
  // MUST run on the render task: it writes the same cache path render() reads/writes, and
  // single-task use is what makes the direct write (no tmp+rename) safe. shouldCancel is polled
  // per decode block; on cancellation the converter drops the partial cache file itself.
  WarmResult warmCache(GfxRenderer& renderer, bool (*shouldCancel)(const void*), const void* cancelCtx) const;

  void render(GfxRenderer& renderer, const int x, const int y);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  int16_t width;
  int16_t height;
  bool rotated = false;
  int16_t reserveMargin_ = 0;
};
