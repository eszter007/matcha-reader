#include "ReadingPace.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {
constexpr char MAGIC[4] = {'P', 'A', 'C', '1'};
}  // namespace

void ReadingPace::addPage(uint32_t seconds, const int progressDeltaBp) {
  if (pages_ >= MIN_PAGES) seconds = std::min(seconds, MAX_PAGE_FACTOR * seconds_ / pages_);
  pages_++;
  seconds_ += seconds;
  progressBp_ += static_cast<uint32_t>(std::max(0, progressDeltaBp));
  if (pages_ >= DECAY_AT_PAGES) {
    pages_ /= 2;
    seconds_ /= 2;
    progressBp_ /= 2;
  }
}

int ReadingPace::secondsForPages(const int pages) const {
  if (pages_ < MIN_PAGES || pages < 0) return -1;
  return static_cast<int>(static_cast<uint64_t>(seconds_) * static_cast<uint64_t>(pages) / pages_);
}

int ReadingPace::secondsForProgress(const float bp) const {
  if (pages_ < MIN_PAGES || progressBp_ == 0 || bp < 0.0f) return -1;
  const float seconds = static_cast<float>(seconds_) * bp / static_cast<float>(progressBp_);
  return static_cast<int>(std::min(seconds, static_cast<float>(INT32_MAX / 2)) + 0.5f);
}

void ReadingPace::load(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("PACE", path.c_str(), f)) return;
  uint8_t data[16];
  if (f.read(data, sizeof(data)) != static_cast<int>(sizeof(data)) || memcmp(data, MAGIC, sizeof(MAGIC)) != 0) return;
  memcpy(&pages_, data + 4, 4);
  memcpy(&seconds_, data + 8, 4);
  memcpy(&progressBp_, data + 12, 4);
}

void ReadingPace::save(const std::string& path) const {
  if (pages_ == 0) return;
  HalFile f;
  if (!Storage.openFileForWrite("PACE", path.c_str(), f)) {
    LOG_ERR("PACE", "Cannot save reading pace");
    return;
  }
  uint8_t data[16];
  memcpy(data, MAGIC, sizeof(MAGIC));
  memcpy(data + 4, &pages_, 4);
  memcpy(data + 8, &seconds_, 4);
  memcpy(data + 12, &progressBp_, 4);
  f.write(data, sizeof(data));
}
