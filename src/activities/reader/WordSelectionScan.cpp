#include "WordSelectionScan.h"

#include <Arduino.h>
#include <DictIndex.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WordLookup.h>

#include <cstring>

#include "Epub/Kinsoku.h"
#include "Epub/Page.h"

namespace {
// Bare vector growth (reserve()/push_back() past capacity) calls operator new, which aborts the
// whole device on OOM under -fno-exceptions instead of returning nullptr (see CLAUDE.md). Word
// Lookup can be opened right after a heap-starved chapter build (a furigana-dense CJK page can
// leave the heap down to ~15-20KB, confirmed on a real device: an unguarded reserve() here
// aborted with exactly that heap profile). Same doubling-then-linear-fallback guard as
// VerticalParsedText.cpp's pushGlyph()/canPushStreamChar() -- a dropped glyph here just means one
// character isn't selectable for lookup, a far better failure mode than crashing the reader.
constexpr uint32_t SMALL_ALLOC_MARGIN = 16 * 1024;
constexpr size_t LINEAR_GROWTH_STEP = 32;
}  // namespace

// Returns true for any character that could be part of a Japanese word.
// Punctuation, whitespace, and formatting marks are excluded.
// No hiragana skip list — filtering happens at the output stage instead,
// so multi-char words starting with particles (から, ところ) are found.
bool WordSelectionScan::isLookupableChar(uint32_t cp) {
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

bool WordSelectionScan::isKatakana(uint32_t cp) {
  return (cp >= 0x30A0 && cp <= 0x30FF) || cp == 0x30FC || (Kinsoku::isSmallKana(cp) && cp >= 0x30A0);
}

bool WordSelectionScan::isHiragana(uint32_t cp) {
  return (cp >= 0x3040 && cp <= 0x309F) || (Kinsoku::isSmallKana(cp) && cp < 0x30A0);
}

bool WordSelectionScan::isCJK(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

bool WordSelectionScan::isDigitCp(uint32_t cp) { return (cp >= '0' && cp <= '9') || (cp >= 0xFF10 && cp <= 0xFF19); }

size_t WordSelectionScan::katakanaNameRunBeforeHonorific(const std::string& text) {
  // Decode the leading codepoints (a name+honorific never needs more than kMaxLookupChars).
  uint32_t cps[kMaxLookupChars];
  size_t n = 0;
  for (size_t b = 0; b < text.size() && n < kMaxLookupChars;) {
    const auto c = static_cast<unsigned char>(text[b]);
    if (c < 0x80) {
      cps[n++] = c;
      b += 1;
    } else if ((c & 0xE0) == 0xC0 && b + 1 < text.size()) {
      cps[n++] = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[b + 1]) & 0x3F);
      b += 2;
    } else if ((c & 0xF0) == 0xE0 && b + 2 < text.size()) {
      cps[n++] = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[b + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[b + 2]) & 0x3F);
      b += 3;
    } else {
      break;
    }
  }

  // Leading katakana run (>=2 chars). Long-vowel ー is katakana here (ムーミン), 中黒 ・ is not.
  size_t run = 0;
  while (run < n && isKatakana(cps[run]) && cps[run] != 0x30FB) run++;
  if (run < 2) return 0;

  // Honorific immediately after the run.
  const auto at = [&](size_t idx) -> uint32_t { return idx < n ? cps[idx] : 0; };
  const size_t k = run;
  const bool honorific = (at(k) == 0x3055 && (at(k + 1) == 0x3093 || at(k + 1) == 0x307E)) ||  // さん / さま
                         (at(k) == 0x304F && at(k + 1) == 0x3093) ||                           // くん
                         (at(k) == 0x3061 && at(k + 1) == 0x3083 && at(k + 2) == 0x3093) ||    // ちゃん
                         at(k) == 0x69D8 || at(k) == 0x6C0F;                                   // 様 / 氏
  return honorific ? run : 0;
}

bool WordSelectionScan::stripTrailingParticle(const std::string& text, WordLookupResult& result,
                                              const bool needDefinition) {
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

void WordSelectionScan::encodeUtf8(uint32_t cp, std::string& out) {
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

void WordSelectionScan::reserveGlyphsSafe(std::vector<GlyphRef>& vec, size_t count) {
  if (count <= vec.capacity()) return;
  const size_t requestBytes = count * sizeof(GlyphRef);
  if (ESP.getMaxAllocHeap() < requestBytes + SMALL_ALLOC_MARGIN) {
    LOG_ERR("WLKP", "Skipping glyph reserve (%u bytes doesn't fit, free=%u); growing incrementally",
            static_cast<unsigned>(requestBytes), ESP.getMaxAllocHeap());
    return;
  }
  vec.reserve(count);
}

bool WordSelectionScan::pushGlyphSafe(std::vector<GlyphRef>& vec, const GlyphRef& g) {
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

void WordSelectionScan::reset() {
  allGlyphs.clear();
  selectableGlyphs.clear();
  selectToAllIdx.clear();
  phase = Phase::Scan;
  scanPos = 0;
  skipUntil = 0;
  scanTruncated = false;
  restoredCursorIndex = kNoRestoredCursor;
}

void WordSelectionScan::restartStepScan() {
  // Keep allGlyphs -- only the step-scan state is rewound. selectableGlyphs/selectToAllIdx are
  // rebuilt from scratch by the re-walk (they must stay in lockstep, so both clear together).
  selectableGlyphs.clear();
  selectToAllIdx.clear();
  phase = Phase::Scan;
  scanPos = 0;
  skipUntil = 0;
  scanTruncated = false;
  restoredCursorIndex = kNoRestoredCursor;
}

void WordSelectionScan::initFromVerticalPage(const VerticalPage& page) {
  reset();
  reserveGlyphsSafe(allGlyphs, page.glyphs.size());
  for (const auto& g : page.glyphs) {
    if (g.renderKind == VerticalGlyph::RotatedRun) continue;
    GlyphRef ref{g.x, g.y, g.column, g.row, g.codepoint, g.paragraphIndex, false};
    if (!pushGlyphSafe(allGlyphs, ref)) {
      scanTruncated = true;
      break;
    }
  }
  // No upfront reserve for selectableGlyphs: only ~5% of positions become selectable, so
  // reserving allGlyphs.size() entries pinned ~15KB for a dense page across the whole lookup
  // lifetime -- exactly the margin the font decompressor needed while rendering definitions
  // (confirmed crash_report: FDC 16KB temp buffers failing). pushGlyphSafe grows the vector
  // with guarded doubling instead.
}

void WordSelectionScan::initFromPage(const Page& page) {
  reset();
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
               (static_cast<unsigned char>(word[b + 2]) & 0x3F) << 6 | (static_cast<unsigned char>(word[b + 3]) & 0x3F);
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
  scanTruncated = oom;
  // No upfront reserve for selectableGlyphs: only ~5% of positions become selectable, so
  // reserving allGlyphs.size() entries pinned ~15KB for a dense page across the whole lookup
  // lifetime -- exactly the margin the font decompressor needed while rendering definitions
  // (confirmed crash_report: FDC 16KB temp buffers failing). pushGlyphSafe grows the vector
  // with guarded doubling instead.
}

void WordSelectionScan::initFromUtf8Text(const std::string& text) {
  reset();
  size_t b = 0;
  while (b < text.size()) {
    auto c0 = static_cast<unsigned char>(text[b]);
    uint32_t cp;
    if (c0 < 0x80) {
      cp = c0;
      b += 1;
    } else if ((c0 & 0xE0) == 0xC0) {
      cp = (c0 & 0x1F) << 6 | (static_cast<unsigned char>(text[b + 1]) & 0x3F);
      b += 2;
    } else if ((c0 & 0xF0) == 0xE0) {
      cp = (c0 & 0x0F) << 12 | (static_cast<unsigned char>(text[b + 1]) & 0x3F) << 6 |
           (static_cast<unsigned char>(text[b + 2]) & 0x3F);
      b += 3;
    } else {
      cp = (c0 & 0x07) << 18 | (static_cast<unsigned char>(text[b + 1]) & 0x3F) << 12 |
           (static_cast<unsigned char>(text[b + 2]) & 0x3F) << 6 | (static_cast<unsigned char>(text[b + 3]) & 0x3F);
      b += 4;
    }
    if (cp == '\n' || cp == '\r') continue;
    if (!pushGlyphSafe(allGlyphs, GlyphRef{0, 0, 0, 0, cp, 0, false})) {
      scanTruncated = true;
      break;
    }
  }
  // No upfront reserve for selectableGlyphs: only ~5% of positions become selectable, so
  // reserving allGlyphs.size() entries pinned ~15KB for a dense page across the whole lookup
  // lifetime -- exactly the margin the font decompressor needed while rendering definitions
  // (confirmed crash_report: FDC 16KB temp buffers failing). pushGlyphSafe grows the vector
  // with guarded doubling instead.
}

bool WordSelectionScan::step(const uint32_t maxMillis) {
  const uint32_t start = millis();
  while (phase != Phase::Done) {
    if (scanPos >= allGlyphs.size()) {
      phase = Phase::Done;
      break;
    }
    scanOnePosition();
    if (millis() - start >= maxMillis) break;
  }
  return phase == Phase::Done;
}

namespace {
constexpr uint32_t WLSCAN_MAGIC =
    0x43534C57;  // "WLSC" -- POS-validated deinflection (segmentation changes even though the dict
                 // files keep their byte size when reconverted with posFlags, so the size-based
                 // dictFingerprint below cannot catch that swap on its own)

// Cheap fingerprint of the dictionary content: a changed/replaced jmdict.idx invalidates cached
// scans (segmentation depends on the dictionary). File size is not a perfect identity, but any
// realistic dictionary swap changes it.
uint32_t dictFingerprint() {
  HalFile f;
  if (!Storage.openFileForRead("WLS", DictIndex::IDX_PATH, f)) return 0;
  return static_cast<uint32_t>(f.size());
}
}  // namespace

// FNV-1a over the glyph stream's content-identity fields. Any change in the page's text or
// segmentation-relevant structure (paragraph boundaries) produces a different hash, so a cached
// scan can never be applied to a page it wasn't computed from.
uint32_t WordSelectionScan::glyphContentHash() const {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint32_t v) {
    for (int b = 0; b < 4; b++) {
      h ^= (v >> (b * 8)) & 0xFF;
      h *= 16777619u;
    }
  };
  mix(static_cast<uint32_t>(allGlyphs.size()));
  for (const auto& g : allGlyphs) {
    mix(g.codepoint);
    mix(g.paragraphIndex);
  }
  return h;
}

bool WordSelectionScan::tryLoadCache(const std::string& path, const uint16_t spineIndex, const uint16_t pageIndex) {
  HalFile f;
  if (!Storage.openFileForRead("WLS", path, f)) return false;

  struct Header {
    uint32_t magic;
    uint16_t spine;
    uint16_t page;
    uint32_t glyphHash;
    uint32_t dictSize;
    uint16_t count;
    uint16_t lastCursor;
  } __attribute__((packed)) hdr;
  if (f.read(reinterpret_cast<uint8_t*>(&hdr), sizeof(hdr)) != static_cast<int>(sizeof(hdr))) return false;
  if (hdr.magic != WLSCAN_MAGIC || hdr.spine != spineIndex || hdr.page != pageIndex) return false;
  if (hdr.glyphHash != glyphContentHash() || hdr.dictSize != dictFingerprint()) return false;
  if (hdr.count > allGlyphs.size()) return false;

  selectableGlyphs.clear();
  selectToAllIdx.clear();
  reserveGlyphsSafe(selectableGlyphs, hdr.count);
  selectToAllIdx.reserve(hdr.count);
  for (uint16_t i = 0; i < hdr.count; i++) {
    uint32_t idx;
    if (f.read(reinterpret_cast<uint8_t*>(&idx), sizeof(idx)) != static_cast<int>(sizeof(idx)) ||
        idx >= allGlyphs.size()) {
      selectableGlyphs.clear();
      selectToAllIdx.clear();
      return false;
    }
    if (!pushGlyphSafe(selectableGlyphs, allGlyphs[idx])) {
      selectableGlyphs.clear();
      selectToAllIdx.clear();
      return false;
    }
    selectToAllIdx.push_back(idx);
  }
  phase = Phase::Done;
  restoredCursorIndex = hdr.lastCursor < hdr.count ? hdr.lastCursor : kNoRestoredCursor;
  LOG_INF("WLS", "Scan cache hit for spine=%u page=%u (%u selectable, cursor=%u)", spineIndex, pageIndex, hdr.count,
          restoredCursorIndex);
  return true;
}

bool WordSelectionScan::saveCache(const std::string& path, const uint16_t spineIndex, const uint16_t pageIndex,
                                  const uint16_t cursorIndex) const {
  if (!isDone()) return false;
  // Never persist a scan that ran out of heap mid-build: allGlyphs (or selectableGlyphs) was
  // truncated, so it found too few -- often zero -- selectable words. Caching that would make
  // "no matches" stick on this page for every future open, even once the heap recovers (the
  // X3-under-a-huge-CSS-book failure the reporter hit). Drop it; the next open re-scans.
  if (scanTruncated) {
    LOG_INF("WLS", "Scan truncated by low heap (%u selectable); not caching to avoid poisoning",
            static_cast<unsigned>(selectToAllIdx.size()));
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite("WLS", path, f)) return false;

  struct Header {
    uint32_t magic;
    uint16_t spine;
    uint16_t page;
    uint32_t glyphHash;
    uint32_t dictSize;
    uint16_t count;
    uint16_t lastCursor;
  } __attribute__((packed)) hdr;
  hdr.magic = WLSCAN_MAGIC;
  hdr.spine = spineIndex;
  hdr.page = pageIndex;
  hdr.glyphHash = glyphContentHash();
  hdr.dictSize = dictFingerprint();
  hdr.count = static_cast<uint16_t>(selectToAllIdx.size());
  hdr.lastCursor = cursorIndex < hdr.count ? cursorIndex : 0;
  f.write(reinterpret_cast<const uint8_t*>(&hdr), sizeof(hdr));
  for (const size_t idx : selectToAllIdx) {
    const uint32_t v = static_cast<uint32_t>(idx);
    f.write(reinterpret_cast<const uint8_t*>(&v), sizeof(v));
  }
  return true;
}

// Pre-scan step: examine ONE character position, doing dictionary lookups to find word
// boundaries. Characters inside a matched word are skipped (skipUntil). Filtered particles
// (は、が etc.) are still checked — if they start a multi-char match (e.g. という, ことになる),
// they become selectable.
void WordSelectionScan::scanOnePosition() {
  const size_t i = scanPos;
  scanPos++;

  const auto& g = allGlyphs[i];
  if (i < skipUntil) return;

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
    return;
  }

  // No Japanese word begins with a small kana -- sokuon っ, small ゃゅょ, small vowels ぁぃぅぇぉ,
  // ゎ (and their katakana forms). A match starting on one is always a mid-word fragment left by an
  // unhandled contraction (reported: っち out of たまっちゃった). Never start a lookup there; the
  // char is still covered by the preceding word's skipUntil when that word segmented correctly.
  //
  // One real exception: the colloquial quotative って (っていう, ってこと, ...) DOES begin with っ.
  // Allow the lookup for っ+て, but accept only an EXACT dictionary/grammar hit below -- a
  // deinflection-derived hit at a っ start is exactly the fragment class this skip exists to
  // suppress (って→う via the godan te-form rule).
  bool sokuonTeStart = false;
  if (scanStart < allGlyphs.size()) {
    switch (allGlyphs[scanStart].codepoint) {
      case 0x3063: {  // っ
        const bool teNext = scanStart + 1 < allGlyphs.size() && allGlyphs[scanStart + 1].paragraphIndex == paraIdx &&
                            allGlyphs[scanStart + 1].codepoint == 0x3066;  // て
        if (!teNext) return;
        sokuonTeStart = true;
        break;
      }
      case 0x3041:
      case 0x3043:
      case 0x3045:
      case 0x3047:
      case 0x3049:  // ぁぃぅぇぉ
      case 0x3083:
      case 0x3085:
      case 0x3087:
      case 0x308E:  // ゃゅょ ゎ
      case 0x30A1:
      case 0x30A3:
      case 0x30A5:
      case 0x30A7:
      case 0x30A9:  // ァィゥェォ
      case 0x30C3:
      case 0x30E3:
      case 0x30E5:
      case 0x30E7:
      case 0x30EE:  // ッ ャュョ ヮ
        return;
      default:
        break;
    }
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
  // っ-start positions accept exact entries only (see the small-kana skip above): a deinflected
  // hit there is a conjugation fragment (って→う), not the quotative って.
  if (sokuonTeStart && hasMatch && result.deinflected) hasMatch = false;
  if (hasMatch) {
    int matchChars = 0;
    stripTrailingParticle(text, result, needDef);
    size_t pos = 0;
    while (pos < result.matchLength && pos < text.size()) {
      auto c = static_cast<unsigned char>(text[pos]);
      if (c < 0x80)
        pos += 1;
      else if ((c & 0xE0) == 0xC0)
        pos += 2;
      else if ((c & 0xF0) == 0xE0)
        pos += 3;
      else
        pos += 4;
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
          matchAllKana = false;
          break;
        }
      }
      if (matchAllKana) {
        bool hwHasKanji = false;
        for (size_t hb = 0; hb < result.entry.headword.size();) {
          auto hc = static_cast<unsigned char>(result.entry.headword[hb]);
          uint32_t hcp = 0;
          if (hc < 0x80) {
            hcp = hc;
            hb += 1;
          } else if ((hc & 0xE0) == 0xC0) {
            hcp = ((hc & 0x1F) << 6) | (result.entry.headword[hb + 1] & 0x3F);
            hb += 2;
          } else if ((hc & 0xF0) == 0xE0) {
            hcp = ((hc & 0x0F) << 12) | ((result.entry.headword[hb + 1] & 0x3F) << 6) |
                  (result.entry.headword[hb + 2] & 0x3F);
            hb += 3;
          } else {
            hb += 4;
          }
          if (isCJK(hcp)) {
            hwHasKanji = true;
            break;
          }
        }
        const bool usuallyKana = result.entry.definition.find("[kana]") != std::string::npos;
        if (hwHasKanji && !usuallyKana) hasMatch = false;

        // Reading-record collision the headword check above cannot see: the kana READING of a
        // kanji word is its own index record whose headword IS the kana (しながら -> 品柄
        // "quality"), so hwHasKanji stays false and the match survives. Signal instead: an exact
        // all-hiragana jmdict match that is neither marked usually-kana ([kana]) nor common by
        // priority is a reading collision -- no book writes 品柄 as しながら. Thresholds:
        // Jitendex priority = score+128 (128 = no frequency data); jmdict-simplified emits
        // 200 (common) / 100. Grammar/name hits are exempt (pattern entries carry neither
        // [kana] nor frequency), as are deinflected hits (already POS-validated, and their
        // candidate is a dictionary form, not a surface reading).
        constexpr uint8_t kMinKanaExactPriority = 200;
        if (hasMatch && !result.deinflected && matchChars >= 2 && result.entry.sourceDict == DictIndex::DICT_JMDICT &&
            !usuallyKana && result.entry.priority < kMinKanaExactPriority) {
          hasMatch = false;
        }
      }
    }

    // A match that STARTS with a case/topic particle usually mis-segments: the particle should
    // stand alone and the character after it begins the real word. Two examples:
    //   にど (2) -> に + どっさり,   はあり (3, =羽蟻 "winged ant") -> は + ありません(ある).
    // Suppress the particle-led match (pass through so the next char starts the real word) only
    // when the word beginning one char later is AT LEAST AS LONG as this match. That length test
    // is what keeps genuine words that merely begin with a particle-kana intact: とても (3) has
    // remainder ても (2) < 3 so it stays, ところ/における likewise, while 羽蟻's remainder
    // ありません (5) >= 3 correctly loses to the split.
    auto isCaseParticle = [](uint32_t cp) {
      return cp == 0x306B || cp == 0x306E || cp == 0x3068 || cp == 0x304C || cp == 0x306F || cp == 0x3092 ||
             cp == 0x3082 || cp == 0x3067 || cp == 0x3078 || cp == 0x3084 || cp == 0x304B;
    };
    if (hasMatch && matchChars >= 2 && isCaseParticle(allGlyphs[scanStart].codepoint) &&
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
          if (c < 0x80)
            pp += 1;
          else if ((c & 0xE0) == 0xC0)
            pp += 2;
          else if ((c & 0xF0) == 0xE0)
            pp += 3;
          else
            pp += 4;
          nmc++;
        }
        // >= matchChars keeps the original 2-char behavior (nmc>=2) and extends it to longer
        // particle-led mis-segmentations without disturbing real particle-initial words.
        if (nmc >= matchChars) hasMatch = false;  // let the next char start the longer word
      }
    }

    // Fictional katakana name + honorific: group the whole run as one selectable unit even when
    // the dictionary only covers a prefix (ヘム of ヘムレン) or nothing. Dictionary names
    // (スナフキン) already match the whole run, so this is a no-op for them.
    if (isKatakana(allGlyphs[scanStart].codepoint)) {
      const size_t nameRun = katakanaNameRunBeforeHonorific(text);
      if (nameRun >= 2 && static_cast<int>(nameRun) > matchChars) {
        hasMatch = true;
        matchChars = static_cast<int>(nameRun);
      }
    }

    if (hasMatch && (matchChars > 1 || digitGlyphs > 0)) {
      // Extend skip for honorifics after names
      const size_t afterMatch = scanStart + matchChars;
      skipUntil = afterMatch;
      if (afterMatch < allGlyphs.size() && allGlyphs[afterMatch].paragraphIndex == paraIdx) {
        uint32_t nextCp = allGlyphs[afterMatch].codepoint;
        if (nextCp == 0x3055) {  // さ → さん、さま
          if (afterMatch + 1 < allGlyphs.size()) {
            uint32_t nn = allGlyphs[afterMatch + 1].codepoint;
            if (nn == 0x3093) skipUntil = afterMatch + 2;
            if (nn == 0x307E && afterMatch + 2 < allGlyphs.size() &&
                allGlyphs[afterMatch + 2].paragraphIndex == paraIdx)
              skipUntil = afterMatch + 2;
          }
        } else if (nextCp == 0x304F) {  // く → くん
          if (afterMatch + 1 < allGlyphs.size() && allGlyphs[afterMatch + 1].codepoint == 0x3093)
            skipUntil = afterMatch + 2;
        } else if (nextCp == 0x3061) {  // ち → ちゃん
          if (afterMatch + 2 < allGlyphs.size() && allGlyphs[afterMatch + 1].codepoint == 0x3083 &&
              allGlyphs[afterMatch + 2].codepoint == 0x3093)
            skipUntil = afterMatch + 3;
        } else if (nextCp == 0x6C0F || nextCp == 0x69D8) {  // 氏、様
          skipUntil = afterMatch + 1;
        } else if (nextCp == 0x58EB || nextCp == 0x5E2B || nextCp == 0x54E1) {  // 士・師・員
          if (isCJK(allGlyphs[scanStart + matchChars - 1].codepoint)) skipUntil = afterMatch + 1;
        }
      }
    }
  }

  // Include this position if it has a dictionary match AND survives the display filter. The
  // filter runs here (not as a later pass) so partial results shown during a progressive scan
  // never lose entries afterwards. Note the skipUntil bookkeeping above stays in effect for
  // filtered-out matches too, exactly like the previous scan-then-filter split behaved.
  if (hasMatch && passesDisplayFilter(i)) {
    // Push selectableGlyphs first -- if it can't grow, the heap is exhausted, so stop the scan
    // rather than push a mismatched selectToAllIdx entry with no corresponding glyph.
    if (!pushGlyphSafe(selectableGlyphs, g)) {
      scanPos = allGlyphs.size();  // abort the remainder of the scan
      scanTruncated = true;        // partial result -- don't let saveCache() persist it
      return;
    }
    selectToAllIdx.push_back(i);
  }
}

namespace {
// Post-scan display filter: single-char entries that are common particles or conjugation
// fragments. These are valid in the scan (so から, ところ etc. get found correctly) but useless
// as standalone lookup results.
bool isDisplayNoise(uint32_t cp) {
  switch (cp) {
    case 0x306F:  // は
    case 0x304C:  // が
    case 0x306E:  // の
    case 0x306B:  // に
    case 0x3067:  // で
    case 0x3092:  // を
    case 0x3082:  // も
    case 0x3068:  // と
    case 0x304B:  // か
    case 0x306A:  // な
    case 0x3078:  // へ
    case 0x3088:  // よ
    case 0x306D:  // ね
    case 0x308F:  // わ
    case 0x3066:  // て
    case 0x3060:  // だ
    case 0x305F:  // た
    case 0x308B:  // る
    case 0x3093:  // ん
    case 0x3044:  // い
    case 0x304F:  // く
    case 0x3057:  // し
    case 0x3055:  // さ
    case 0x305B:  // せ
    case 0x3089:  // ら
    case 0x304D:  // き
    case 0x3053:  // こ
    case 0x305D:  // そ
    case 0x3042:  // あ
    case 0x304A:  // お (honorific prefix)
    case 0x307E:  // ま
    case 0x3059:  // す
    case 0x308C:  // れ
    case 0x3079:  // べ
    case 0x305E:  // ぞ
    case 0x3081:  // め
    case 0x3076:  // ぶ
    case 0x307F:  // み
    case 0x3064:  // つ
    case 0x306C:  // ぬ
    case 0x3075:  // ふ
    case 0x3080:  // む
      return true;
    default:
      return false;
  }
}
}  // namespace

// Examine a matched position the way performLookup() would display it; false = display noise
// (bare particles, conjugation fragments) that should not become a selectable entry.
bool WordSelectionScan::passesDisplayFilter(const size_t allIdx) const {
  const uint32_t paraIdx = allGlyphs[allIdx].paragraphIndex;

  // Multi-char conjugation suffixes that are never standalone words.
  auto isConjugationNoise = [&]() -> bool {
    std::string matched;
    int mLen = 0;
    for (size_t j = allIdx; j < allGlyphs.size() && mLen < 4; j++) {
      if (allGlyphs[j].paragraphIndex != paraIdx) break;
      encodeUtf8(allGlyphs[j].codepoint, matched);
      mLen++;
    }
    static const char* const patterns[] = {"\xe3\x81\xbe\xe3\x81\x97\xe3\x81\x9f",  // ました
                                           "\xe3\x81\xbe\xe3\x81\x9b\xe3\x82\x93",  // ません
                                           "\xe3\x81\xa7\xe3\x81\x97\xe3\x81\x9f",  // でした
                                           "\xe3\x81\xa7\xe3\x81\x99",              // です
                                           "\xe3\x81\xbe\xe3\x81\x99",              // ます
                                           nullptr};
    for (int p = 0; patterns[p]; p++) {
      size_t plen = strlen(patterns[p]);
      if (matched.size() >= plen && matched.substr(0, plen) == patterns[p]) return true;
    }
    return false;
  };

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
      if (c < 0x80)
        p += 1;
      else if ((c & 0xE0) == 0xC0)
        p += 2;
      else if ((c & 0xF0) == 0xE0)
        p += 3;
      else
        p += 4;
      mc++;
    }
    // Filter: a SHORT match whose every char is an individually-noise kana (は, が, には, では)
    // is a stray particle / particle-combo, not a lookup-worthy word. Cap this at <=2 chars: the
    // noise list is a set of single kana, and many real 3-4 char words are built entirely from
    // them -- mimetics like ふんふん/きらきら and words like とても/ところ. Filtering those by the
    // per-char list dropped them from the page (reported: ふんふん skipped). A genuine 3+ char
    // dictionary match is kept.
    bool allNoise = mc <= 2;
    for (size_t ci = allIdx; allNoise && ci < allIdx + static_cast<size_t>(mc) && ci < allGlyphs.size(); ci++) {
      if (!isDisplayNoise(allGlyphs[ci].codepoint)) {
        allNoise = false;
        break;
      }
    }
    if (allNoise) return false;

    // Exact-match colloquial/grammatical contractions that are never useful as
    // standalone vocabulary (なくちゃ→ちゃ, じゃ, etc.). Use the exact matched
    // text so real words (茶碗/ちゃわん) are not filtered.
    // そうに/そうな/そうだ are the ～そう "seeming/appears" auxiliary (心配そうに); their only
    // dictionary hit is the rare homophone 僧尼 ("monks and nuns") and no grammar entry covers
    // them, so as standalone lookups they are just misleading -- filter them out.
    const std::string matchedText = ltext.substr(0, lr.matchLength);
    static const char* const exactNoise[] = {"\xe3\x81\xa1\xe3\x82\x83",              // ちゃ
                                             "\xe3\x81\x98\xe3\x82\x83",              // じゃ
                                             "\xe3\x81\xa1\xe3\x82\x83\xe3\x81\x86",  // ちゃう
                                             "\xe3\x81\xa3\xe3\x81\xa6",              // って
                                             "\xe3\x81\x9d\xe3\x81\x86\xe3\x81\xab",  // そうに
                                             "\xe3\x81\x9d\xe3\x81\x86\xe3\x81\xaa",  // そうな
                                             "\xe3\x81\x9d\xe3\x81\x86\xe3\x81\xa0",  // そうだ
                                             nullptr};
    for (int e = 0; exactNoise[e]; e++) {
      if (matchedText == exactNoise[e]) return false;
    }
  }
  if (isConjugationNoise()) {
    return false;
  }
  return true;
}
