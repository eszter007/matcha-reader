#include "ResidentFiles.h"

#ifdef BOARD_HAS_PSRAM

#include <Arduino.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <string>

namespace {
// Share of the board's PSRAM resident files may hold; the rest stays for fonts, covers, Bluetooth
// and the reader's large buffers.
constexpr size_t BUDGET_PERCENT = 40;
// Four StarDict dictionaries (index + samples) and the Japanese/Chinese tiers of one language.
constexpr int MAX_SLOTS = 12;
// Each read is one storage call; the mutex is free between them.
constexpr size_t READ_CHUNK = 32 * 1024;
// An entry used this recently is never evicted: dictionaries asked in turn for every word would
// otherwise push each other out and reload from the card on each lookup. The newcomer stays on
// the card instead.
constexpr unsigned long PROTECTED_MS = 10 * 1000;

struct Slot {
  std::string key;
  uint32_t version = 0;
  uint32_t generation = 0;
  uint32_t lastUse = 0;
  unsigned long lastUseMs = 0;
  size_t bytes = 0;
  HalMemory::PsramBuffer data;
};

Slot g_slots[MAX_SLOTS];
uint32_t g_clock = 0;

size_t budget() { return HalMemory::getPsramHeap().totalBytes * BUDGET_PERCENT / 100; }

size_t residentBytes() {
  size_t total = 0;
  for (const auto& s : g_slots) total += s.data ? s.bytes : 0;
  return total;
}

void evict(Slot& s) {
  s.data.reset();
  s.key.clear();
  s.bytes = 0;
  s.generation++;  // outstanding Refs to this slot stop resolving
}

bool evictable(const Slot& s, const unsigned long now) { return s.data && now - s.lastUseMs >= PROTECTED_MS; }

// Frees least recently used entries until `bytes` more fit the budget, and returns a free slot to
// put it in. Evicts nothing unless that is enough: a newcomer that cannot fit costs no entry.
int makeRoom(const size_t bytes) {
  const unsigned long now = millis();
  size_t reclaimable = 0;
  bool slotFree = false;
  for (const auto& s : g_slots) {
    if (!s.data) slotFree = true;
    if (evictable(s, now)) reclaimable += s.bytes;
  }
  if (residentBytes() - reclaimable + bytes > budget()) return -1;
  if (!slotFree && reclaimable == 0) return -1;
  while (true) {
    int freeSlot = -1;
    int oldest = -1;
    for (int i = 0; i < MAX_SLOTS; ++i) {
      if (!g_slots[i].data) {
        if (freeSlot < 0) freeSlot = i;
      } else if (evictable(g_slots[i], now) && (oldest < 0 || g_slots[i].lastUse < g_slots[oldest].lastUse)) {
        oldest = i;
      }
    }
    if (freeSlot >= 0 && residentBytes() + bytes <= budget()) return freeSlot;
    if (oldest < 0) return -1;
    evict(g_slots[oldest]);
  }
}
}  // namespace

ResidentFiles::Ref ResidentFiles::acquire(const char* key, const uint32_t version, HalFile& file, const size_t from,
                                          const size_t bytes) {
  Ref ref;
  if (!key || bytes == 0 || bytes > budget()) return ref;
  for (int i = 0; i < MAX_SLOTS; ++i) {
    Slot& s = g_slots[i];
    if (s.data && s.version == version && s.bytes == bytes && s.key == key) {
      s.lastUse = ++g_clock;
      s.lastUseMs = millis();
      return Ref{static_cast<int8_t>(i), s.generation};
    }
  }
  if (!loadsAllowed_) return ref;
  // Not even a full eviction would give one block this large: keep the entries.
  if (HalMemory::getPsramHeap().largestBlockBytes + residentBytes() < bytes) return ref;
  const int i = makeRoom(bytes);
  if (i < 0) return ref;
  auto data = HalMemory::allocatePsram(bytes);
  if (!data) return ref;
  const unsigned long startMs = millis();
  for (size_t done = 0; done < bytes;) {
    const size_t n = std::min(READ_CHUNK, bytes - done);
    if (!file.seekSet(from + done) || file.read(data.get() + done, n) != static_cast<int>(n)) {
      LOG_ERR("RES", "Could not read %s into PSRAM; using the card", key);
      return ref;
    }
    done += n;
  }
  Slot& s = g_slots[i];
  s.key = key;
  s.version = version;
  s.bytes = bytes;
  s.lastUse = ++g_clock;
  s.lastUseMs = millis();
  s.data = std::move(data);
  LOG_INF("RES", "%s resident in PSRAM: %u B in %lu ms", key, static_cast<unsigned>(bytes), millis() - startMs);
  return Ref{static_cast<int8_t>(i), s.generation};
}

const uint8_t* ResidentFiles::data(const Ref ref) {
  if (ref.slot < 0 || ref.slot >= MAX_SLOTS) return nullptr;
  Slot& s = g_slots[ref.slot];
  if (!s.data || s.generation != ref.generation) return nullptr;
  s.lastUse = ++g_clock;
  s.lastUseMs = millis();
  return s.data.get();
}

#else

ResidentFiles::Ref ResidentFiles::acquire(const char*, uint32_t, HalFile&, size_t, size_t) { return Ref{}; }

const uint8_t* ResidentFiles::data(Ref) { return nullptr; }

#endif
