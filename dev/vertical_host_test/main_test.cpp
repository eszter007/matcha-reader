#include <cstdio>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "VerticalParsedText.h"
#include "blocks/VerticalTextBlock.h"

int main() {
  GfxRenderer renderer;
  const uint16_t viewportWidth = 440;
  const uint16_t viewportHeight = 760;
  const int fontId = 1016;
  const int rubyFontId = 1012;

  // --- Test 1: Plain text (original test) ---
  std::printf("=== Test 1: Plain text ===\n");
  {
    VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);
    layout.addParagraph(
        "吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。"
        "何でも薄暗いじめじめした所でニャーニャー泣いていた事だけは記憶している。");
    layout.addParagraph("彼はCrossPoint Readerという端末で2024年に日本語の小説を読んでいた。「これは良い」と思った。");

    std::vector<VerticalPage> pages = layout.layoutPages();
    std::printf("Laid out 2 paragraph(s) into %zu page(s).\n", pages.size());

    for (size_t p = 0; p < pages.size(); p++) {
      const VerticalPage& page = pages[p];
      std::printf("\n--- Page %zu: %zu glyphs, %u columns x %u rows ---\n", p, page.glyphs.size(), page.columnCount,
                  page.rowsPerColumn);
      VerticalTextBlock block(page);
      block.render(renderer, fontId);
    }
  }

  // --- Test 2: Furigana/ruby annotations ---
  std::printf("\n\n=== Test 2: Furigana ===\n");
  {
    VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);

    // Simulate: 漢字(かんじ)を読(よ)む
    std::vector<VerticalParsedText::RubyRun> runs;
    runs.push_back({"漢字", "かんじ"});  // group ruby: 2 base, 3 ruby chars
    runs.push_back({"を", ""});          // plain
    runs.push_back({"読", "よ"});        // per-char ruby
    runs.push_back({"む。", ""});        // plain with punctuation
    layout.addAnnotatedParagraph(runs);

    // Second paragraph: mixed content with longer ruby
    std::vector<VerticalParsedText::RubyRun> runs2;
    runs2.push_back({"東京", "とうきょう"});  // 2 base, 5 ruby
    runs2.push_back({"は", ""});
    runs2.push_back({"美", "うつく"});  // 1 base, 3 ruby
    runs2.push_back({"しい", ""});
    runs2.push_back({"都市", "とし"});  // 2 base, 2 ruby (1:1)
    runs2.push_back({"です。", ""});
    layout.addAnnotatedParagraph(runs2);

    std::vector<VerticalPage> pages = layout.layoutPages();
    std::printf("Laid out 2 annotated paragraph(s) into %zu page(s).\n", pages.size());

    bool hasRuby = false;
    for (size_t p = 0; p < pages.size(); p++) {
      const VerticalPage& page = pages[p];
      std::printf("\n--- Page %zu: %zu glyphs ---\n", p, page.glyphs.size());
      VerticalTextBlock block(page);
      block.render(renderer, fontId, rubyFontId, 0, 0);

      for (const auto& g : page.glyphs) {
        if (!g.rubyText.empty()) {
          hasRuby = true;
          std::printf("  [ruby] base_cp=U+%04X ruby=\"%s\"\n", g.codepoint, g.rubyText.c_str());
        }
      }
    }

    if (!hasRuby) {
      std::printf("FAIL: no ruby annotations found in output glyphs\n");
      return 1;
    }
  }

  // --- Sanity checks across all tests ---
  std::printf("\n=== Running sanity checks ===\n");
  {
    VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);
    layout.addParagraph("吾輩は猫である。名前はまだ無い。");

    std::vector<VerticalParsedText::RubyRun> runs;
    runs.push_back({"漢字", "かんじ"});
    runs.push_back({"を読む。", ""});
    layout.addAnnotatedParagraph(runs);

    std::vector<VerticalPage> pages = layout.layoutPages();

    bool ok = true;
    for (const auto& page : pages) {
      for (const auto& g : page.glyphs) {
        if (g.column >= page.columnCount) {
          std::printf("FAIL: glyph column %u >= columnCount %u\n", g.column, page.columnCount);
          ok = false;
        }
        if (!g.rotated && g.row > page.rowsPerColumn) {
          std::printf("FAIL: upright glyph row %u > rowsPerColumn %u\n", g.row, page.rowsPerColumn);
          ok = false;
        }
      }
    }
    std::printf("\n%s\n", ok ? "SANITY CHECKS PASSED" : "SANITY CHECKS FAILED");
    if (!ok) return 1;
  }

  return 0;
}
