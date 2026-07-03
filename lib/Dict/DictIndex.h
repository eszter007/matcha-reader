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
  static constexpr const char* NAMES_IDX_PATH = "/dict/jmnedict.idx";
  static constexpr const char* NAMES_DAT_PATH = "/dict/jmnedict.dat";
  static constexpr const char* GRAMMAR_IDX_PATH = "/dict/grammar.idx";
  static constexpr const char* GRAMMAR_DAT_PATH = "/dict/grammar.dat";

  // Which dictionaries lookupExact() should consult. Each dict search is ~2 SD reads, so scoping
  // out dictionaries a candidate can't possibly be in is the main lever for Word Lookup speed:
  //  - deinflected (conjugated) candidates only live in jmdict (names/grammar aren't inflected);
  //  - jmnedict holds proper names, which are kanji/katakana -- never pure hiragana;
  //  - grammar holds multi-char patterns -- never a single character.
  static constexpr uint8_t DICT_JMDICT = 1;
  static constexpr uint8_t DICT_GRAMMAR = 2;
  static constexpr uint8_t DICT_NAMES = 4;
  static constexpr uint8_t DICT_ALL = DICT_JMDICT | DICT_GRAMMAR | DICT_NAMES;

  // Look up a headword in the index.  Returns true and fills `out` on hit.
  // Collects all readings from the requested dictionaries into a single definition.
  // needDefinition=false skips reading the definition payloads from the .dat file (out.definition
  // is left empty) and the adjacent-record collect walk -- existence, headword and matchLength are
  // still exact. The Word Lookup page scan uses this: it discards definitions for most positions,
  // and each avoided definition is 1-5 SD reads.
  static bool lookupExact(const char* headword, DictEntry& out, uint8_t dictMask = DICT_ALL,
                          bool needDefinition = true);

  // Look up in a specific index/dat file pair. Collects ALL entries with
  // the same headword (scanning adjacent records) and merges definitions
  // (unless needDefinition=false; see lookupExact).
  static bool lookupInFile(const char* headword, const char* idxPath, const char* datPath, DictEntry& out,
                           bool needDefinition = true);

  // Free all lookup caches (sparse-index tiers, record block caches) and close the dictionary
  // files, returning ~30KB to the heap pool. MUST be called when a Word Lookup activity exits:
  // heavy operations right after a lookup session (e.g. horizontal re-pagination needs one
  // contiguous 32KB block for the zip inflate dictionary) fail if this memory lingers. The next
  // lookup transparently rebuilds everything (~0.5s).
  static void releaseCaches();

  // Diagnostic: logs and resets the running lookupExact()/record-cache counters. Call with a
  // label (e.g. "buildSelectableGlyphs") right after a scan to see the real call volume instead
  // of guessing. Temporary instrumentation for the Word Lookup slowness investigation.
  static void logAndResetStats(const char* label);
};
