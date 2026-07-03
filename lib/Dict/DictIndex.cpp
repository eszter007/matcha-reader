#include "DictIndex.h"

#include <HalStorage.h>
#include <Logging.h>

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
};

DictFileHandles& handlesFor(const char* idxPath) {
  static DictFileHandles jmdict;
  static DictFileHandles grammar;
  static DictFileHandles names;
  if (std::strcmp(idxPath, DictIndex::GRAMMAR_IDX_PATH) == 0) return grammar;
  if (std::strcmp(idxPath, DictIndex::NAMES_IDX_PATH) == 0) return names;
  return jmdict;
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
    idxFile.seek(mid * sizeof(DictIndexRecord));
    if (idxFile.read(reinterpret_cast<uint8_t*>(&rec), sizeof(rec)) != sizeof(rec)) {
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
        idxFile.seek((first - 1) * sizeof(DictIndexRecord));
        DictIndexRecord prevRec;
        if (idxFile.read(reinterpret_cast<uint8_t*>(&prevRec), sizeof(prevRec)) != sizeof(prevRec)) break;
        if (std::memcmp(key, prevRec.headword, DictIndexRecord::HEADWORD_SIZE) != 0) break;
        first--;
      }

      // Collect all entries with this headword, pick highest priority and merge
      struct Entry { std::string def; uint8_t priority; };
      std::vector<Entry> entries;
      constexpr int kMaxEntries = 5;

      for (size_t idx = first; idx < recordCount && static_cast<int>(entries.size()) < kMaxEntries; idx++) {
        idxFile.seek(idx * sizeof(DictIndexRecord));
        DictIndexRecord r;
        if (idxFile.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != sizeof(r)) break;
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
