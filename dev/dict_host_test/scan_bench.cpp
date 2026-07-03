// Dev-only: measure SD-read volume of a Word Lookup page scan, mimicking
// EpubReaderWordLookupActivity::buildSelectableGlyphs (WordLookup::lookup at each char position,
// skipping matched spans). Run with .spx present vs absent to see the read reduction.
//
//   CP_SD_ROOT=/abs/sdcard ./scan_bench

#include <cstdio>
#include <cstdlib>
#include <string>

#include "DictIndex.h"
#include "HalStorage.h"
#include "WordLookup.h"

namespace {
size_t utf8Bytes(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  return 4;
}

// ~170 chars of dense Japanese prose (representative of a vertical reader page).
const char* kPage =
    "その家の間取り図を見た瞬間、私は言葉を失った。玄関を入ってすぐの廊下は不自然に長く、"
    "リビングの奥にはなぜか窓のない小さな部屋が隠されている。二階へ上がる階段の位置もおかしい。"
    "設計者は一体何を考えていたのだろうか。妻に相談すると、彼女もまた同じ違和感を覚えたようだった。"
    "私たちはその晩、遅くまでその図面を眺めながら、いくつもの仮説を立てては消していった。";
}  // namespace

int main() {
  const char* root = std::getenv("CP_SD_ROOT");
  if (root) Storage.root_ = root;

  std::string text = kPage;
  halstub::g_reads = 0;
  halstub::g_seeks = 0;
  DictIndex::logAndResetStats("warmup-reset");  // zero the lookupExact counters

  size_t positions = 0;
  size_t skipUntilByte = 0;
  for (size_t off = 0; off < text.size();) {
    size_t step = utf8Bytes(static_cast<unsigned char>(text[off]));
    if (off < skipUntilByte) {
      off += step;
      continue;
    }
    positions++;
    WordLookupResult r;
    // Scan-mode: needDefinition=false, mirroring buildSelectableGlyphs.
    if (WordLookup::lookup(text, off, r, false) && r.matchLength > step) {
      skipUntilByte = off + r.matchLength;  // skip the matched span, like buildSelectableGlyphs
    }
    off += step;
  }

  std::printf("positions scanned: %zu\n", positions);
  std::printf("SD reads: %ld, seeks: %ld\n", halstub::g_reads, halstub::g_seeks);
  DictIndex::logAndResetStats("scan");
  return 0;
}
