#pragma once

#include <cstdint>
#include <memory>

class Atkinson1BitDitherer;
class AtkinsonDitherer;
class Print;

bool calculateBmpOutputSize(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight, bool crop,
                            int& outputWidth, int& outputHeight);

// Output size for a source scaled into a target box, and the part of the source that fills it.
// Fit keeps the whole source. Crop gives the box exactly: the source is scaled to cover it and
// the overflow is dropped, keeping the top-left (manga covers want their top).
struct BmpOutputLayout {
  int width = 0;
  int height = 0;
  int scaledWidth = 0;  // the whole source at the chosen scale: at least width
  int scaledHeight = 0;
  int sourceWidth = 0;  // source columns and rows that reach the output: all of them unless cropped
  int sourceHeight = 0;
};
bool calculateBmpOutputLayout(int sourceWidth, int sourceHeight, int targetWidth, int targetHeight, bool crop,
                              BmpOutputLayout& layout);

class BmpStreamWriter {
 public:
  BmpStreamWriter();
  ~BmpStreamWriter();

  BmpStreamWriter(const BmpStreamWriter&) = delete;
  BmpStreamWriter& operator=(const BmpStreamWriter&) = delete;

  bool begin(Print& output, int width, int height, bool oneBit, bool originalThresholds = false);
  bool writeRow(const uint8_t* grayRow, int y);

  int getWidth() const { return width; }
  int getHeight() const { return height; }
  // Rows written so far: fewer than the height after a decode that stopped early.
  int getRowsWritten() const { return rowsWritten; }
  uint8_t* getScratchRow();

 private:
  Print* output{nullptr};
  int width{0};
  int height{0};
  int bytesPerRow{0};
  bool oneBit{false};
  int rowsWritten{0};
  uint8_t rowsSinceYield{0};
  std::unique_ptr<uint8_t[]> packedRow;
  std::unique_ptr<uint8_t[]> scratchRow;
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;
};

class GrayRowScaler {
 public:
  bool begin(int sourceWidth, int sourceHeight, BmpStreamWriter& output);
  bool writeRow(const uint8_t* sourceRow, int sourceY);

 private:
  int sourceWidth{0};
  int outputWidth{0};
  int outputHeight{0};
  uint32_t scaleX{0};
  uint32_t scaleY{0};
  uint32_t nextOutputBoundary{0};
  int currentOutputY{0};
  int accumulatedSourceRows{0};
  BmpStreamWriter* output{nullptr};
  std::unique_ptr<uint32_t[]> rowSums;
};
