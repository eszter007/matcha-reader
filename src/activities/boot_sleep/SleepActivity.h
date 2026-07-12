#pragma once
#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;
  void loop() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderTransparentSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool overlay = false) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  // Scan dirPath for BMP wallpapers (plus PNGs in overlay mode) and render a random one
  // (excluding recently shown). overlay=true draws it over the retained framebuffer instead
  // of a cleared screen. Returns false when the folder is missing/empty or the image could
  // not be rendered.
  bool renderRandomSleepImage(const char* dirPath, bool overlay) const;
  bool renderPngOverlaySleepImage(const std::string& path) const;

  bool fromTimeout = false;
};
