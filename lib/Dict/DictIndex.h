#pragma once

#include <cstdint>
#include <string>

// On-disk format for dictionary index records.
// Index file is a sorted array of these; binary search by headword.
// Dat file contains the variable-length definition text that offset/length point into.
struct DictIndexRecord {
  static constexpr size_t HEADWORD_SIZE = 32;
  char headword[HEADWORD_SIZE];
  uint32_t offset;
  uint16_t length;
  uint8_t priority;
  uint8_t pad;
} __attribute__((packed));

static_assert(sizeof(DictIndexRecord) == 40, "DictIndexRecord must be 40 bytes");

struct DictEntry {
  std::string headword;
  std::string definition;
  uint8_t priority;
};

// Opens jmdict.idx / jmdict.dat from the SD card and provides O(log n)
// lookup by headword via binary search over the sorted index file.
// No full-file load — each search step reads one 40-byte record.
class DictIndex {
 public:
  // Check whether the dictionary files exist on the SD card.
  static bool isAvailable();

  static constexpr const char* IDX_PATH = "/dict/jmdict.idx";
  static constexpr const char* DAT_PATH = "/dict/jmdict.dat";

  // Look up a headword in the index.  Returns true and fills `out` on hit.
  // On device, reads from SD via HalStorage.  In host tests, reads from
  // local files via stdio.
  static bool lookupExact(const char* headword, DictEntry& out);
};
