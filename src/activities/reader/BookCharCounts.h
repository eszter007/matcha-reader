#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class Epub;

// Each spine file's readable character count, kept in <book cache>/chars.bin. The count depends
// only on the text, so it survives font, size, margin and mode changes. The reader fills it from
// its own layouts and counts the remaining files while it sits idle. The idle count reads the raw
// XHTML and does not apply CSS: text hidden by display:none is counted until that file is laid out.
// Loaded, changed and saved on the reader's loop task only.
class BookCharCounts {
 public:
  static constexpr uint32_t UNKNOWN = UINT32_MAX;

  // Reads chars.bin, or starts with every file unknown when it is missing or from another book.
  // False only when the table could not be allocated.
  bool load(const Epub& epub);
  bool loaded() const { return !counts.empty(); }
  uint32_t get(int spine) const;
  void set(int spine, uint32_t chars);
  // The first file still unknown at or after `from`, wrapping round; -1 when every one is known.
  int nextUnknown(int from) const;
  // Writes chars.bin when something changed since the last save.
  void save(const Epub& epub);

  // Counts one spine file from the book. False when `cancel` stopped it or the file could not be
  // read: the file then stays unknown, to be counted again later.
  static bool count(const std::shared_ptr<Epub>& epub, int spine, bool (*cancel)(void*), void* ctx, uint32_t& out);

 private:
  std::vector<uint32_t> counts;
  bool dirty = false;
};
