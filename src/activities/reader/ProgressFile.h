#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ProgressFile {

// Two layouts share progress.bin. Stock CrossPoint writes 4, 6 or 10 bytes: spine, page,
// chapter page count, then the visible-text offset at [6-9]. The fork keeps its own bytes
// right after the count -- [6] vertical, [7] furigana, [8] percent -- and the offset at
// [9-12], so its files are 8, 9 or 13 bytes. The size is the only thing that tells them
// apart; a card that saw both firmwares carries both kinds.
inline constexpr bool isStockLayout(const int size) { return size == 4 || size == 6 || size == 10; }

// The book's own vertical-text and furigana bytes ([6] and [7]) from progress.bin, or -1 each
// when there are none (no file, or the stock layout, which has the text offset there).
inline void readBookChoices(const std::string& cachePath, int8_t& vertical, int8_t& furigana) {
  vertical = -1;
  furigana = -1;
  HalFile f;
  if (!Storage.openFileForRead("PRG", cachePath + "/progress.bin", f)) return;
  uint8_t data[13];
  const int size = f.read(data, sizeof(data));
  if (size < 8 || isStockLayout(size)) return;
  vertical = static_cast<int8_t>(data[6]);
  furigana = static_cast<int8_t>(data[7]);
}

// What progress.bin holds, in either layout (see ProgressFile): stock CrossPoint has the text offset at
// [6-9] and no reader choices, the fork has vertical/furigana at [6-7] and the offset at [9-12].
struct SavedSpot {
  bool valid = false;
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t pageCount = 0;  // 0 when not recorded
  bool hasOffset = false;
  uint32_t offset = 0;
  int8_t vertical = -1;  // -1: the book has no choice of its own
  int8_t furigana = -1;
};

inline SavedSpot readSavedSpot(const std::string& cachePath) {
  SavedSpot s;
  HalFile f;
  uint8_t data[13];
  if (!Storage.openFileForRead("ERS", cachePath + "/progress.bin", f)) return s;
  const int size = f.read(data, sizeof(data));
  if (size < 4) return s;
  const auto le32 = [&data](const int at) {
    return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8) |
           (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
  };
  s.valid = true;
  s.spine = static_cast<uint16_t>(data[0] | (data[1] << 8));
  s.page = static_cast<uint16_t>(data[2] | (data[3] << 8));
  if (size >= 6) s.pageCount = static_cast<uint16_t>(data[4] | (data[5] << 8));
  if (ProgressFile::isStockLayout(size)) {
    if (size >= 10) {
      s.hasOffset = true;
      s.offset = le32(6);
    }
  } else {
    if (size >= 8) {
      s.vertical = static_cast<int8_t>(data[6]);
      s.furigana = static_cast<int8_t>(data[7]);
    }
    if (size >= 13) {
      s.hasOffset = true;
      s.offset = le32(9);
    }
  }
  return s;
}

// Writes `len` bytes of reader progress to `<cachePath>/progress.bin` without
// ever leaving the canonical file half-written.
//
// The bytes go to a temporary `progress.bin.tmp` first; only once that is fully
// written and closed is it renamed over progress.bin. An interrupted write
// (power loss or a crash mid-SPI) therefore damages only the throwaway temp file.
// Previously a truncate-in-place write that was cut short left progress.bin with
// a broken FAT cluster chain that the firmware could neither rewrite nor clear,
// stranding the book on an old page (issue #2275).
//
// This is crash-safe, not metadata-atomic: on FAT the replace is remove + rename,
// two separate directory operations, so a crash between them can leave neither
// file -- which simply reads as "no saved progress" on next launch, never a
// corrupt or unclearable file. The point is that progress.bin is never torn.
//
// Note: this prevents corruption on a healthy card going forward. It cannot
// repair an already-corrupted progress.bin -- removing the stale file may itself
// fail at the FAT level, in which case recovery still requires fsck on a host.
//
// Returns true only if the new progress.bin is fully in place.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  const std::string finalPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress.bin.tmp";

  {
    HalFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // canonical file first. The brief window where neither file exists reads as
  // "no saved progress" on next launch -- never a corrupt, unclearable file.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

}  // namespace ProgressFile
