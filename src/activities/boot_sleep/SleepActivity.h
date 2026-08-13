#pragma once
#include "activities/Activity.h"

class Bitmap;
class HalFile;

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
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool preserveBackground = false) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen() const;
  // Legacy fork overlay locations (/.sleep/transparent, /sleep/transparent), tried after the
  // upstream ones so cards set up before this merge keep working. PNG overlays only exist here:
  // upstream's BGRA path is BMP-only, and a PNG carries its alpha without a hand-built BMP.
  bool renderLegacyTransparentOverlay() const;
  bool renderPngOverlaySleepImage(const std::string& path) const;
  void renderBlankSleepScreen() const;

  bool fromTimeout = false;
};
