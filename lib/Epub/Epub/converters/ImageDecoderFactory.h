#pragma once
#include <string>

#include "ImageToFramebufferDecoder.h"

class ImageDecoderFactory {
 public:
  // Returns a non-owning pointer to a shared, STATELESS converter instance (or nullptr for an
  // unsupported extension). The instances are file-scope statics constructed during static init,
  // before any task exists -- deliberately NOT lazily allocated: getDecoder is called from the
  // render task, the loop task, and the manga prefetch worker, and an unsynchronized first-use
  // `if (!ptr) ptr.reset(new ...)` between two tasks is a use-after-free waiting to happen
  // (task A's reset can delete the instance task B just obtained). Each converter is
  // vtable-pointer-sized with no data members; the heavy per-decode state (JPEGDEC ~20KB /
  // PNGdec ~44KB) is still heap-allocated on demand inside decodeToFramebuffer, so eager
  // construction costs a few bytes of BSS and removes both the race and the OOM path entirely.
  static ImageToFramebufferDecoder* getDecoder(const std::string& imagePath);
  static bool isFormatSupported(const std::string& imagePath);
};
