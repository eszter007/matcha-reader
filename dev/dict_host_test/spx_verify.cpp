// Dev-only: verify the two-tier .spx sparse index produces IDENTICAL DictIndex results to the
// full binary search, against the real dictionaries under sdcard/dict.
//
// Run it twice from spx_verify.sh: once with the .spx files present (accelerated path) and once
// with them renamed away (fallback path), then diff the two outputs. Any difference means the
// sparse index mis-locates a lookup.
//
//   CP_SD_ROOT=/abs/path/to/sdcard ./spx_verify > out.txt
//
// Output is one deterministic line per probe: "name|keyhex|found|priority|deflen|defcrc".

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "DictIndex.h"
#include "HalStorage.h"

namespace {
constexpr size_t RECORD_SIZE = 40;
constexpr size_t HW = 32;

struct Dict {
  const char* name;
  const char* idx;
  const char* dat;
};
const Dict kDicts[] = {
    {"jmdict", DictIndex::IDX_PATH, DictIndex::DAT_PATH},
    {"grammar", DictIndex::GRAMMAR_IDX_PATH, DictIndex::GRAMMAR_DAT_PATH},
    {"jmnedict", DictIndex::NAMES_IDX_PATH, DictIndex::NAMES_DAT_PATH},
};

uint32_t crc32(const std::string& s) {
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : s) {
    c ^= b;
    for (int i = 0; i < 8; i++) c = (c >> 1) ^ (0xEDB88320u & (-(c & 1)));
  }
  return ~c;
}

// Read the null-trimmed headword of record r straight from the real .idx file.
std::string readHeadword(FILE* f, size_t r) {
  std::fseek(f, static_cast<long>(r * RECORD_SIZE), SEEK_SET);
  char hw[HW];
  if (std::fread(hw, 1, HW, f) != HW) return "";
  size_t n = 0;
  while (n < HW && hw[n]) n++;
  return std::string(hw, n);
}

void probe(const Dict& d, const std::string& key) {
  DictEntry e;
  bool found = DictIndex::lookupInFile(key.c_str(), d.idx, d.dat, e);
  // key as hex so multibyte prints cleanly and diffs exactly
  std::string kh;
  char buf[4];
  for (unsigned char b : key) {
    std::snprintf(buf, sizeof(buf), "%02x", b);
    kh += buf;
  }
  std::printf("%s|%s|%d|%d|%d|%08x\n", d.name, kh.c_str(), found ? 1 : 0,
              found ? e.priority : -1, found ? (int)e.definition.size() : -1,
              found ? crc32(e.definition) : 0u);
}
}  // namespace

int main() {
  const char* root = std::getenv("CP_SD_ROOT");
  if (root) Storage.root_ = root;

  for (const auto& d : kDicts) {
    std::string idxPath = std::string(Storage.root_) + d.idx;
    FILE* f = std::fopen(idxPath.c_str(), "rb");
    if (!f) {
      std::fprintf(stderr, "skip %s: cannot open %s\n", d.name, idxPath.c_str());
      continue;
    }
    std::fseek(f, 0, SEEK_END);
    size_t count = std::ftell(f) / RECORD_SIZE;
    std::fseek(f, 0, SEEK_SET);

    // Deterministic coverage: every stride-th record (hits, incl. duplicate-headword runs) plus
    // a derived miss key for each. Dense enough to hit every block/boundary transition.
    size_t stride = count > 40000 ? count / 40000 : 1;
    for (size_t r = 0; r < count; r += stride) {
      std::string hw = readHeadword(f, r);
      if (hw.empty()) continue;
      probe(d, hw);                 // exact hit
      probe(d, hw + "\x01");        // near-miss just after this headword
      if (hw.size() > 3) probe(d, hw.substr(0, hw.size() - 1));  // prefix (hit or miss)
    }
    std::fclose(f);
  }
  return 0;
}
