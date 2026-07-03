#include "EpubReaderWordLookupActivity.h"

#include <Arduino.h>
#include <DictIndex.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WordLookup.h>

#include "CrossPointSettings.h"
#include "Epub/Kinsoku.h"
#include "Epub/Page.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kMaxLookupChars = 8;

// Bare vector growth (reserve()/push_back() past capacity) calls operator new, which aborts the
// whole device on OOM under -fno-exceptions instead of returning nullptr (see CLAUDE.md). Word
// Lookup can be opened right after a heap-starved chapter build (a furigana-dense CJK page can
// leave the heap down to ~15-20KB, confirmed on a real device: an unguarded reserve() here
// aborted with exactly that heap profile). Same doubling-then-linear-fallback guard as
// VerticalParsedText.cpp's pushGlyph()/canPushStreamChar() -- a dropped glyph here just means one
// character isn't selectable for lookup, a far better failure mode than crashing the reader.
constexpr uint32_t SMALL_ALLOC_MARGIN = 16 * 1024;
constexpr size_t LINEAR_GROWTH_STEP = 32;

// Returns true for any character that could be part of a Japanese word.
// Punctuation, whitespace, and formatting marks are excluded.
// No hiragana skip list — filtering happens at the output stage instead,
// so multi-char words starting with particles (から, ところ) are found.
bool isLookupableChar(uint32_t cp) {
  if (cp < 0x30) return false;
  if (Kinsoku::needsVerticalRotation(cp)) return false;
  if (Kinsoku::verticalShiftType(cp) != 0) return false;
  // Small kana (っゃゅょ) are valid word characters — don't filter them.
  // They have special vertical positioning but are part of words like どっさり, ちゃん.
  if (cp == 0xFE45 || cp == 0xFE46) return false;
  if (cp == 0x30FC) return false;
  if (cp == 0x30FB) return false;
  if (cp == 0x2026 || cp == 0x2025) return false;
  if (cp >= '0' && cp <= '9') return false;
  if (cp >= 0xFF10 && cp <= 0xFF19) return false;
  if (cp >= 0x3040 && cp <= 0x309F) return true;  // Hiragana
  if (cp >= 0x30A0 && cp <= 0x30FF) return true;  // Katakana
  if (cp >= 0x4E00 && cp <= 0x9FFF) return true;  // CJK Unified
  if (cp >= 0x3400 && cp <= 0x4DBF) return true;  // CJK Ext A
  if (cp >= 0xF900 && cp <= 0xFAFF) return true;  // CJK Compat
  return cp >= 0x80;
}

bool isKatakana(uint32_t cp) {
  return (cp >= 0x30A0 && cp <= 0x30FF) || cp == 0x30FC ||
         (Kinsoku::isSmallKana(cp) && cp >= 0x30A0);
}

bool isHiragana(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x309F) ||
         (Kinsoku::isSmallKana(cp) && cp < 0x30A0);
}

bool isCJK(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
         (cp >= 0xF900 && cp <= 0xFAFF);
}

bool isDigitCp(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19);
}

// A greedy dictionary match can absorb a trailing case-particle onto a kanji
// stem (東の, 私は) and land on a junk entry. When the match is [kanji…][particle]
// and the stem alone is itself a valid word, shorten `result` to the stem.
// Only fires when the char before the particle is a kanji, so hiragana
// compounds (もの, こと) are left intact.
bool stripTrailingParticle(const std::string& text, WordLookupResult& result, bool needDefinition = true) {
  if (result.matchLength == 0) return false;
  size_t pos = 0, lastStart = 0;
  uint32_t lastCp = 0, prevCp = 0;
  int chars = 0;
  while (pos < result.matchLength && pos < text.size()) {
    prevCp = lastCp;
    lastStart = pos;
    auto c = static_cast<unsigned char>(text[pos]);
    if (c < 0x80) {
      lastCp = c;
      pos += 1;
    } else if ((c & 0xE0) == 0xC0) {
      lastCp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[pos + 1]) & 0x3F);
      pos += 2;
    } else if ((c & 0xF0) == 0xE0) {
      lastCp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(text[pos + 2]) & 0x3F);
      pos += 3;
    } else {
      lastCp = 0;
      pos += 4;
    }
    chars++;
  }
  if (chars < 2) return false;

  // Trailing case/possessive particles: の は が を に へ も と
  const bool isParticle = lastCp == 0x306E || lastCp == 0x306F || lastCp == 0x304C || lastCp == 0x3092 ||
                          lastCp == 0x306B || lastCp == 0x3078 || lastCp == 0x3082 || lastCp == 0x3068;
  if (!isParticle) return false;
  // Only strip a trailing particle when the preceding char is a kanji (東の→東,
  // 私は→私). Kana stems are ambiguous — ちょっと's と is part of the word, not a
  // particle — so never strip after kana.
  if (!isCJK(prevCp)) return false;

  std::string stem = text.substr(0, lastStart);
  WordLookupResult sr;
  if (WordLookup::lookup(stem, 0, sr, needDefinition) && sr.matchLength == stem.size()) {
    result = std::move(sr);
    return true;
  }
  return false;
}
}

void EpubReaderWordLookupActivity::reserveGlyphsSafe(std::vector<GlyphRef>& vec, size_t count) {
  if (count <= vec.capacity()) return;
  const size_t requestBytes = count * sizeof(GlyphRef);
  if (ESP.getMaxAllocHeap() < requestBytes + SMALL_ALLOC_MARGIN) {
    LOG_ERR("WLKP", "Skipping glyph reserve (%u bytes doesn't fit, free=%u); growing incrementally",
            static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
    return;
  }
  vec.reserve(count);
}

bool EpubReaderWordLookupActivity::pushGlyphSafe(std::vector<GlyphRef>& vec, const GlyphRef& g) {
  if (vec.size() < vec.capacity()) {
    vec.push_back(g);
    return true;
  }
  const size_t doubledCapacity = vec.capacity() == 0 ? 8 : vec.capacity() * 2;
  const size_t doubledBytes = doubledCapacity * sizeof(GlyphRef);
  if (ESP.getMaxAllocHeap() >= doubledBytes + SMALL_ALLOC_MARGIN) {
    vec.reserve(doubledCapacity);
    vec.push_back(g);
    return true;
  }
  const size_t linearCapacity = vec.capacity() + LINEAR_GROWTH_STEP;
  const size_t linearBytes = linearCapacity * sizeof(GlyphRef);
  if (ESP.getMaxAllocHeap() >= linearBytes + SMALL_ALLOC_MARGIN) {
    vec.reserve(linearCapacity);
    vec.push_back(g);
    return true;
  }
  LOG_ERR("WLKP", "Low heap (%u bytes, need ~%u); dropping glyph", ESP.getMaxAllocHeap(),
          static_cast<unsigned>(linearBytes));
  return false;
}

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                            const VerticalPage& page)
    : Activity("WordLookup", renderer, mappedInput) {
  reserveGlyphsSafe(allGlyphs, page.glyphs.size());
  for (const auto& g : page.glyphs) {
    if (g.renderKind == VerticalGlyph::RotatedRun) continue;
    GlyphRef ref{g.x, g.y, g.column, g.row, g.codepoint, g.paragraphIndex, false};
    if (!pushGlyphSafe(allGlyphs, ref)) break;
  }
  // Diagnostic timing for the Word Lookup slowness investigation -- see DictIndex::logAndResetStats().
  const uint32_t scanStart = millis();
  LOG_INF("WLA", "buildSelectableGlyphs (vertical): scanning %u characters", static_cast<unsigned>(allGlyphs.size()));
  buildSelectableGlyphs();
  LOG_INF("WLA", "buildSelectableGlyphs (vertical): done in %u ms, %u selectable", millis() - scanStart,
          static_cast<unsigned>(selectableGlyphs.size()));
  DictIndex::logAndResetStats("buildSelectableGlyphs (vertical)");
}

EpubReaderWordLookupActivity::EpubReaderWordLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                            const Page& page)
    : Activity("WordLookup", renderer, mappedInput) {
  // Horizontal mode: flatten the page's lines into one continuous character
  // stream (single paragraph). Latin words keep their separating spaces; CJK
  // runs are concatenated directly so dictionary lookups see contiguous text.
  auto isAsciiWord = [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  };
  uint32_t lastCp = 0;
  bool oom = false;
  for (const auto& el : page.elements) {
    if (oom) break;
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (!line.getBlock()) continue;
    for (const auto& word : line.getBlock()->getWords()) {
      if (oom) break;
      if (word.empty()) continue;
      // Insert a separating space only between two ASCII-word boundaries.
      if (lastCp && isAsciiWord(static_cast<unsigned char>(lastCp)) &&
          isAsciiWord(static_cast<unsigned char>(word[0]))) {
        if (!pushGlyphSafe(allGlyphs, GlyphRef{0, 0, 0, 0, ' ', 0, false})) {
          oom = true;
          break;
        }
      }
      size_t b = 0;
      while (b < word.size()) {
        auto c0 = static_cast<unsigned char>(word[b]);
        uint32_t cp;
        if (c0 < 0x80) {
          cp = c0;
          b += 1;
        } else if ((c0 & 0xE0) == 0xC0) {
          cp = (c0 & 0x1F) << 6 | (static_cast<unsigned char>(word[b + 1]) & 0x3F);
          b += 2;
        } else if ((c0 & 0xF0) == 0xE0) {
          cp = (c0 & 0x0F) << 12 | (static_cast<unsigned char>(word[b + 1]) & 0x3F) << 6 |
               (static_cast<unsigned char>(word[b + 2]) & 0x3F);
          b += 3;
        } else {
          cp = (c0 & 0x07) << 18 | (static_cast<unsigned char>(word[b + 1]) & 0x3F) << 12 |
               (static_cast<unsigned char>(word[b + 2]) & 0x3F) << 6 |
               (static_cast<unsigned char>(word[b + 3]) & 0x3F);
          b += 4;
        }
        if (!pushGlyphSafe(allGlyphs, GlyphRef{0, 0, 0, 0, cp, 0, false})) {
          oom = true;
          break;
        }
        lastCp = cp;
      }
    }
  }
  // Diagnostic timing for the Word Lookup slowness investigation -- see DictIndex::logAndResetStats().
  const uint32_t scanStart = millis();
  LOG_INF("WLA", "buildSelectableGlyphs (horizontal): scanning %u characters",
          static_cast<unsigned>(allGlyphs.size()));
  buildSelectableGlyphs();
  LOG_INF("WLA", "buildSelectableGlyphs (horizontal): done in %u ms, %u selectable", millis() - scanStart,
          static_cast<unsigned>(selectableGlyphs.size()));
  DictIndex::logAndResetStats("buildSelectableGlyphs (horizontal)");
}

void EpubReaderWordLookupActivity::buildSelectableGlyphs() {
  // Pre-scan: walk through all characters, do dictionary lookups to find
  // word boundaries. Characters inside a matched word are skipped.
  // Filtered particles (は、が etc.) are still checked — if they start a
  // multi-char match (e.g. という, ことになる), they become selectable.
  reserveGlyphsSafe(selectableGlyphs, allGlyphs.size());
  size_t skipUntil = 0;
  for (size_t i = 0; i < allGlyphs.size(); i++) {
    const auto& g = allGlyphs[i];
    if (i < skipUntil) continue;

    const uint32_t paraIdx = g.paragraphIndex;

    // Skip leading digits — a number is looked up together with the counter
    // that follows (2年, １５人) so the reading is clear. The dictionary match is
    // on the counter word; the digits are a display prefix.
    size_t scanStart = i;
    int digitGlyphs = 0;
    while (scanStart < allGlyphs.size() && allGlyphs[scanStart].paragraphIndex == paraIdx &&
           isDigitCp(allGlyphs[scanStart].codepoint)) {
      scanStart++;
      digitGlyphs++;
    }
    // A digit run with nothing after it isn't a lookup position.
    if (digitGlyphs > 0 && (scanStart >= allGlyphs.size() || allGlyphs[scanStart].paragraphIndex != paraIdx)) {
      continue;
    }

    // Build lookup text from the first non-digit char
    std::string text;
    int charCount = 0;
    for (size_t j = scanStart; j < allGlyphs.size() && charCount < kMaxLookupChars; j++) {
      if (allGlyphs[j].paragraphIndex != paraIdx) break;
      encodeUtf8(allGlyphs[j].codepoint, text);
      charCount++;
    }

    // Try dictionary lookup to find match length. The definition text is only consulted by the
    // hiragana/[kana] suppression check below, which only runs for positions that start with
    // neither kanji nor katakana -- for every other position, skip fetching definitions from the
    // .dat file entirely (1-5 SD reads saved per match; the real definition is fetched fresh when
    // the user selects a word).
    const bool needDef = !isCJK(allGlyphs[scanStart].codepoint) && !isKatakana(allGlyphs[scanStart].codepoint);
    WordLookupResult result;
    bool hasMatch = !text.empty() && WordLookup::lookup(text, 0, result, needDef);
    int matchChars = 0;
    if (hasMatch) {
      stripTrailingParticle(text, result, needDef);
      size_t pos = 0;
      while (pos < result.matchLength && pos < text.size()) {
        auto c = static_cast<unsigned char>(text[pos]);
        if (c < 0x80) pos += 1;
        else if ((c & 0xE0) == 0xC0) pos += 2;
        else if ((c & 0xF0) == 0xE0) pos += 3;
        else pos += 4;
        matchChars++;
      }

      // Suppress hiragana-only text matching a kanji headword via reading
      // collision (ました→真下, ぶ→武 — conjugation fragments). But KEEP entries
      // marked "usually kana" ([kana]): ちょっと→一寸, とても→迚も are real words
      // that happen to have a kanji headword.
      if (!isCJK(allGlyphs[scanStart].codepoint) && !isKatakana(allGlyphs[scanStart].codepoint)) {
        bool matchAllKana = true;
        for (size_t ci = scanStart; ci < scanStart + static_cast<size_t>(matchChars) && ci < allGlyphs.size(); ci++) {
          if (isCJK(allGlyphs[ci].codepoint) || isKatakana(allGlyphs[ci].codepoint)) {
            matchAllKana = false; break;
          }
        }
        if (matchAllKana) {
          bool hwHasKanji = false;
          for (size_t hb = 0; hb < result.entry.headword.size();) {
            auto hc = static_cast<unsigned char>(result.entry.headword[hb]);
            uint32_t hcp = 0;
            if (hc < 0x80) { hcp = hc; hb += 1; }
            else if ((hc & 0xE0) == 0xC0) { hcp = ((hc & 0x1F) << 6) | (result.entry.headword[hb+1] & 0x3F); hb += 2; }
            else if ((hc & 0xF0) == 0xE0) { hcp = ((hc & 0x0F) << 12) | ((result.entry.headword[hb+1] & 0x3F) << 6) | (result.entry.headword[hb+2] & 0x3F); hb += 3; }
            else { hb += 4; }
            if (isCJK(hcp)) { hwHasKanji = true; break; }
          }
          const bool usuallyKana = result.entry.definition.find("[kana]") != std::string::npos;
          if (hwHasKanji && !usuallyKana) hasMatch = false;
        }
      }

      // A case particle that matched exactly 2 chars (にど, のと) usually
      // mis-segments: the 2nd character starts a longer real word (どっさり,
      // ところ). If so, don't let the particle consume it — pass through so the
      // next char begins the longer word.
      auto isCaseParticle = [](uint32_t cp) {
        return cp == 0x306B || cp == 0x306E || cp == 0x3068 || cp == 0x304C || cp == 0x306F ||
               cp == 0x3092 || cp == 0x3082 || cp == 0x3067 || cp == 0x3078 || cp == 0x3084 || cp == 0x304B;
      };
      if (hasMatch && matchChars == 2 && isCaseParticle(allGlyphs[scanStart].codepoint) &&
          scanStart + 1 < allGlyphs.size() && allGlyphs[scanStart + 1].paragraphIndex == paraIdx) {
        std::string nextText;
        int nc = 0;
        for (size_t j = scanStart + 1; j < allGlyphs.size() && nc < kMaxLookupChars; j++) {
          if (allGlyphs[j].paragraphIndex != paraIdx) break;
          encodeUtf8(allGlyphs[j].codepoint, nextText);
          nc++;
        }
        WordLookupResult nr;
        // Only the match length is used here -- never fetch definitions.
        if (!nextText.empty() && WordLookup::lookup(nextText, 0, nr, /*needDefinition=*/false)) {
          int nmc = 0;
          size_t pp = 0;
          while (pp < nr.matchLength && pp < nextText.size()) {
            auto c = static_cast<unsigned char>(nextText[pp]);
            if (c < 0x80) pp += 1;
            else if ((c & 0xE0) == 0xC0) pp += 2;
            else if ((c & 0xF0) == 0xE0) pp += 3;
            else pp += 4;
            nmc++;
          }
          if (nmc >= 2) hasMatch = false;  // let the next char start the longer word
        }
      }

      if (hasMatch && (matchChars > 1 || digitGlyphs > 0)) {
        skipUntil = scanStart + matchChars;
        // Extend skip for honorifics after names
        size_t afterMatch = scanStart + matchChars;
        if (afterMatch < allGlyphs.size() && allGlyphs[afterMatch].paragraphIndex == paraIdx) {
          uint32_t nextCp = allGlyphs[afterMatch].codepoint;
          if (nextCp == 0x3055) { // さ → さん、さま
            if (afterMatch + 1 < allGlyphs.size()) {
              uint32_t nn = allGlyphs[afterMatch + 1].codepoint;
              if (nn == 0x3093) skipUntil = afterMatch + 2;
              if (nn == 0x307E && afterMatch + 2 < allGlyphs.size() && allGlyphs[afterMatch + 2].paragraphIndex == paraIdx) skipUntil = afterMatch + 2;
            }
          } else if (nextCp == 0x304F) { // く → くん
            if (afterMatch + 1 < allGlyphs.size() && allGlyphs[afterMatch + 1].codepoint == 0x3093)
              skipUntil = afterMatch + 2;
          } else if (nextCp == 0x3061) { // ち → ちゃん
            if (afterMatch + 2 < allGlyphs.size() && allGlyphs[afterMatch + 1].codepoint == 0x3083 && allGlyphs[afterMatch + 2].codepoint == 0x3093)
              skipUntil = afterMatch + 3;
          } else if (nextCp == 0x6C0F || nextCp == 0x69D8) { // 氏、様
            skipUntil = afterMatch + 1;
          } else if (nextCp == 0x58EB || nextCp == 0x5E2B || nextCp == 0x54E1) { // 士・師・員
            if (isCJK(allGlyphs[scanStart + matchChars - 1].codepoint)) skipUntil = afterMatch + 1;
          }
        }
      }
    }

    // Include this position if it has a dictionary match
    if (hasMatch) {
      // Push selectableGlyphs first -- if it can't grow, the heap is exhausted, so stop the scan
      // rather than push a mismatched selectToAllIdx entry with no corresponding glyph.
      if (!pushGlyphSafe(selectableGlyphs, g)) break;
      selectToAllIdx.push_back(i);
    }
  }

  // Post-scan display filter: remove single-char entries that are common
  // particles or conjugation fragments. These are valid in the scan (so から,
  // ところ etc. get found correctly) but useless as standalone lookup results.
  auto isDisplayNoise = [](uint32_t cp) -> bool {
    switch (cp) {
      case 0x306F: // は
      case 0x304C: // が
      case 0x306E: // の
      case 0x306B: // に
      case 0x3067: // で
      case 0x3092: // を
      case 0x3082: // も
      case 0x3068: // と
      case 0x304B: // か
      case 0x306A: // な
      case 0x3078: // へ
      case 0x3088: // よ
      case 0x306D: // ね
      case 0x308F: // わ
      case 0x3066: // て
      case 0x3060: // だ
      case 0x305F: // た
      case 0x308B: // る
      case 0x3093: // ん
      case 0x3044: // い
      case 0x304F: // く
      case 0x3057: // し
      case 0x3055: // さ
      case 0x305B: // せ
      case 0x3089: // ら
      case 0x304D: // き
      case 0x3053: // こ
      case 0x305D: // そ
      case 0x3042: // あ
      case 0x304A: // お (honorific prefix)
      case 0x307E: // ま
      case 0x3059: // す
      case 0x308C: // れ
      case 0x3079: // べ
      case 0x305E: // ぞ
      case 0x3081: // め
      case 0x3076: // ぶ
      case 0x307F: // み
      case 0x3064: // つ
      case 0x306C: // ぬ
      case 0x3075: // ふ
      case 0x3080: // む
        return true;
      default:
        return false;
    }
  };

  // Also filter multi-char conjugation suffixes that are never standalone words.
  auto isConjugationNoise = [&](size_t readIdx) -> bool {
    const size_t allIdx = selectToAllIdx[readIdx];
    const uint32_t paraIdx = allGlyphs[allIdx].paragraphIndex;
    // Build the matched string (up to 4 chars)
    std::string matched;
    int mLen = 0;
    for (size_t j = allIdx; j < allGlyphs.size() && mLen < 4; j++) {
      if (allGlyphs[j].paragraphIndex != paraIdx) break;
      encodeUtf8(allGlyphs[j].codepoint, matched);
      mLen++;
    }
    // Check against common conjugation patterns
    static const char* const patterns[] = {
      "\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f",     // ました
      "\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93",     // ません
      "\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f",     // でした
      "\xe3\x81\xa7\xe3\x81\x99",                   // です
      "\xe3\x81\xbe\xe3\x81\x99",                   // ます
      nullptr
    };
    for (int p = 0; patterns[p]; p++) {
      size_t plen = strlen(patterns[p]);
      if (matched.size() >= plen && matched.substr(0, plen) == patterns[p]) return true;
    }
    return false;
  };

  // For each entry, do a quick performLookup-style check to see what
  // the actual displayed match would be, and filter noise from that.
  size_t writeIdx = 0;
  for (size_t readIdx = 0; readIdx < selectableGlyphs.size(); readIdx++) {
    const size_t allIdx = selectToAllIdx[readIdx];
    const uint32_t paraIdx = allGlyphs[allIdx].paragraphIndex;

    // Build lookup text from this position
    std::string ltext;
    int lcount = 0;
    for (size_t j = allIdx; j < allGlyphs.size() && lcount < kMaxLookupChars; j++) {
      if (allGlyphs[j].paragraphIndex != paraIdx) break;
      encodeUtf8(allGlyphs[j].codepoint, ltext);
      lcount++;
    }
    WordLookupResult lr;
    // Only the match length is consulted below -- skip definition fetches.
    if (!ltext.empty() && WordLookup::lookup(ltext, 0, lr, /*needDefinition=*/false)) {
      stripTrailingParticle(ltext, lr, /*needDefinition=*/false);
      // Count matched chars
      int mc = 0;
      size_t p = 0;
      while (p < lr.matchLength && p < ltext.size()) {
        auto c = static_cast<unsigned char>(ltext[p]);
        if (c < 0x80) p += 1;
        else if ((c & 0xE0) == 0xC0) p += 2;
        else if ((c & 0xF0) == 0xE0) p += 3;
        else p += 4;
        mc++;
      }
      // Filter: all matched chars are noise particles → skip
      bool allNoise = true;
      for (size_t ci = allIdx; ci < allIdx + static_cast<size_t>(mc) && ci < allGlyphs.size(); ci++) {
        if (!isDisplayNoise(allGlyphs[ci].codepoint)) { allNoise = false; break; }
      }
      if (allNoise) continue;

      // Exact-match colloquial/grammatical contractions that are never useful as
      // standalone vocabulary (なくちゃ→ちゃ, じゃ, etc.). Use the exact matched
      // text so real words (茶碗/ちゃわん) are not filtered.
      const std::string matchedText = ltext.substr(0, lr.matchLength);
      static const char* const exactNoise[] = {
        "\xe3\x81\xa1\xe3\x82\x83",  // ちゃ
        "\xe3\x81\x98\xe3\x82\x83",  // じゃ
        "\xe3\x81\xa1\xe3\x82\x83\xe3\x81\x86",  // ちゃう
        "\xe3\x81\xa3\xe3\x81\xa6",  // って
        nullptr
      };
      bool isExactNoise = false;
      for (int e = 0; exactNoise[e]; e++) {
        if (matchedText == exactNoise[e]) { isExactNoise = true; break; }
      }
      if (isExactNoise) continue;
    }
    if (isConjugationNoise(readIdx)) {
      continue;
    }
    selectableGlyphs[writeIdx] = selectableGlyphs[readIdx];
    selectToAllIdx[writeIdx] = selectToAllIdx[readIdx];
    writeIdx++;
  }
  selectableGlyphs.resize(writeIdx);
  selectToAllIdx.resize(writeIdx);
}

void EpubReaderWordLookupActivity::onEnter() {
  Activity::onEnter();
  // Find first position with a match
  const int maxIdx = static_cast<int>(selectableGlyphs.size()) - 1;
  for (cursorIndex = 0; cursorIndex <= maxIdx; cursorIndex++) {
    performLookup();
    if (hasResult) break;
  }
  if (cursorIndex > maxIdx) cursorIndex = 0;
  requestUpdate();
}

void EpubReaderWordLookupActivity::onExit() {
  // Return the dictionary cache memory (~30KB) to the pool -- the reader needs it for heavy
  // operations like re-pagination (zip inflate wants one contiguous 32KB block).
  DictIndex::releaseCaches();
  Activity::onExit();
}

void EpubReaderWordLookupActivity::moveCursor(int delta) {
  if (selectableGlyphs.empty()) return;
  const int maxIdx = static_cast<int>(selectableGlyphs.size()) - 1;
  // selectableGlyphs is already the pre-filtered list of positions buildSelectableGlyphs()
  // confirmed have a dictionary match -- every index in it is valid by construction, so this just
  // moves one step and shows whatever's there. The previous version re-validated via
  // performLookup() and kept advancing past any position where that didn't independently agree
  // with the scan, silently skipping entries end-users could never reach -- confirmed on a real
  // device as "every second entry is skipped" during navigation (1, 3, 5, 7, ...).
  cursorIndex += delta;
  if (cursorIndex < 0) cursorIndex = 0;
  if (cursorIndex > maxIdx) cursorIndex = maxIdx;
  performLookup();
}

void EpubReaderWordLookupActivity::encodeUtf8(uint32_t cp, std::string& out) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

std::string EpubReaderWordLookupActivity::buildLookupText(size_t startIdx) const {
  std::string text;
  if (startIdx >= selectableGlyphs.size() || startIdx >= selectToAllIdx.size()) return text;

  const size_t allStart = selectToAllIdx[startIdx];
  const uint32_t paraIdx = allGlyphs[allStart].paragraphIndex;
  int charCount = 0;

  for (size_t i = allStart; i < allGlyphs.size() && charCount < kMaxLookupChars; i++) {
    const auto& g = allGlyphs[i];
    if (g.paragraphIndex != paraIdx) break;
    encodeUtf8(g.codepoint, text);
    charCount++;
  }
  return text;
}

void EpubReaderWordLookupActivity::performLookup() {
  hasResult = false;
  resultHeadword.clear();
  resultDefinition.clear();
  resultMatchLen = 0;
  scrollOffset = 0;
  totalLines = 9999;

  std::string text = buildLookupText(static_cast<size_t>(cursorIndex));
  if (text.empty()) return;

  // If the text starts with digits (2年, １５人), look up the counter/word that
  // follows and show the digits as a prefix so the reading is clear (2年).
  std::string digitPrefix;
  {
    size_t b = 0;
    while (b < text.size()) {
      auto c = static_cast<unsigned char>(text[b]);
      if (c >= '0' && c <= '9') {
        digitPrefix.push_back(static_cast<char>(c));
        b += 1;
      } else if (c == 0xEF && b + 2 < text.size() &&
                 static_cast<unsigned char>(text[b + 1]) == 0xBC &&
                 static_cast<unsigned char>(text[b + 2]) >= 0x90 &&
                 static_cast<unsigned char>(text[b + 2]) <= 0x99) {
        // Fullwidth digit ０-９ (U+FF10–U+FF19)
        digitPrefix.append(text, b, 3);
        b += 3;
      } else {
        break;
      }
    }
    if (b > 0 && b < text.size()) {
      text = text.substr(b);  // look up the part after the digits
    } else {
      digitPrefix.clear();  // nothing after digits, or no digits
    }
  }

  WordLookupResult result;
  if (WordLookup::lookup(text, 0, result)) {
    stripTrailingParticle(text, result);
    hasResult = true;
    resultHeadword = digitPrefix + result.entry.headword;
    resultDefinition = std::move(result.entry.definition);
    int chars = 0;
    size_t pos = 0;
    while (pos < result.matchLength && pos < text.size()) {
      auto c = static_cast<unsigned char>(text[pos]);
      if (c < 0x80) pos += 1;
      else if ((c & 0xE0) == 0xC0) pos += 2;
      else if ((c & 0xF0) == 0xE0) pos += 3;
      else pos += 4;
      chars++;
    }
    resultMatchLen = chars;

    // For short hiragana-only matches (≤3 chars), check if the grammar dict
    // has a better entry and promote it to the main result. Functional words
    // like こと, もの, よう get unhelpful JMdict hits ("ancient capital").
    if (chars <= 3 && Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
      bool allHiragana = true;
      for (size_t b = 0; b < result.matchLength && b < text.size();) {
        auto c = static_cast<unsigned char>(text[b]);
        uint32_t cp = 0;
        if (c < 0x80) { cp = c; b += 1; }
        else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (text[b+1] & 0x3F); b += 2; }
        else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | ((text[b+1] & 0x3F) << 6) | (text[b+2] & 0x3F); b += 3; }
        else { b += 4; }
        if (cp < 0x3040 || cp > 0x309F) { allHiragana = false; break; }
      }
      if (allHiragana) {
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(resultHeadword.c_str(), DictIndex::GRAMMAR_IDX_PATH,
                                     DictIndex::GRAMMAR_DAT_PATH, gramEntry)) {
          resultDefinition = std::move(gramEntry.definition);
        }
      }
    }
  }

  // Grammar scan: search for grammar patterns in a window around the cursor.
  // Try starting from a few characters BEFORE the cursor (to catch patterns
  // like ことになる when cursor is on こと) and also from the cursor itself.
  hasGrammar = false;
  grammarHeadword.clear();
  grammarDefinition.clear();
  if (Storage.exists(DictIndex::GRAMMAR_IDX_PATH)) {
    const size_t allStart = selectToAllIdx[static_cast<size_t>(cursorIndex)];
    const uint32_t paraIdx = allGlyphs[allStart].paragraphIndex;

    // Try starting positions: cursor-3, cursor-2, cursor-1, cursor
    int bestGramLen = 0;
    for (int backoff = 3; backoff >= 0; backoff--) {
      size_t scanStart = allStart;
      for (int b = 0; b < backoff && scanStart > 0; b++) {
        scanStart--;
        if (allGlyphs[scanStart].paragraphIndex != paraIdx) { scanStart++; break; }
      }

      std::string gramText;
      int gCharCount = 0;
      for (size_t j = scanStart; j < allGlyphs.size() && gCharCount < 12; j++) {
        if (allGlyphs[j].paragraphIndex != paraIdx) break;
        encodeUtf8(allGlyphs[j].codepoint, gramText);
        gCharCount++;
      }

      for (int wLen = std::min(gCharCount, 10); wLen >= 2; wLen--) {
        size_t byteEnd = 0;
        int cnt = 0;
        for (size_t b = 0; b < gramText.size() && cnt < wLen; cnt++) {
          auto c = static_cast<unsigned char>(gramText[b]);
          if (c < 0x80) b += 1;
          else if ((c & 0xE0) == 0xC0) b += 2;
          else if ((c & 0xF0) == 0xE0) b += 3;
          else b += 4;
          byteEnd = b;
        }
        std::string window = gramText.substr(0, byteEnd);
        DictEntry gramEntry;
        if (DictIndex::lookupInFile(window.c_str(), DictIndex::GRAMMAR_IDX_PATH,
                                     DictIndex::GRAMMAR_DAT_PATH, gramEntry)) {
          if (gramEntry.headword != resultHeadword && wLen > bestGramLen) {
            bestGramLen = wLen;
            hasGrammar = true;
            grammarHeadword = std::move(gramEntry.headword);
            grammarDefinition = std::move(gramEntry.definition);
          }
          break;
        }
      }
    }
  }

  // Merge grammar into the definition so the single scroll-aware render loop
  // handles it (gets maxDefY clamping and scroll offset for free).
  if (hasGrammar) {
    resultDefinition += "\n\n— Grammar: " + grammarHeadword + " —\n" + grammarDefinition;
  }

  requestUpdate();
}

void EpubReaderWordLookupActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    performLookup();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { moveCursor(1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { moveCursor(-1); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    if (hasResult && scrollOffset < maxScroll) { scrollOffset = std::min(maxScroll, scrollOffset + 5); requestUpdate(); }
  });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    if (scrollOffset > 0) { scrollOffset = std::max(0, scrollOffset - 5); requestUpdate(); }
  });
}

void EpubReaderWordLookupActivity::renderContentArea(const Rect& screen, int contentTop) {
  auto metrics = UITheme::getInstance().getMetrics();
  const int jaFont = SETTINGS.getReaderFontId();

  // Bulk-load every glyph the headword + definition need before drawing/measuring any of them --
  // same fix, and same root cause, as the vertical-page-turn slowness fixed earlier this session.
  // Without this, dictionary definitions (which merge up to 5 entries and can run to hundreds of
  // characters spanning many different compressed font groups) fall through the slow one-by-one
  // glyph fallback path a character at a time.
  if (hasResult) {
    if (auto* fcm = renderer.getFontCacheManager()) {
      fcm->clearCache();
      fcm->prewarmCache(jaFont, resultHeadword.c_str(), 1 << EpdFontFamily::BOLD);
      fcm->prewarmCache(SMALL_FONT_ID, resultDefinition.c_str(), 1 << EpdFontFamily::REGULAR);
    }
  }

  if (selectableGlyphs.empty() || !hasResult) {
    UITheme::drawCenteredText(renderer, screen, UI_12_FONT_ID,
                              screen.y + screen.height / 2, tr(STR_NO_MATCH), true);
  } else {
    const int maxWidth = screen.width - metrics.contentSidePadding * 2;
    const int textX = screen.x + metrics.contentSidePadding;

    int defY;

    if (scrollOffset == 0) {
      // Headword at the top (position counter is now in the header).
      renderer.drawText(jaFont, textX, contentTop, resultHeadword.c_str(), true, EpdFontFamily::BOLD);
      defY = contentTop + renderer.getLineHeight(jaFont) + metrics.verticalSpacing;
    } else {
      // When scrolled, show a compact header line with the headword + scroll mark.
      std::string scrollInfo = resultHeadword + " \xe2\x96\xb2";
      renderer.drawText(SMALL_FONT_ID, textX, contentTop, scrollInfo.c_str(), true);
      defY = contentTop + renderer.getLineHeight(SMALL_FONT_ID) + 4;
    }

    const int defFont = SMALL_FONT_ID;
    const int defLineH = renderer.getLineHeight(defFont);
    int linesDrawn = 0;
    int lineIndex = 0;
    // screen.height already excludes the button-hints band, so its bottom edge
    // is the top of the buttons; stay a hair above it.
    const int maxDefY = screen.y + screen.height - 2;
    const int firstDefY = defY;
    const int kMaxDefLines = 999;
    std::string defText = resultDefinition;
    size_t nlPos = 0;
    while (nlPos <= defText.size() && linesDrawn < kMaxDefLines) {
      size_t nextNl = defText.find('\n', nlPos);
      std::string paragraph = (nextNl == std::string::npos)
          ? defText.substr(nlPos) : defText.substr(nlPos, nextNl - nlPos);
      nlPos = (nextNl == std::string::npos) ? defText.size() + 1 : nextNl + 1;

      if (paragraph.empty()) {
        lineIndex++;
        if (lineIndex > scrollOffset) defY += defLineH / 2;
        continue;
      }
      // Try space-based wrapping first (for Latin text), then fall back to
      // character-level wrapping (for CJK text without spaces).
      std::string rem = paragraph;
      while (!rem.empty() && linesDrawn < kMaxDefLines) {
        if (renderer.getTextWidth(defFont, rem.c_str()) <= maxWidth) {
          lineIndex++;
          if (lineIndex > scrollOffset && defY + defLineH <= maxDefY) {
            renderer.drawText(defFont, textX, defY, rem.c_str(), true);
            defY += defLineH;
            linesDrawn++;
          }
          break;
        }
        // Try to break at the last space that fits
        std::string bestLine;
        size_t lastSpaceBreak = std::string::npos;
        std::string accum;
        const char* p = rem.c_str();
        while (*p) {
          size_t charLen = 1;
          auto c0 = static_cast<unsigned char>(*p);
          if (c0 >= 0xF0) charLen = 4;
          else if (c0 >= 0xE0) charLen = 3;
          else if (c0 >= 0xC0) charLen = 2;
          std::string test = accum + std::string(p, charLen);
          if (renderer.getTextWidth(defFont, test.c_str()) > maxWidth) {
            // Never orphan sentence-ending punctuation on its own line
            uint32_t nextCp = 0;
            if (charLen == 3) nextCp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(p[1]) & 0x3F) << 6) | (static_cast<unsigned char>(p[2]) & 0x3F);
            else if (charLen == 1) nextCp = c0;
            if (nextCp == 0x3002 || nextCp == 0x3001 || nextCp == 0xFF01 || nextCp == 0xFF1F ||
                nextCp == '.' || nextCp == ',' || nextCp == '!' || nextCp == '?') {
              accum = test;
              p += charLen;
            }
            break;
          }
          accum = test;
          if (*p == ' ') lastSpaceBreak = accum.size();
          p += charLen;
        }
        if (accum.empty()) {
          // Single char wider than maxWidth — force it
          auto c0 = static_cast<unsigned char>(rem[0]);
          size_t cl = 1;
          if (c0 >= 0xF0) cl = 4; else if (c0 >= 0xE0) cl = 3; else if (c0 >= 0xC0) cl = 2;
          accum = rem.substr(0, cl);
          rem = rem.substr(cl);
        } else if (lastSpaceBreak != std::string::npos && lastSpaceBreak > 0) {
          // Break at last space to keep Latin words intact
          std::string line = accum.substr(0, lastSpaceBreak);
          rem = rem.substr(lastSpaceBreak);
          // Skip leading space
          if (!rem.empty() && rem[0] == ' ') rem = rem.substr(1);
          accum = line;
        } else {
          // No space found — break at character boundary (CJK text)
          rem = rem.substr(accum.size());
        }
        lineIndex++;
        if (lineIndex > scrollOffset && defY + defLineH <= maxDefY) {
          renderer.drawText(defFont, textX, defY, accum.c_str(), true);
          defY += defLineH;
          linesDrawn++;
        }
      }
    }

    totalLines = lineIndex;
    // Leave at least a screenful visible: max scroll = total - capacity
    const int visibleCapacity = (maxDefY - firstDefY) / defLineH;
    maxScroll = std::max(0, totalLines - visibleCapacity);
  }
}

void EpubReaderWordLookupActivity::render(RenderLock&&) {
  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int footerHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int contentBottom = screen.y + screen.height - footerHeight;

  // Position counter (35/50) shown right-aligned on the header baseline.
  std::string posText;
  if (hasResult && !selectableGlyphs.empty()) {
    posText = std::to_string(cursorIndex + 1) + "/" + std::to_string(selectableGlyphs.size());
  }
  const Rect headerRect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight};

  if (!initialRenderDone) {
    renderer.clearScreen();

    GUI.drawHeader(renderer, headerRect, tr(STR_WORD_LOOKUP), posText.empty() ? nullptr : posText.c_str());

    renderContentArea(screen, contentTop);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    renderer.displayBuffer();
    initialRenderDone = true;
    fastRefreshCount = 0;
  } else {
    // Clear from content top all the way to the physical bottom (including the
    // button-hint band margins, which screen.height excludes), then redraw hints.
    const int physBottom = renderer.getScreenHeight();
    renderer.fillRect(0, contentTop, renderer.getScreenWidth(), physBottom - contentTop, false);
    // Redraw the header so the position counter updates (drawHeader clears it).
    GUI.drawHeader(renderer, headerRect, tr(STR_WORD_LOOKUP), posText.empty() ? nullptr : posText.c_str());
    const auto labels2 = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels2.btn1, labels2.btn2, labels2.btn3, labels2.btn4);

    renderContentArea(screen, contentTop);

    fastRefreshCount++;
    if (fastRefreshCount >= kFullRefreshInterval) {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      fastRefreshCount = 0;
    } else {
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
  }
}
