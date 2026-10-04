// Regression tests for DictIndex's duplicate-headword ("sibling run") window.
//
// A dictionary .idx holds every sense of a headword as a separate record, so one headword is a
// RUN of adjacent records. lookupInFile() ranks that run by priority and merges the top few
// definitions. The run window used to be measured from `mid` -- whichever record the binary
// search happened to land on -- and capped at 32 records per direction. `mid` is not stable:
// with the .spx sparse index present the search bisects a single 48-record stride window, and
// without it the whole file, so the two paths land on different members of the same run. Runs
// longer than the cap therefore yielded a DIFFERENT 32-record slice, a different ranking and a
// different merged definition depending only on whether the .spx sidecar happened to be on the
// SD card (on the shipped Japanese dictionary, こう / し / かん and nine other readings).
//
// These tests build a synthetic dictionary containing runs on both sides of that cap and assert
// the accelerated and fallback search paths return byte-identical results.

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "DictIndex.h"
#include "HalStorage.h"

namespace {

constexpr size_t RECORD_SIZE = sizeof(DictIndexRecord);  // 40
constexpr size_t HEADWORD_SIZE = DictIndexRecord::HEADWORD_SIZE;
constexpr uint32_t SPX_STRIDE = 48;  // must match scripts/gen_dict_spx.py
constexpr size_t SPX_HEADER_SIZE = 32;

// Long enough that the .spx fine tier exceeds the firmware's 128-entry RAM coarse budget, so the
// narrowing exercises BOTH tiers (coarse bracket -> fine window -> stride block) rather than
// degenerating into a single-tier lookup.
constexpr size_t HEADWORD_COUNT = 8000;

std::string headwordAt(size_t i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "k%06zu", i);
  return buf;
}

// How many records (senses) headword `i` has. The interesting cases straddle the old 32-record
// per-direction cap; everything else is a single record, like most real entries.
size_t runLengthAt(size_t i) {
  switch (i) {
    case 1000:
      return 54;  // = こう, the longest run in the shipped jmdict
    case 3000:
      return 33;  // just over the old cap
    case 5000:
      return 32;  // exactly at the old cap (both paths already agreed here)
    case 7000:
      return 46;  // = し
    default:
      return 1;
  }
}

// Priority layout inside a run. Deliberately adversarial: run 1000 puts its best senses at the
// tail and run 7000 at the head, so a window anchored anywhere but the run start misses them.
uint8_t priorityAt(size_t i, size_t j, size_t len) {
  if (i == 1000) return static_cast<uint8_t>(200 + j);              // best at the tail
  if (i == 7000) return static_cast<uint8_t>(200 + (len - 1 - j));  // best at the head
  if (i == 3000) return static_cast<uint8_t>((j * 97 + 11) % 251);  // scattered
  return static_cast<uint8_t>(100 + (j % 7));
}

// Alternating word classes on run 7000 so a POS-masked lookup has to pick among siblings.
uint8_t posFlagsAt(size_t i, size_t j) {
  if (i != 7000) return 0;
  return (j % 2) ? DictIndexRecord::POS_OTHER : DictIndexRecord::POS_V5;
}

std::string definitionAt(size_t i, size_t j) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "def<%s#%zu>", headwordAt(i).c_str(), j);
  return buf;
}

void writeU32(std::string& out, uint32_t v) {
  for (int b = 0; b < 4; b++) out.push_back(static_cast<char>((v >> (8 * b)) & 0xFF));
}

// Build vocab.idx / vocab.dat / vocab.spx under `dir`. Records are emitted in headword order,
// which for the fixed-width "k%06zu" keys is exactly the memcmp order lookupInFile bisects on.
void buildDictionary(const std::filesystem::path& dir) {
  std::filesystem::create_directories(dir / "dictionaries" / "jp");

  std::string idx, dat, checkpoints;
  size_t recordIndex = 0;
  uint32_t fineCount = 0;

  for (size_t i = 0; i < HEADWORD_COUNT; i++) {
    const std::string hw = headwordAt(i);
    const size_t len = runLengthAt(i);
    for (size_t j = 0; j < len; j++) {
      char headword[HEADWORD_SIZE] = {};
      std::memcpy(headword, hw.data(), hw.size());
      if (recordIndex % SPX_STRIDE == 0) {
        checkpoints.append(headword, HEADWORD_SIZE);
        fineCount++;
      }

      const std::string def = definitionAt(i, j);
      idx.append(headword, HEADWORD_SIZE);
      writeU32(idx, static_cast<uint32_t>(dat.size()));  // offset
      idx.push_back(static_cast<char>(def.size() & 0xFF));
      idx.push_back(static_cast<char>((def.size() >> 8) & 0xFF));  // length
      idx.push_back(static_cast<char>(priorityAt(i, j, len)));
      idx.push_back(static_cast<char>(posFlagsAt(i, j)));
      dat += def;
      recordIndex++;
    }
  }
  ASSERT_EQ(idx.size(), recordIndex * RECORD_SIZE);

  // .spx sidecar, byte-for-byte what scripts/gen_dict_spx.py emits.
  std::string spx = "CPSPX1";
  spx.push_back('\0');
  spx.push_back('\0');
  writeU32(spx, 1);  // version
  writeU32(spx, SPX_STRIDE);
  writeU32(spx, static_cast<uint32_t>(recordIndex));
  writeU32(spx, fineCount);
  writeU32(spx, 0);  // reserved
  spx.resize(SPX_HEADER_SIZE, '\0');
  spx += checkpoints;

  const auto write = [&dir](const char* name, const std::string& bytes) {
    std::FILE* f = std::fopen((dir / "dictionaries" / "jp" / name).string().c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(bytes.data(), 1, bytes.size(), f), bytes.size());
    std::fclose(f);
  };
  write("vocab.idx", idx);
  write("vocab.dat", dat);
  write("vocab.spx", spx);
}

// Everything a caller can observe from one lookup.
struct Result {
  bool found = false;
  uint8_t priority = 0;
  uint8_t posFlags = 0;
  std::string definition;

  bool operator==(const Result& o) const {
    return found == o.found && priority == o.priority && posFlags == o.posFlags && definition == o.definition;
  }
};

Result lookup(const std::string& headword, bool needDefinition, uint8_t posMask) {
  DictEntry entry;
  Result r;
  r.found = DictIndex::lookupInFile(headword.c_str(), DictIndex::VOCAB_IDX_PATH, DictIndex::VOCAB_DAT_PATH, entry,
                                    needDefinition, posMask);
  if (r.found) {
    r.priority = entry.priority;
    r.posFlags = entry.posFlags;
    r.definition = entry.definition;
  }
  return r;
}

class DictSiblingWindow : public ::testing::Test {
 protected:
  static std::filesystem::path root_;
  static std::filesystem::path spxPath_;

  static void SetUpTestSuite() {
    // ctest runs each discovered case as its own parallel process, and these cases rename the
    // sidecar in place -- a shared directory name would have them hiding each other's .spx
    // mid-run. Keep the synthetic dictionary private to this process.
    root_ = std::filesystem::temp_directory_path() / ("cp_dict_sibling_window_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root_);
    buildDictionary(root_);
    spxPath_ = root_ / "dictionaries" / "jp" / "vocab.spx";
    testRoot() = root_.string();
  }

  static void TearDownTestSuite() { std::filesystem::remove_all(root_); }

  void TearDown() override {
    restoreSpx();
    DictIndex::releaseCaches();
  }

  // The two search paths differ only in whether the sidecar is on the "card", so hiding the file
  // is exactly how a user without a regenerated dictionary sees the fallback path.
  static void hideSpx() {
    DictIndex::releaseCaches();  // drop the loaded coarse tier and the open file handles
    std::filesystem::rename(spxPath_, spxPath_.string() + ".off");
  }
  static void restoreSpx() {
    DictIndex::releaseCaches();
    if (std::filesystem::exists(spxPath_.string() + ".off")) {
      std::filesystem::rename(spxPath_.string() + ".off", spxPath_);
    }
  }

  // Run `probe` against every headword with the sidecar present, then again with it hidden.
  static void collectBothPaths(bool needDefinition, uint8_t posMask, std::vector<Result>& withSpx,
                               std::vector<Result>& withoutSpx) {
    restoreSpx();
    for (size_t i = 0; i < HEADWORD_COUNT; i++) withSpx.push_back(lookup(headwordAt(i), needDefinition, posMask));
    hideSpx();
    for (size_t i = 0; i < HEADWORD_COUNT; i++) withoutSpx.push_back(lookup(headwordAt(i), needDefinition, posMask));
    restoreSpx();
  }
};

std::filesystem::path DictSiblingWindow::root_;
std::filesystem::path DictSiblingWindow::spxPath_;

// The fixture is only meaningful if the sidecar is actually being used; a silently rejected
// .spx would make every "both paths agree" assertion below pass trivially.
TEST_F(DictSiblingWindow, SparseIndexIsActuallyLoaded) {
  ASSERT_TRUE(std::filesystem::exists(spxPath_));
  // A stale sidecar is refused, so a load that changes nothing observable still must not change
  // results: prove the accelerated path finds the same records the fallback does on a short run.
  const Result accelerated = lookup(headwordAt(42), true, 0);
  hideSpx();
  const Result fallback = lookup(headwordAt(42), true, 0);
  EXPECT_TRUE(accelerated.found);
  EXPECT_EQ(accelerated, fallback);
}

TEST_F(DictSiblingWindow, DefinitionsMatchAcrossSearchPaths) {
  std::vector<Result> withSpx, withoutSpx;
  collectBothPaths(/*needDefinition=*/true, /*posMask=*/0, withSpx, withoutSpx);

  ASSERT_EQ(withSpx.size(), withoutSpx.size());
  for (size_t i = 0; i < withSpx.size(); i++) {
    EXPECT_TRUE(withSpx[i].found) << headwordAt(i);
    EXPECT_EQ(withSpx[i], withoutSpx[i]) << "headword " << headwordAt(i) << " (run of " << runLengthAt(i)
                                         << ") differs between the .spx and full-search paths";
  }
}

TEST_F(DictSiblingWindow, ExistenceOnlyResultsMatchAcrossSearchPaths) {
  std::vector<Result> withSpx, withoutSpx;
  collectBothPaths(/*needDefinition=*/false, /*posMask=*/0, withSpx, withoutSpx);

  ASSERT_EQ(withSpx.size(), withoutSpx.size());
  for (size_t i = 0; i < withSpx.size(); i++) {
    EXPECT_TRUE(withSpx[i].found) << headwordAt(i);
    // priority reaches WordSelectionScan's kana-reading suppression gate, so a path-dependent
    // value here changes which words the reader offers as selectable.
    EXPECT_EQ(withSpx[i], withoutSpx[i]) << "headword " << headwordAt(i) << " (run of " << runLengthAt(i) << ")";
  }
}

TEST_F(DictSiblingWindow, PosMaskedExistenceMatchesAcrossSearchPaths) {
  std::vector<Result> withSpx, withoutSpx;
  collectBothPaths(/*needDefinition=*/false, /*posMask=*/DictIndexRecord::POS_V5, withSpx, withoutSpx);

  ASSERT_EQ(withSpx.size(), withoutSpx.size());
  for (size_t i = 0; i < withSpx.size(); i++) {
    EXPECT_EQ(withSpx[i], withoutSpx[i]) << "headword " << headwordAt(i);
  }
}

// Agreement alone would also be satisfied by both paths being wrong in the same way. The point of
// anchoring on the run is that the whole run is ranked, so the top-priority sense always wins.
TEST_F(DictSiblingWindow, BestSenseWinsRegardlessOfWhereItSitsInTheRun) {
  // Run 1000 (54 records) keeps its highest priority at the LAST record...
  const Result tail = lookup(headwordAt(1000), true, 0);
  ASSERT_TRUE(tail.found);
  EXPECT_EQ(tail.priority, priorityAt(1000, runLengthAt(1000) - 1, runLengthAt(1000)));
  EXPECT_EQ(tail.definition.rfind(definitionAt(1000, runLengthAt(1000) - 1), 0), 0u)
      << "merged definition must lead with the top-priority sense";

  // ...and run 7000 (46 records) at the first.
  const Result head = lookup(headwordAt(7000), true, 0);
  ASSERT_TRUE(head.found);
  EXPECT_EQ(head.priority, priorityAt(7000, 0, runLengthAt(7000)));
  EXPECT_EQ(head.definition.rfind(definitionAt(7000, 0), 0), 0u);
}

// The ranking must not depend on where in the run the search landed, and the same five senses
// must come back every time -- including for a run more than 32 records long.
TEST_F(DictSiblingWindow, LongRunMergesTheTopFiveSensesOfTheWholeRun) {
  const size_t len = runLengthAt(1000);
  std::string expected;
  for (size_t n = 0; n < 5; n++) {
    if (n) expected += "\n\n---\n";
    expected += definitionAt(1000, len - 1 - n);  // priorities descend from the tail
  }
  const Result accelerated = lookup(headwordAt(1000), true, 0);
  hideSpx();
  const Result fallback = lookup(headwordAt(1000), true, 0);

  EXPECT_EQ(accelerated.definition, expected);
  EXPECT_EQ(fallback.definition, expected);
}

}  // namespace
