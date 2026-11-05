#pragma once

#include <cstddef>
#include <cstdint>

class HalFile;

// File contents kept in PSRAM across sessions, for data read again and again (dictionary
// indexes): a lookup then reads memory instead of the SD card. One shared budget, a share of the
// PSRAM the board reports, so fonts and covers keep the rest; the least recently used entry makes
// room, freed before the new one is allocated. Without PSRAM every acquire() fails and callers
// keep their SD path.
// Single task only: the reader and its lookup screens; nothing here is locked.
class ResidentFiles {
 public:
#ifdef BOARD_HAS_PSRAM
  static constexpr bool kAvailable = true;
#else
  // Lets callers skip building keys on boards where acquire() always fails.
  static constexpr bool kAvailable = false;
#endif

  // A handle to one resident entry. data() checks it on every use: an entry evicted since is
  // never read through a stale pointer.
  struct Ref {
    int8_t slot = -1;
    uint32_t generation = 0;
    explicit operator bool() const { return slot >= 0; }
  };

  // The entry for `key` + `version` (the caller's staleness key: sizes, counts), loading
  // `bytes` from `file` at `from` on first use. An invalid Ref without PSRAM, when the entry is
  // over the budget, or when the read fails.
  static Ref acquire(const char* key, uint32_t version, HalFile& file, size_t from, size_t bytes);
  // While one is alive, acquire() serves entries already loaded but loads nothing new: for work
  // done in short slices of the main loop (the Word Lookup prescan), which a whole-file read
  // would stall for up to a second. The next interactive lookup loads instead.
  class NoLoads {
   public:
    NoLoads() : previous_(loadsAllowed_) { loadsAllowed_ = false; }
    ~NoLoads() { loadsAllowed_ = previous_; }
    NoLoads(const NoLoads&) = delete;
    NoLoads& operator=(const NoLoads&) = delete;

   private:
    bool previous_;
  };

  // The entry's bytes, or null when it has been evicted (the caller then re-acquires or reads
  // the card). Marks the entry used.
  static const uint8_t* data(Ref ref);

 private:
  static inline bool loadsAllowed_ = true;
};
