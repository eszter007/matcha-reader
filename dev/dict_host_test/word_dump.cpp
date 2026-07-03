// Dev-only: dump every word WordLookup finds while scanning a page, so the current (optimized)
// lookup can be diffed against the original for behaviour changes. Same page as scan_bench.
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
const char* kPage =
    "その家の間取り図を見た瞬間、私は言葉を失った。玄関を入ってすぐの廊下は不自然に長く、"
    "リビングの奥にはなぜか窓のない小さな部屋が隠されている。二階へ上がる階段の位置もおかしい。"
    "設計者は一体何を考えていたのだろうか。妻に相談すると、彼女もまた同じ違和感を覚えたようだった。"
    "私たちはその晩、遅くまでその図面を眺めながら、いくつもの仮説を立てては消していった。";
}  // namespace

int main(int argc, char** argv) {
  const char* root = std::getenv("CP_SD_ROOT");
  if (root) Storage.root_ = root;
  // "nodef" runs the scan-mode lookups (needDefinition=false); positions and match lengths must be
  // identical to the default mode -- only the definition text (not printed here) may differ.
  const bool needDef = !(argc > 1 && std::string(argv[1]) == "nodef");
  std::string text = kPage;
  for (size_t off = 0; off < text.size();) {
    size_t step = utf8Bytes(static_cast<unsigned char>(text[off]));
    WordLookupResult r;
    if (WordLookup::lookup(text, off, r, needDef)) {
      std::printf("%zu %zu %s\n", off, r.matchLength, r.entry.headword.c_str());
    }
    off += step;
  }
  return 0;
}
