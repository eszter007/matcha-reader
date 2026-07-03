#include "DictIndex.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {
// Re-opening the idx/dat files on every lookup was the dominant cost of building the Word Lookup
// screen: each SD file open has real filesystem overhead (path resolution, directory traversal),
// and WordLookup::lookup() calls DictIndex::lookupExact() up to 8 window lengths x several
// deinflection candidates x up to 3 dictionaries -- per character position on the page. For a
// full page that's thousands of opens where a handful would do. There are only ever 3 fixed
// (idx, dat) path pairs (jmdict/grammar/jmnedict) for the process's whole lifetime, so it's safe
// to open each pair once (lazily, on first use) and keep it open rather than reopening per call.
struct DictFileHandles {
  HalFile idxFile;
  HalFile datFile;
  bool opened = false;
  bool triedOpen = false;  // true once an open has been attempted, success or failure

  // Small read-ahead cache for the index-record accesses in lookupInFile(). Most of the latency
  // in a single SD read is per-transaction protocol overhead, not the 40-byte payload -- and two
  // of the three access patterns there are sequential (the "scan backward to find the first
  // record with this headword" walk, and the "collect up to 5 entries" walk), not the coarse
  // binary search itself. WordLookup::lookup() calls into here up to 8 window lengths x several
  // deinflection candidates x up to 3 dictionaries PER CHARACTER on the page, so cutting even the
  // sequential-phase read count matters a lot in aggregate. Centering the block on whatever index
  // triggered the miss covers both the backward walk and the forward collect from one fetch in
  // the common case (same-headword runs are bounded to 5 by kMaxEntries in lookupInFile).
  static constexpr size_t BLOCK_RECORDS = 32;
  DictIndexRecord blockCache[BLOCK_RECORDS];
  size_t blockStart = SIZE_MAX;  // record index blockCache[0] corresponds to; SIZE_MAX = empty
  size_t blockCount = 0;         // valid records currently in blockCache
};

DictFileHandles& handlesFor(const char* idxPath) {
  static DictFileHandles jmdict;
  static DictFileHandles grammar;
  static DictFileHandles names;
  if (std::strcmp(idxPath, DictIndex::GRAMMAR_IDX_PATH) == 0) return grammar;
  if (std::strcmp(idxPath, DictIndex::NAMES_IDX_PATH) == 0) return names;
  return jmdict;
}

// Reads index record `idx` (of `recordCount` total), transparently using/filling h's block cache.
// Semantically identical to a direct seek+read of that one record -- callers don't need to know
// whether this was served from cache or SD.
bool readIndexRecord(DictFileHandles& h, size_t idx, size_t recordCount, DictIndexRecord& out) {
  if (h.blockStart != SIZE_MAX && idx >= h.blockStart && idx < h.blockStart + h.blockCount) {
    out = h.blockCache[idx - h.blockStart];
    return true;
  }

  const size_t half = DictFileHandles::BLOCK_RECORDS / 2;
  const size_t start = (idx > half) ? (idx - half) : 0;
  const size_t count = std::min(DictFileHandles::BLOCK_RECORDS, recordCount - start);

  h.idxFile.seek(start * sizeof(DictIndexRecord));
  const size_t bytesToRead = count * sizeof(DictIndexRecord);
  if (h.idxFile.read(reinterpret_cast<uint8_t*>(h.blockCache), bytesToRead) != static_cast<int>(bytesToRead)) {
    h.blockStart = SIZE_MAX;  // invalidate on read failure -- don't serve a partial block later
    return false;
  }

  h.blockStart = start;
  h.blockCount = count;
  out = h.blockCache[idx - start];
  return true;
}
}  // namespace

bool DictIndex::isAvailable() {
  return Storage.exists(IDX_PATH) && Storage.exists(DAT_PATH);
}

bool DictIndex::lookupInFile(const char* headword, const char* idxPath, const char* datPath, DictEntry& out) {
  DictFileHandles& h = handlesFor(idxPath);
  if (!h.opened) {
    // Optional dictionaries (grammar, jmnedict) may genuinely not be on the SD card -- without
    // this latch, every lookup attempt would retry the open (and its filesystem overhead) only to
    // fail again, once per candidate/window/character for the whole page.
    if (h.triedOpen) return false;
    h.triedOpen = true;
    if (!Storage.openFileForRead("DICT", idxPath, h.idxFile) ||
        !Storage.openFileForRead("DICT", datPath, h.datFile)) {
      return false;
    }
    h.opened = true;
  }
  HalFile& idxFile = h.idxFile;
  HalFile& datFile = h.datFile;

  const size_t fileSize = idxFile.size();
  if (fileSize < sizeof(DictIndexRecord)) {
    return false;
  }

  const size_t recordCount = fileSize / sizeof(DictIndexRecord);

  char key[DictIndexRecord::HEADWORD_SIZE];
  std::memset(key, 0, sizeof(key));
  const size_t len = std::strlen(headword);
  if (len >= sizeof(key)) {
    return false;
  }
  std::memcpy(key, headword, len);

  size_t lo = 0;
  size_t hi = recordCount;
  DictIndexRecord rec;

  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (!readIndexRecord(h, mid, recordCount, rec)) {
      break;
    }

    const int cmp = std::memcmp(key, rec.headword, DictIndexRecord::HEADWORD_SIZE);
    if (cmp < 0) {
      hi = mid;
    } else if (cmp > 0) {
      lo = mid + 1;
    } else {
      // Found a match — scan backwards to find the first record with this headword
      size_t first = mid;
      while (first > 0) {
        DictIndexRecord prevRec;
        if (!readIndexRecord(h, first - 1, recordCount, prevRec)) break;
        if (std::memcmp(key, prevRec.headword, DictIndexRecord::HEADWORD_SIZE) != 0) break;
        first--;
      }

      // Collect all entries with this headword, pick highest priority and merge
      struct Entry { std::string def; uint8_t priority; };
      std::vector<Entry> entries;
      constexpr int kMaxEntries = 5;

      for (size_t idx = first; idx < recordCount && static_cast<int>(entries.size()) < kMaxEntries; idx++) {
        DictIndexRecord r;
        if (!readIndexRecord(h, idx, recordCount, r)) break;
        if (std::memcmp(key, r.headword, DictIndexRecord::HEADWORD_SIZE) != 0) break;

        datFile.seek(r.offset);
        std::string def;
        def.resize(r.length);
        if (datFile.read(reinterpret_cast<uint8_t*>(def.data()), r.length) != static_cast<int>(r.length)) continue;

        entries.push_back({std::move(def), r.priority});
      }

      if (entries.empty()) return false;

      // Sort by priority (highest first)
      for (size_t a = 0; a < entries.size(); a++) {
        for (size_t b = a + 1; b < entries.size(); b++) {
          if (entries[b].priority > entries[a].priority) {
            std::swap(entries[a], entries[b]);
          }
        }
      }

      out.headword = headword;
      out.priority = entries[0].priority;
      if (entries.size() == 1) {
        out.definition = std::move(entries[0].def);
      } else {
        // Merge: show best entry, then separator + other entries
        out.definition = entries[0].def;
        for (size_t e = 1; e < entries.size(); e++) {
          out.definition += "\n\n---\n" + entries[e].def;
        }
      }
      return true;
    }
  }

  return false;
}

bool DictIndex::lookupExact(const char* headword, DictEntry& out) {
  // No Storage.exists() pre-checks needed here -- lookupInFile()'s own open-once cache already
  // makes a missing optional dictionary (grammar, jmnedict) a cheap no-op after the first attempt,
  // and an existence check would itself be a filesystem call repeated on every lookup otherwise.
  if (lookupInFile(headword, IDX_PATH, DAT_PATH, out)) return true;
  if (lookupInFile(headword, GRAMMAR_IDX_PATH, GRAMMAR_DAT_PATH, out)) return true;
  return lookupInFile(headword, NAMES_IDX_PATH, NAMES_DAT_PATH, out);
}
