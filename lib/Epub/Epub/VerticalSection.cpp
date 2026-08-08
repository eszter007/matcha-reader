#include "VerticalSection.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <XmlParserUtils.h>
#include <expat.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "Epub/RubyGlossary.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "GfxRenderer.h"

namespace {

// v51: bumped again past every v50 cache -- confirmed on a real device that v50's retry logic ran
// correctly (logged "removing partial cache file" for several broken images) but several STILL
// failed extraction on that same rebuild pass: the zip inflate's 32KB contiguous block was
// competing with the font decompressor's hot-group buffer, still resident from this same
// chapter's column-fitting text measurements. Now frees that buffer immediately before each image
// extraction attempt. A chapter rebuilt under v50 can have images that failed even with the retry
// logic and are now stuck cached as "extraction already tried, still missing" until a fresh
// rebuild gives the fix (more headroom, not just a retry) a chance to actually help. See cache
// format comment below.
// v52: not a format change -- forces a rebuild of vertical caches that were built while the CSS
// rule table was still held resident (see Epub::load): its heap fragmentation made the layout's
// stream reserve fail on long chapters, silently truncating them into sparse pages ON DISK.
// v78: PR #12 (rotated-run loop fix) also fixed encodeCp for supplementary-plane codepoints
// (4-byte UTF-8; previously emitted invalid 3-byte sequences) WITHOUT a bump -- caches built
// before it can hold the broken bytes and must not be reused.
// v80: not a format change -- invalidates best-effort caches built while the resident write
// staging buffer deepened the low-heap dips and dropped glyphs (missing characters ON DISK);
// the transient per-page staging buffer now prevents those drops on rebuild. (The
// early-first-render / mid-build page-turn work layered on top changes no on-disk format, so
// it needs no further bump -- a v80 cache built by this firmware is byte-identical.)
// v99: merge of upstream 1.5.0. Not a format change -- an invalidation. Upstream reworked the
// measurement stack the vertical layout is built on (SD kern classes + ligatures, the advance-table
// fast path, fallback-resolved text font ids), so a v98 cache's stored glyph positions no longer
// match the cell verticalCellPx() re-derives at DRAW time from the new metrics --
// the exact "cell flapping" mismatch that path exists to prevent. The horizontal pipeline
// invalidates for the same reason (SECTION_FILE_VERSION 51); this one must not be forgotten.
// v100: image pages carry their source href.
// v101: image pages built while extraction was failing stored the VIEWPORT box as the image size
// (the displayW/displayH fallback when getDimensions fails). config.useExactDimensions stretches
// the decode to exactly that box, so those pages rendered visibly distorted once the image finally
// became available. Extraction is reliable now (dee87656), so a rebuild records true dimensions.
// v102: aside/figure/figcaption joined isBlockTag (parity with the horizontal parser's
// SECTION_FILE_VERSION 68 bump), so kakomi boxes and figure blocks on those tags now break
// paragraphs and pick up styled-block params. A v101 cache flowed them as inline text.
// v103: no format change -- bumped so every vertical cache re-paginates once with the
// partial-reserve fix, replacing pages that older builds split at random fill levels
// under X3 heap pressure (no manual .crosspoint deletion needed).
// v104: glyph records are fixed-size (textId) with a per-page text pool appended after
// the glyph array; ruby/run strings moved out of VerticalGlyph (~3x smaller page buffers).
// v105: the header includes the vertical column-spacing setting.
// v131: bordered blocks render at all, and their geometry changed with them. No format change.
// CssPropertyFlags::anySet() omitted `border`, so every border-only rule (the EBPAJ `.k-*` set)
// was dropped at parse time and no kakomi box ever recorded a rect. Alongside that: rects are
// inset 0.25em from the text, boxed columns end short of the foot to leave room for the bottom
// rule, and a bordered block is separated from both neighbours by a blank column -- so
// pagination inside and around a box changes too. (127-130 were consumed by intermediate box
// geometries during development; caches carrying those numbers must not be trusted.)
// v134: every page record begins with the source position of its first character
// (VerticalPage::visibleTextOffset), counted the same way the horizontal parser counts it. It is
// what lets a vertical/horizontal switch resolve the reading position by content instead of by
// proportion. Format change: a v131 record starts with the image flag. (132-133 were consumed
// during development by builds that computed those positions wrongly; such records parse cleanly,
// so caches carrying those numbers must not be trusted.)
constexpr uint8_t VSECTION_FILE_VERSION = 134;
// 4KB, not 1KB: chapter builds are SD-latency-bound -- the inflate staging write, the
// staging read-back, and the expat feed each touch the card once per chunk, so quadrupling
// the chunk quarters the transaction count for ~12KB of transient buffers.
constexpr size_t PARSE_BUFFER_SIZE = 4096;

using RubyRun = VerticalParsedText::RubyRun;

// Receives extraction output in document order, one paragraph or image at a time. The extractor
// deliberately has no whole-chapter storage: a real Japanese chapter's text plus its laid-out
// glyph pages runs to megabytes, which can never fit in the ESP32-C3's ~220KB heap (the previous
// accumulate-everything design only worked in the desktop emulator's 8MB heap). See
// VerticalSection.h for the full memory model.
struct ParagraphSink {
  virtual ~ParagraphSink() = default;
  // Takes ownership of the runs; the extractor's buffer is cleared after the call.
  // Consumes the runs' contents (moves the strings out) and clear()s the vector, so the
  // caller's buffer keeps its capacity across paragraphs. continuesPrevious=true means these
  // runs seamlessly continue the previously delivered paragraph (streaming cadence) -- no
  // paragraph break must be recorded before them.
  virtual void onParagraph(std::vector<RubyRun>& runs, bool continuesPrevious) = 0;
  // visibleTextOffset: the source position the image occupies, so its page can be stamped
  // like a text page and the page-start sequence stays non-decreasing for the binary search
  // in getPageForVisibleTextOffset(). An image contributes no visible characters of its own.
  virtual void onImage(const std::string& src, uint32_t visibleTextOffset) = 0;
  // Styled-block boundaries -- a block element whose CSS carries vertical layout parameters
  // (border box, start offset, hanging indent, centering, before/after gaps).
  virtual void onBlockStyleStart(const VerticalBlockParams& /*params*/) {}
  virtual void onBlockStyleEnd() {}
};

struct TextExtractor {
  // Each paragraph is a sequence of RubyRun entries. Unannotated text has
  // empty ruby; annotated text (<ruby>base<rt>reading</rt></ruby>) maps
  // base -> rubyText.
  ParagraphSink* sink = nullptr;

  std::vector<RubyRun> currentRuns;
  std::string currentText;
  int blockDepth = 0;
  int skipDepth = -1;

  // Pathological books put an entire chapter in one <p>/<div>. currentText/currentRuns are the
  // only unbounded-by-markup buffers left in extraction, so force a paragraph split once the
  // accumulated text passes this size -- the split hands the text to the sink (which lays out and
  // flushes pages to SD), keeping extraction O(bounded-paragraph) instead of O(chapter). A forced
  // split starts a new column mid-paragraph; harmless compared to the alternative (OOM).
  static constexpr size_t MAX_PARAGRAPH_BYTES = 16 * 1024;

  // Styled-block detection: (selector -> vertical layout params) distilled from the CSS cache.
  // boxOpenedAtDepth is the elementDepth of the styled element while inside one, else -1.
  const std::vector<std::pair<std::string, CssParser::VerticalBlockStyle>>* blockStyles = nullptr;
  int boxOpenedAtDepth = -1;

  // Ruby parsing state
  bool inRuby = false;
  bool inRt = false;
  bool inRp = false;
  std::string rubyBase;
  std::string rubyAnnotation;

  // Position tracking, so a reading position survives a vertical/horizontal switch.
  //
  // visibleTextOffset counts codepoints of visible character data in document order, under the
  // SAME rule the horizontal parser applies (ChapterHtmlSlimParser::characterData): inside
  // <body>, outside head/style/script/title/rp, entity expansions excluded. Both layouts must
  // agree character for character -- the number is only useful because it means the same thing
  // on both sides -- so any change here needs the horizontal counter changed with it.
  //
  // <rt> text IS counted (rp is the excluded one), matching isNonVisibleElement().
  uint32_t visibleTextOffset = 0;
  bool insideBody = false;
  // Offset of the first character of currentText / rubyBase, captured when each goes from
  // empty to non-empty. That is what a RubyRun is stamped with.
  uint32_t currentTextOffset = 0;
  uint32_t rubyBaseOffset = 0;

  // Stamp where a run begins. Synthetic text (entity expansion, <br/>, a gaiji replacement)
  // occupies no counted source position -- the horizontal parser does not count it either --
  // so a run it opens is attributed to the position the next real character will take.
  void beginTextRunIfEmpty() {
    if (currentText.empty()) currentTextOffset = visibleTextOffset;
  }
  void beginRubyBaseIfEmpty() {
    if (rubyBase.empty()) rubyBaseOffset = visibleTextOffset;
  }

  // Furigana-glossary harvest (owned by the layout sink; see RubyGlossary). Mono-ruby
  // elements (小<rt>こ</rt>林<rt>ばやし</rt>) additionally record the whole-element pair
  // (小林 -> こばやし) so word lookup's whole-word selection can match.
  std::vector<RubyGlossary::Pair>* rubyHarvest = nullptr;
  std::string rubyElemBase;
  std::string rubyElemRuby;
  int rubyElemRunCount = 0;

  // Style tracking — each entry records the elementDepth at which
  // bold/italic was activated. On endElement, if we're leaving that
  // depth, pop and flush.
  int boldDepth = 0;
  int italicDepth = 0;
  int elementDepth = 0;
  static constexpr int MAX_STYLE_STACK = 8;
  int boldOpenedAtDepth[MAX_STYLE_STACK] = {};
  int boldStackSize = 0;
  int italicOpenedAtDepth[MAX_STYLE_STACK] = {};
  int italicStackSize = 0;
  int emphasisDepth = 0;
  int emphasisOpenedAtDepth[MAX_STYLE_STACK] = {};
  int emphasisStackSize = 0;

  bool hasEmphasis() const { return emphasisDepth > 0; }

  // Diagnostic: bisects a ~11KB drop seen accumulating within a single furigana-dense paragraph's
  // SAX processing, too large to be explained by RubyRun/string vector growth alone. Call at the
  // top of each SAX callback -- logs only on a drop since the last checkpoint, so a normal chapter
  // stays quiet and only the actual culprit tag/callback prints. Remove once found.
  uint32_t lastCheckpointMaxAlloc = 0;
  void checkHeap(const char* phase, const char* tag = "") {
    const uint32_t now = ESP.getMaxAllocHeap();
    if (lastCheckpointMaxAlloc != 0 && now + 256 < lastCheckpointMaxAlloc) {
      LOG_DBG("VSC", "heap drop at %s(%s): %u -> %u", phase, tag, lastCheckpointMaxAlloc, now);
    }
    lastCheckpointMaxAlloc = now;
  }

  static bool isBoldTag(const char* name) { return strcasecmp(name, "b") == 0 || strcasecmp(name, "strong") == 0; }
  static bool isItalicTag(const char* name) { return strcasecmp(name, "i") == 0 || strcasecmp(name, "em") == 0; }
  uint8_t currentStyle() const {
    uint8_t s = 0;
    if (boldDepth > 0) s |= 1;    // EpdFontFamily::BOLD
    if (italicDepth > 0) s |= 2;  // EpdFontFamily::ITALIC
    return s;
  }

  static bool isSkipTag(const char* name) {
    return strcasecmp(name, "head") == 0 || strcasecmp(name, "style") == 0 || strcasecmp(name, "script") == 0;
  }

  // std::move()-ing currentText/currentRuns into the sink hands off their heap buffer and leaves
  // the local variable at capacity 0 -- so without a reserve() right after, the NEXT run/paragraph
  // has to regrow from scratch via std::string/vector's own doubling, one malloc+free cycle at a
  // time. For furigana-dense text (ruby annotations on nearly every kanji, e.g. "kyokasho"-style
  // textbook readings) that's dozens to hundreds of tiny alloc/free cycles per paragraph -- exactly
  // the kind of churn that fragments a ~220KB heap. These hints are small requests sized for the
  // common case (a handful of CJK characters / a handful of runs between markup boundaries), not a
  // correctness requirement -- an unusually long run still grows normally via doubling.
  static constexpr size_t TEXT_RESERVE_HINT = 128;  // bytes
  static constexpr size_t RUBY_RESERVE_HINT = 32;   // bytes
  static constexpr size_t RUNS_RESERVE_HINT = 16;   // elements

  void flushCurrentText() {
    if (!currentText.empty()) {
      // COPY, don't move: moving handed currentText's grown buffer to a transient RubyRun and
      // restarted this one at TEXT_RESERVE_HINT, so every paragraph re-grew it by doubling
      // (alloc-copy-free per step, hundreds of times per chapter) -- the main planter of the
      // persistent fragments that shredded maxAlloc on long single-file books. The copy is a
      // transient that coalesces back; currentText's capacity now lives for the whole build.
      currentRuns.push_back(RubyRun{currentText, {}, currentStyle(), hasEmphasis(), currentTextOffset});
      currentText.clear();
    }
  }

  // Streaming accumulation bounds: hand runs to the sink every ~SOFT_FLUSH_BYTES (or
  // SOFT_FLUSH_RUNS for furigana-dense text) as a seamless paragraph CONTINUATION, instead of
  // buffering whole paragraphs SAX-side. A 238KB single-file novel accumulated 16KB text +
  // its RubyRun copies here per forced-split "paragraph" -- transient peaks and buffer growth
  // that shredded the heap's largest block. With a ~2KB cadence every buffer on this layer
  // stays small and stable for the whole build.
  static constexpr size_t SOFT_FLUSH_BYTES = 2048;
  static constexpr size_t SOFT_FLUSH_RUNS = 48;
  bool midParagraph = false;

  static bool isAsciiWordByte(const unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  }

  // Whether `s` ends mid-token, i.e. inside something the layout must see whole. Byte-level testing is
  // not enough: Japanese books write years in FULLWIDTH digits (１９３８), and U+FF10-U+FF19 encode as
  // EF BC 90..99, whose tail byte is not an ASCII word byte. Cutting there hands the layout two
  // 2-digit batches and each is set as its own tate-chu-yoko pair -- 19 stacked over 38 rather than one
  // rotated run.
  static bool endsMidToken(const std::string& s) {
    if (s.empty()) return false;
    const auto last = static_cast<unsigned char>(s.back());
    if (isAsciiWordByte(last)) return true;
    return s.size() >= 3 && static_cast<unsigned char>(s[s.size() - 3]) == 0xEF &&
           static_cast<unsigned char>(s[s.size() - 2]) == 0xBC && last >= 0x90 && last <= 0x99;
  }

  // paragraphEnds=false streams a partial paragraph: the sink lays it out with no break
  // recorded, and the next emit continues it seamlessly (continuesPrevious=true).
  void emitRuns(const bool paragraphEnds) {
    // A soft flush is a cadence, not a deadline. If the text so far ends inside a Latin word or a number,
    // skip this one and let the next take it: the vertical layout gathers a rotated run only within one
    // batch, so cutting here renders "authority" as "au", a blank cell, then "thority". Deferring is
    // bounded -- the paragraph's own end always flushes -- and the triggers retry on the next character
    // or run.
    //
    // The tail is in currentText, or, when the run-COUNT trigger fires from the </ruby> handler, in the
    // last run already pushed.
    if (!paragraphEnds) {
      const std::string& tail =
          !currentText.empty() ? currentText : (currentRuns.empty() ? currentText : currentRuns.back().baseText);
      if (endsMidToken(tail)) return;
    }
    flushCurrentText();
    if (!currentRuns.empty()) {
      if (sink) {
        // Diagnostic: bisects where a chapter's heap actually drops -- was the paragraph itself
        // already large/run-heavy going INTO onParagraph (accumulation phase, this SAX callback
        // layer), or does the drop happen INSIDE onParagraph (stream-building/layout phase,
        // VerticalParsedText)? Remove once the sparse-page root cause is found.
        size_t totalBytes = 0;
        for (const auto& r : currentRuns) totalBytes += r.baseText.size() + r.rubyText.size();
        LOG_DBG("VSC", "flushParagraph: %u runs, %u bytes, maxAlloc=%u before onParagraph",
                static_cast<unsigned>(currentRuns.size()), static_cast<unsigned>(totalBytes), ESP.getMaxAllocHeap());
        // By reference: onParagraph moves the individual runs' strings out and clear()s the
        // vector, handing its capacity back -- currentRuns keeps one stable buffer for the
        // whole build instead of re-growing from RUNS_RESERVE_HINT every paragraph.
        sink->onParagraph(currentRuns, midParagraph);
        LOG_DBG("VSC", "flushParagraph: maxAlloc=%u after onParagraph", ESP.getMaxAllocHeap());
      }
      currentRuns.clear();
      midParagraph = !paragraphEnds;
    } else if (paragraphEnds) {
      midParagraph = false;
    }
  }

  void flushParagraph() { emitRuns(true); }

  static bool isBlockTag(const char* name) {
    static constexpr const char* blockTags[] = {"p",       "div",     "h1",    "h2",     "h3",
                                                "h4",      "h5",      "h6",    "li",     "blockquote",
                                                "section", "article", "aside", "figure", "figcaption"};
    for (const auto* tag : blockTags) {
      if (strcasecmp(name, tag) == 0) return true;
    }
    return false;
  }

  // Merge every matching styled-block entry for this element into params. Supports the selector
  // forms the CSS map stores: "tag", ".class", "tag.class" (case-insensitive, class-attr token
  // match). Returns true if anything matched.
  bool resolveBlockStyle(const char* name, const char** atts, VerticalBlockParams& params) const {
    if (!blockStyles || blockStyles->empty()) return false;
    bool matched = false;
    for (const auto& [sel, vs] : *blockStyles) {
      if (sel.empty()) continue;
      bool hit = false;
      if (sel[0] == '.') {
        hit = hasClass(atts, sel.c_str() + 1);
      } else {
        const size_t dot = sel.find('.');
        if (dot == std::string::npos) {
          hit = strcasecmp(sel.c_str(), name) == 0;
        } else {
          hit =
              strlen(name) == dot && strncasecmp(sel.c_str(), name, dot) == 0 && hasClass(atts, sel.c_str() + dot + 1);
        }
      }
      if (!hit) continue;
      matched = true;
      if (vs.startEm > 0) params.startEm = vs.startEm;
      if (vs.beforeEm > 0) params.beforeEm = vs.beforeEm;
      if (vs.afterEm > 0) params.afterEm = vs.afterEm;
      if (vs.hangEm > 0) params.hangEm = vs.hangEm;
      params.alignCenter = params.alignCenter || vs.alignCenter;
      if (vs.borderEdges != 0) params.borderEdges = vs.borderEdges;
    }
    return matched;
  }

  static bool hasClass(const char** atts, const char* cls) {
    if (!atts) return false;
    for (int i = 0; atts[i]; i += 2) {
      if (strcasecmp(atts[i], "class") == 0 && atts[i + 1]) {
        const char* val = atts[i + 1];
        const size_t clsLen = strlen(cls);
        while (*val) {
          while (*val == ' ') val++;
          if (strncasecmp(val, cls, clsLen) == 0 && (val[clsLen] == ' ' || val[clsLen] == '\0')) return true;
          while (*val && *val != ' ') val++;
        }
      }
    }
    return false;
  }

  static void XMLCALL startElement(void* userData, const char* name, const char** atts) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->checkHeap("startElement", name);
    self->elementDepth++;
    if (self->skipDepth >= 0) {
      self->skipDepth++;
      return;
    }
    if (strcasecmp(name, "body") == 0) self->insideBody = true;
    if (isSkipTag(name)) {
      self->skipDepth = 1;
      return;
    }
    if (self->boxOpenedAtDepth < 0) {
      VerticalBlockParams params;
      if (self->resolveBlockStyle(name, atts, params) &&
          (params.startEm > 0 || params.beforeEm > 0 || params.afterEm > 0 || params.hangEm > 0 || params.alignCenter ||
           params.borderEdges != 0)) {
        self->flushParagraph();
        LOG_DBG("VSC", "styled block open: <%s> start=%.1f hang=%.1f before=%.1f after=%.1f center=%d edges=0x%X", name,
                params.startEm, params.hangEm, params.beforeEm, params.afterEm, params.alignCenter, params.borderEdges);
        if (self->sink) self->sink->onBlockStyleStart(params);
        self->boxOpenedAtDepth = self->elementDepth;
      }
    }
    if (isBlockTag(name)) {
      if (self->blockDepth == 0) {
        self->flushParagraph();
      }
      self->blockDepth++;
    }
    if (strcasecmp(name, "ruby") == 0) {
      self->flushCurrentText();
      self->inRuby = true;
      self->rubyBase.clear();
      self->rubyBase.reserve(RUBY_RESERVE_HINT);
      self->rubyAnnotation.clear();
      self->rubyAnnotation.reserve(RUBY_RESERVE_HINT);
      self->rubyElemBase.clear();
      self->rubyElemRuby.clear();
      self->rubyElemRunCount = 0;
    } else if (strcasecmp(name, "rt") == 0) {
      self->inRt = true;
      self->rubyAnnotation.clear();
      self->rubyAnnotation.reserve(RUBY_RESERVE_HINT);
    } else if (strcasecmp(name, "rp") == 0) {
      self->inRp = true;
    }
    if (isBoldTag(name) || hasClass(atts, "bold")) {
      self->flushCurrentText();
      self->boldDepth++;
      if (self->boldStackSize < MAX_STYLE_STACK) self->boldOpenedAtDepth[self->boldStackSize++] = self->elementDepth;
    }
    if (isItalicTag(name) || hasClass(atts, "italic")) {
      self->flushCurrentText();
      self->italicDepth++;
      if (self->italicStackSize < MAX_STYLE_STACK)
        self->italicOpenedAtDepth[self->italicStackSize++] = self->elementDepth;
    }
    if (hasClass(atts, "em-sesame") || hasClass(atts, "em-dot") || hasClass(atts, "em-circle") ||
        hasClass(atts, "em-sesame-open") || hasClass(atts, "em-dot-open") || hasClass(atts, "em-circle-open") ||
        hasClass(atts, "em-triangle") || hasClass(atts, "em-double-circle")) {
      self->flushCurrentText();
      self->emphasisDepth++;
      if (self->emphasisStackSize < MAX_STYLE_STACK)
        self->emphasisOpenedAtDepth[self->emphasisStackSize++] = self->elementDepth;
    }
    if (strcasecmp(name, "img") == 0 || strcasecmp(name, "image") == 0) {
      const char* src = nullptr;
      const char* alt = nullptr;
      if (atts) {
        for (int i = 0; atts[i]; i += 2) {
          if (strcasecmp(atts[i], "src") == 0 || strcasecmp(atts[i], "xlink:href") == 0) {
            if (!src) src = atts[i + 1];
          } else if (strcasecmp(atts[i], "alt") == 0) {
            alt = atts[i + 1];
          }
        }
      }
      if (hasClass(atts, "gaiji")) {
        // Gaiji: a tiny inline image standing in for a rare glyph mid-sentence.
        // Routing it through onImage would split the paragraph and insert a
        // near-empty full image page per gaiji; keep the text flowing with
        // replacement text instead (alt / filename codepoint / geta mark).
        self->beginTextRunIfEmpty();
        self->currentText += gaijiReplacementText(src ? src : "", alt ? alt : "");
      } else if (src && src[0] != '\0') {
        // Complete the paragraph built so far, then emit the image in document order. (For the
        // rare mid-paragraph image this places the partial text before the image, where the old
        // accumulate-then-interleave code placed the image before the whole paragraph; identical
        // for the usual block-level images.)
        self->flushParagraph();
        if (self->sink) self->sink->onImage(std::string(src), self->visibleTextOffset);
      }
    }
    if (strcasecmp(name, "br") == 0 || strcasecmp(name, "br/") == 0) {
      if (!self->inRuby) {
        self->beginTextRunIfEmpty();
        self->currentText.push_back('\n');
      }
    }
  }

  static void XMLCALL endElement(void* userData, const char* name) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->checkHeap("endElement", name);
    self->elementDepth--;
    if (self->skipDepth > 0) {
      self->skipDepth--;
      if (self->skipDepth == 0) self->skipDepth = -1;
      return;
    }
    if (self->boxOpenedAtDepth >= 0 && self->elementDepth == self->boxOpenedAtDepth - 1) {
      self->flushParagraph();
      if (self->sink) self->sink->onBlockStyleEnd();
      self->boxOpenedAtDepth = -1;
    }
    if (strcasecmp(name, "rp") == 0) {
      self->inRp = false;
      return;
    }
    if (strcasecmp(name, "rt") == 0) {
      self->inRt = false;
      // Emit a RubyRun for the base text accumulated so far with this annotation.
      if (!self->rubyBase.empty()) {
        if (self->rubyHarvest) {
          // Glossary harvest BEFORE the moves below consume the strings.
          RubyGlossary::collect(*self->rubyHarvest, self->rubyBase, self->rubyAnnotation);
          self->rubyElemBase += self->rubyBase;
          self->rubyElemRuby += self->rubyAnnotation;
          self->rubyElemRunCount++;
        }
        self->currentRuns.push_back(RubyRun{std::move(self->rubyBase), std::move(self->rubyAnnotation),
                                            self->currentStyle(), self->hasEmphasis(), self->rubyBaseOffset});
        self->rubyBase.clear();
        self->rubyBase.reserve(RUBY_RESERVE_HINT);
      }
      self->rubyAnnotation.clear();
      self->rubyAnnotation.reserve(RUBY_RESERVE_HINT);
      return;
    }
    if (strcasecmp(name, "ruby") == 0) {
      // Flush any remaining base text that had no <rt> (malformed markup).
      if (!self->rubyBase.empty()) {
        self->currentRuns.push_back(
            RubyRun{std::move(self->rubyBase), {}, self->currentStyle(), self->hasEmphasis(), self->rubyBaseOffset});
        self->rubyBase.clear();
        self->rubyBase.reserve(RUBY_RESERVE_HINT);
      }
      // Mono-ruby element (per-character <rt>s): also record the whole-element pair so
      // whole-word lookups (小林) match, not just per-character ones (小, 林).
      if (self->rubyHarvest && self->rubyElemRunCount >= 2) {
        RubyGlossary::collect(*self->rubyHarvest, self->rubyElemBase, self->rubyElemRuby);
      }
      self->rubyElemBase.clear();
      self->rubyElemRuby.clear();
      self->rubyElemRunCount = 0;
      self->inRuby = false;
      // Furigana-dense text accumulates many small runs without currentText ever growing;
      // stream them onward at the same cadence as the byte bound (see SOFT_FLUSH_RUNS).
      if (self->currentRuns.size() >= SOFT_FLUSH_RUNS) self->emitRuns(false);
      return;
    }
    if (self->boldStackSize > 0 && self->boldOpenedAtDepth[self->boldStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->boldDepth--;
      self->boldStackSize--;
    }
    if (self->italicStackSize > 0 && self->italicOpenedAtDepth[self->italicStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->italicDepth--;
      self->italicStackSize--;
    }
    if (self->emphasisStackSize > 0 && self->emphasisOpenedAtDepth[self->emphasisStackSize - 1] == self->elementDepth) {
      self->flushCurrentText();
      self->emphasisDepth--;
      self->emphasisStackSize--;
    }
    if (isBlockTag(name)) {
      self->blockDepth--;
      if (self->blockDepth <= 0) {
        self->blockDepth = 0;
        self->flushParagraph();
      }
    }
  }

  static void XMLCALL characterData(void* userData, const char* s, int len) {
    auto* self = static_cast<TextExtractor*>(userData);
    self->checkHeap("characterData");
    // Position bookkeeping first, and independent of every layout-side early return below:
    // the count must track the DOCUMENT, not what this layout chooses to render. Dropped
    // inter-tag whitespace and text past a forced split still occupy source positions, and
    // the horizontal parser counts them.
    const uint32_t offsetOfThisRun = self->visibleTextOffset;
    if (self->insideBody && self->skipDepth < 0 && !self->inRp) {
      const auto* p = reinterpret_cast<const unsigned char*>(s);
      const auto* end = p + len;
      while (p < end) {
        // Advance one UTF-8 sequence: a lead byte plus its continuation bytes.
        p++;
        while (p < end && (*p & 0xC0) == 0x80) p++;
        self->visibleTextOffset++;
      }
    }
    if (self->skipDepth >= 0) return;
    if (self->inRp) return;
    if (self->inRt) {
      self->rubyAnnotation.append(s, static_cast<size_t>(len));
    } else if (self->inRuby) {
      if (self->rubyBase.empty()) self->rubyBaseOffset = offsetOfThisRun;
      self->rubyBase.append(s, static_cast<size_t>(len));
    } else {
      // Forced split for markup-less mega-paragraphs; see MAX_PARAGRAPH_BYTES. (Not applied
      // inside <ruby> -- ruby runs are a handful of characters by nature.)
      if (self->currentText.size() + static_cast<size_t>(len) > MAX_PARAGRAPH_BYTES) {
        LOG_DBG("VSC", "MAX_PARAGRAPH_BYTES forced split at %u bytes", static_cast<unsigned>(self->currentText.size()));
        self->flushParagraph();
      }
      // Drop inter-tag whitespace (the "\n" text nodes between <p>/<div> in pretty-printed
      // xhtml): laid out as real paragraphs they produced phantom blank pages -- e.g. an
      // empty page BEFORE a cover chapter's image page. Only leading whitespace is dropped;
      // intentional blank lines arrive as explicit '\n' from <br/> in startElement, and
      // whitespace inside running text lands with currentText already non-empty.
      if (self->currentText.empty()) {
        int firstInk = 0;
        while (firstInk < len &&
               (s[firstInk] == '\n' || s[firstInk] == '\r' || s[firstInk] == '\t' || s[firstInk] == ' ')) {
          firstInk++;
        }
        if (firstInk == len) return;
        s += firstInk;
        len -= firstInk;
        // The dropped whitespace was counted above, so the surviving text starts that many
        // codepoints later. All of it is ASCII, so bytes and codepoints agree here.
        self->currentTextOffset = offsetOfThisRun + static_cast<uint32_t>(firstInk);
      }
      self->currentText.append(s, static_cast<size_t>(len));
      // Streaming cadence: hand the buffered text onward as a seamless continuation well
      // before it grows large (see SOFT_FLUSH_BYTES).
      if (self->currentText.size() >= SOFT_FLUSH_BYTES) self->emitRuns(false);
    }
  }

  static void XMLCALL defaultHandler(void* userData, const char* s, int len) {
    if (len >= 4 && s[0] == '&') {
      auto* self = static_cast<TextExtractor*>(userData);
      std::string entity(s, static_cast<size_t>(len));
      std::string resolved;
      if (entity == "&nbsp;") {
        resolved = " ";
      } else if (entity == "&mdash;") {
        resolved = "\xe2\x80\x94";
      } else if (entity == "&ndash;") {
        resolved = "\xe2\x80\x93";
      } else if (entity == "&hellip;") {
        resolved = "\xe2\x80\xa6";
      } else if (entity == "&amp;") {
        resolved = "&";
      } else if (entity == "&lt;") {
        resolved = "<";
      } else if (entity == "&gt;") {
        resolved = ">";
      } else if (entity == "&quot;") {
        resolved = "\"";
      } else if (entity == "&apos;") {
        resolved = "'";
      } else {
        return;
      }

      if (self->inRp) return;
      if (self->inRt) {
        self->rubyAnnotation.append(resolved);
      } else if (self->inRuby) {
        self->beginRubyBaseIfEmpty();
        self->rubyBase.append(resolved);
      } else {
        self->beginTextRunIfEmpty();
        self->currentText.append(resolved);
      }
    }
  }
};

}  // namespace

namespace {

// ---- Page (de)serialization (cache format v37) -----------------------------------------------
// File layout:
//   header: u8 version, i32 fontId, u16 viewportWidth, u16 viewportHeight, u8 lineSpacing,
//           u16 pageCount, u32 indexOffset          (pageCount/indexOffset patched post-stream)
//   page records (variable length, written as pages are laid out)
//   footer at indexOffset: pageCount x u32 file offset of each page record
// The footer lets loadSectionFile() open a chapter by reading only the header + 4 bytes/page,
// and getPage() seek straight to one page -- pages are never all resident in RAM.

// The glyph and box field lists below are visited by BOTH the writer and the reader, so their order
// is defined exactly once. Two hand-maintained lists desync every record in the file on any
// divergence, and the bytes still parse, so the version stamp gives no warning -- pages simply come
// back scrambled. G is const on the write side, mutable on the read side.
struct PodReadArchive {
  HalFile& file;
  template <typename T>
  void operator()(T& v) {
    serialization::readPod(file, v);
  }
};
template <typename Sink>
struct PodWriteArchive {
  Sink& file;
  template <typename T>
  void operator()(const T& v) {
    serialization::writePod(file, v);
  }
};

// G is const on the write side, mutable on the read side.
template <typename Ar, typename G>
void visitGlyphFields(Ar& ar, G& g) {
  ar(g.codepoint);
  ar(g.column);
  ar(g.row);
  ar(g.x);
  ar(g.y);
  ar(g.paragraphIndex);
  ar(g.byteOffset);
  ar(g.renderKind);
  ar(g.style);
  ar(g.emphasis);
  ar(g.lineHeadFlush);
  ar(g.textId);
}

template <typename Ar, typename R>
void visitBoxFields(Ar& ar, R& r) {
  ar(r.x);
  ar(r.y);
  ar(r.w);
  ar(r.h);
  ar(r.edges);
}

// Sink is HalFile (direct, the historical path) or serialization::BufWriter
// (whole page into RAM, flushed with one file.write) -- identical byte layout.
template <typename Sink>
bool writePage(Sink& file, const VerticalPage& page) {
  // First field, before the image/text split, so every page record carries it. Lets the reader
  // learn the current page's source position from the page it already loaded, with no extra
  // seek per turn (the reverse direction uses the index table -- see readPageStartOffsets).
  serialization::writePod(file, page.visibleTextOffset);
  const bool isImg = page.isImagePage();
  serialization::writePod(file, isImg);
  if (isImg) {
    serialization::writeString(file, page.imagePath);
    serialization::writeString(file, page.imageSrcPath);
    serialization::writePod(file, page.imageWidth);
    serialization::writePod(file, page.imageHeight);
    serialization::writePod(file, page.imageRotated);
    return true;
  }
  const auto glyphCount = static_cast<uint32_t>(page.glyphs.size());
  serialization::writePod(file, glyphCount);
  serialization::writePod(file, page.columnCount);
  serialization::writePod(file, page.rowsPerColumn);

  // Boxed-block border rects (v63+). Bounded small (a page holds at most a handful of boxes).
  const auto boxCount = static_cast<uint8_t>(std::min<size_t>(page.boxes.size(), 8));
  serialization::writePod(file, boxCount);
  PodWriteArchive<Sink> ar{file};
  for (uint8_t bi = 0; bi < boxCount; bi++) visitBoxFields(ar, page.boxes[bi]);

  for (const auto& g : page.glyphs) visitGlyphFields(ar, g);

  // Per-page text pool (v104): ruby annotations and run strings, referenced by textId.
  const auto textCount = static_cast<uint16_t>(page.texts.size());
  serialization::writePod(file, textCount);
  for (const auto& t : page.texts) {
    // Clamp instead of wrapping: a >64KB entry is impossible today (runs are bounded by the
    // 16KB paragraph cap), but a wrapped uint16_t would silently desync the record.
    const auto len = static_cast<uint16_t>(std::min<size_t>(t.size(), 0xFFFF));
    serialization::writePod(file, len);
    if (len > 0) {
      file.write(reinterpret_cast<const uint8_t*>(t.data()), len);
    }
  }
  return true;
}

// A read can fail two ways that callers must treat DIFFERENTLY: HeapRefused is transient (the
// glyph vector could not be reserved on the current low heap) and the on-disk record is fine,
// so the caller must NOT invalidate the cache -- retry later. Corrupt means the bytes are bad
// and a rebuild is warranted. Collapsing both to a bool let a momentary heap dip nuke a valid
// cache and trigger an expensive rebuild loop.
enum class ReadResult { Ok, HeapRefused, Corrupt };

ReadResult readPage(HalFile& file, VerticalPage& page) {
  serialization::readPod(file, page.visibleTextOffset);
  page.glyphs.clear();
  page.boxes.clear();
  page.texts.clear();
  page.imagePath.clear();
  page.imageSrcPath.clear();

  bool isImg = false;
  serialization::readPod(file, isImg);
  if (isImg) {
    serialization::readString(file, page.imagePath);
    serialization::readString(file, page.imageSrcPath);
    serialization::readPod(file, page.imageWidth);
    serialization::readPod(file, page.imageHeight);
    serialization::readPod(file, page.imageRotated);
    return page.imagePath.empty() ? ReadResult::Corrupt : ReadResult::Ok;
  }

  uint32_t glyphCount;
  serialization::readPod(file, glyphCount);
  serialization::readPod(file, page.columnCount);
  serialization::readPod(file, page.rowsPerColumn);

  uint8_t boxCount = 0;
  serialization::readPod(file, boxCount);
  if (boxCount > 8) {
    LOG_ERR("VSC", "Corrupt page record: %u boxes", boxCount);
    return ReadResult::Corrupt;
  }
  page.boxes.reserve(boxCount);
  PodReadArchive ar{file};
  for (uint8_t bi = 0; bi < boxCount; bi++) {
    VerticalBoxRect r;
    visitBoxFields(ar, r);
    page.boxes.push_back(r);
  }

  // One page is bounded by screen geometry (a few hundred cells); a corrupt count must not
  // drive a huge reserve on a heap that can't take it.
  constexpr uint32_t MAX_GLYPHS_PER_PAGE = 4096;
  if (glyphCount > MAX_GLYPHS_PER_PAGE) {
    LOG_ERR("VSC", "Corrupt page record: %u glyphs", glyphCount);
    return ReadResult::Corrupt;
  }
  // reserve() throws-and-ABORTS under -fno-exceptions; verify the block actually exists
  // first. A corrupt count within the format bound can still demand ~98KB -- observed on
  // device as an abort inside this exact reserve during a mid-build read-back. The margin
  // is deliberately small: the page is transient (rendered, then freed), and an over-strict
  // margin starved mid-build page-turn serves for entire dense stretches.
  const uint32_t glyphBytes = glyphCount * static_cast<uint32_t>(sizeof(VerticalGlyph));
  // Gate only when the vector must actually GROW: a caller passing a pre-reserved page (the
  // build's serve slot) reads with zero allocation and must not be refused. 2K margin, not
  // more: the render draws from a REFERENCE and the page is freed/reused right after; a 4K
  // margin permanently starved mid-build page turns against a stable 13.3K hole.
  if (page.glyphs.capacity() < glyphCount && ESP.getMaxAllocHeap() < glyphBytes + 2 * 1024) {
    LOG_ERR("VSC", "Page record needs %u bytes, maxAlloc=%u; refusing read", glyphBytes, ESP.getMaxAllocHeap());
    return ReadResult::HeapRefused;
  }
  page.glyphs.reserve(glyphCount);

  for (uint32_t gi = 0; gi < glyphCount; gi++) {
    VerticalGlyph g;
    visitGlyphFields(ar, g);
    page.glyphs.push_back(g);
  }

  // Per-page text pool (v104). Out-of-range textIds render as "no text" (glyphText bounds-
  // checks), so a short pool degrades visibly but safely.
  uint16_t textCount = 0;
  serialization::readPod(file, textCount);
  if (textCount > MAX_GLYPHS_PER_PAGE) {
    LOG_ERR("VSC", "Corrupt page record: %u pool texts", textCount);
    return ReadResult::Corrupt;
  }
  // Same discipline as the glyph reserve above: reserve()/resize() abort under
  // -fno-exceptions, and a valid on-disk record read on a momentarily tight heap must come
  // back as HeapRefused (retry later), never a crash and never a cache invalidation.
  const uint32_t textVecBytes = static_cast<uint32_t>(textCount) * sizeof(std::string);
  if (page.texts.capacity() < textCount && ESP.getMaxAllocHeap() < textVecBytes + 2 * 1024) {
    LOG_ERR("VSC", "Text pool needs %u bytes, maxAlloc=%u; refusing read", textVecBytes, ESP.getMaxAllocHeap());
    return ReadResult::HeapRefused;
  }
  page.texts.reserve(textCount);
  for (uint16_t ti = 0; ti < textCount; ti++) {
    uint16_t len = 0;
    serialization::readPod(file, len);
    std::string t;
    if (len > 0) {
      if (ESP.getMaxAllocHeap() < static_cast<uint32_t>(len) + 2 * 1024) {
        LOG_ERR("VSC", "Pool text %u needs %u bytes, maxAlloc=%u; refusing read", ti, len, ESP.getMaxAllocHeap());
        return ReadResult::HeapRefused;
      }
      t.resize(len);
      if (file.read(reinterpret_cast<uint8_t*>(t.data()), len) != static_cast<int>(len)) {
        LOG_ERR("VSC", "Corrupt page record: truncated pool text %u", ti);
        return ReadResult::Corrupt;
      }
    }
    page.texts.push_back(std::move(t));
  }
  return ReadResult::Ok;
}

// Streams the temp HTML once looking for "<rt", to decide ruby layout geometry (column gap /
// right padding) before the first paragraph is laid out. The old design scanned the fully
// accumulated paragraph list for ruby runs; a streaming pipeline has to know up front. A false
// positive (e.g. "<rt" inside a comment) merely pads columns slightly -- harmless.
bool fileContainsRubyTag(const std::string& path) {
  HalFile f;
  if (!Storage.openFileForRead("VSC", path, f)) return false;
  uint8_t buf[512];
  int state = 0;  // matched prefix length of "<rt"
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    for (size_t i = 0; i < n; i++) {
      const char c = static_cast<char>(buf[i]);
      if (state == 0) {
        state = (c == '<') ? 1 : 0;
      } else if (state == 1) {
        state = (c == 'r') ? 2 : (c == '<' ? 1 : 0);
      } else {
        if (c == 't') {
          f.close();
          return true;
        }
        state = (c == '<') ? 1 : 0;
      }
    }
  }
  f.close();
  return false;
}

// Concrete sink: feeds each extracted paragraph straight into the column layout, and whenever a
// batch of characters is buffered (or an image forces a boundary), lays out the batch and writes
// each resulting page to the cache file immediately. Nothing here is O(chapter): the layout
// stream, the produced pages, and the paragraph being extracted are all O(batch).
struct LayoutPageSink final : ParagraphSink {
  VerticalParsedText& layout;
  HalFile& out;
  std::vector<uint32_t>& pageOffsets;
  Epub& epub;
  GfxRenderer& renderer;
  const std::string& chapterDir;
  const std::string& imageBasePath;
  const uint16_t viewportWidth;
  const uint16_t viewportHeight;
  size_t imgIdx = 0;
  bool failed = false;
  // Per-book furigana glossary harvest: unique (base, ruby) pairs seen during this build,
  // merged into <cache>/ruby.bin after a successful parse (see RubyGlossary). Bounded by
  // RubyGlossary's per-section cap, so worst case is a few KB of short strings.
  std::vector<RubyGlossary::Pair> rubyHarvest;

  void onBlockStyleStart(const VerticalBlockParams& params) override { layout.markBlockStart(params); }
  void onBlockStyleEnd() override { layout.markBlockEnd(); }

  // ~1-2 screens of text per layout batch. A batch boundary lands between paragraphs, which
  // already force a fresh column, so the only observable effect is an occasional page that ends
  // at a paragraph boundary instead of mid-paragraph -- same behavior as an image boundary.
  //
  // Was 640. Confirmed on a real device that at 640, the stream_ buffer needed to hold one batch
  // (33-43KB for furigana-dense text, each RubyRun carrying its own PendingChar-per-character
  // cost) and layoutPages()'s own per-page glyph buffers (13824+ bytes each) are BOTH alive at
  // flush time -- stream_ isn't freed until after layoutPages() returns and reset() runs. Peak
  // memory is the SUM of both, and for dense chapters that sum exceeded available heap, dropping
  // glyphs on the hardest paragraphs even after the chunking fix bounded stream_'s own reserve.
  // Halving the batch roughly halves stream_'s peak size, leaving proportionally more headroom
  // for layoutPages() at the cost of more (individually smaller, safer) flush cycles. Was 640,
  // then 320 -- still measurably dropped glyphs on the single most furigana-dense paragraph in a
  // real test chapter (71 runs), on top of the chunk-sizing estimate bug fixed alongside this
  // (see onParagraph()). Halved again; the trend across both prior reductions has been strictly
  // more real content preserved with each halving.
  static constexpr size_t BATCH_CHARS = 160;

  // Early-first-render hook, forwarded from VerticalSection::setEarlyRenderHook(). The
  // initial target page and mid-build page turns share ONE mechanism: pageRequest holds the
  // newest wanted page (seeded with the initial target at build start), and writeOne serves
  // it whenever the heap allows.
  void (*earlyRenderFn)(void*, const VerticalPage&, int) = nullptr;
  void* earlyRenderCtx = nullptr;
  std::atomic<int>* pageRequest = nullptr;
  uint64_t lastRefusedAttemptKey = 0;  // see servePageRequest()
  int fontReleasedForReq_ = -1;        // one font-cache release per starved request; see servePageRequest()

  // Reusable read-back slot for mid-build backward turns. It grows through readPage's guarded
  // on-demand reserve; preallocating a second full page here starved the actual layout page.
  VerticalPage servePage_;

  LayoutPageSink(VerticalParsedText& layout, HalFile& out, std::vector<uint32_t>& pageOffsets, Epub& epub,
                 GfxRenderer& renderer, const std::string& chapterDir, const std::string& imageBasePath,
                 uint16_t viewportWidth, uint16_t viewportHeight)
      : layout(layout),
        out(out),
        pageOffsets(pageOffsets),
        epub(epub),
        renderer(renderer),
        chapterDir(chapterDir),
        imageBasePath(imageBasePath),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight) {}

  void onParagraph(std::vector<RubyRun>& runs, const bool continuesPrevious) override {
    if (failed) return;
    // (Furigana-glossary harvest happens in TextExtractor's ruby handlers, which also see
    // whole-<ruby>-element boundaries for mono-ruby pairs; rubyHarvest is only OWNED here.)
    // A single paragraph (one <p>/<div>, or many furigana-annotated RubyRuns) can itself hold
    // thousands of characters -- MAX_PARAGRAPH_BYTES (16KB, ~5000+ CJK chars) only bounds the
    // SAX-side accumulation buffers, not this batch's memory budget, and ordinary long-form prose
    // routinely exceeds BATCH_CHARS on its own without ever hitting that limit. Feeding the whole
    // paragraph to addAnnotatedParagraph() in one call let a single paragraph's stream_ growth
    // blow past the intended batch size entirely -- confirmed on a real device: a 71-run/12KB
    // paragraph needed a single ~160KB stream_ reserve, far more than the device's entire heap,
    // well before pendingCount() ever got a chance to trigger a flush. Chunking runs here gives
    // pendingCount() >= BATCH_CHARS a chance to fire (and flush) partway through a large paragraph
    // instead of only after the whole thing is already buffered. Splitting mid-paragraph this way
    // is the same accepted tradeoff as the existing MAX_PARAGRAPH_BYTES forced split (a stray
    // column break where none existed in the source -- harmless compared to the alternative, OOM).
    std::vector<RubyRun> chunk;
    size_t chunkEstimatedChars = 0;
    // Only the paragraph's FIRST chunk starts a new paragraph in the layout engine; later
    // chunks continue it (no break recorded), so a flush between chunks no longer decides
    // whether a stray column break appears -- see addAnnotatedParagraph's doc comment.
    // continuesPrevious: this whole call is itself a continuation (streaming cadence from
    // the extractor), so even its first chunk records no break.
    bool firstChunkOfParagraph = !continuesPrevious;
    auto utf8Chars = [](const std::string& s) {
      size_t n = 0;
      for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) n++;
      }
      return n;
    };
    auto pushRun = [&](RubyRun&& run) {
      // Only baseText contributes actual stream_ entries -- rubyText is folded into each base
      // character's PendingChar.rubyText field. Counting is EXACT (UTF-8 lead bytes): the old
      // bytes/3 estimate undercounted ASCII runs 3x, letting a chunk overshoot the batch cap
      // and stream_'s preallocated capacity (see preallocateStream()).
      const size_t runEstimatedChars = utf8Chars(run.baseText);
      chunk.push_back(std::move(run));
      chunkEstimatedChars += runEstimatedChars;
      if (layout.pendingCount() + chunkEstimatedChars >= BATCH_CHARS) {
        layout.addAnnotatedParagraph(chunk, !firstChunkOfParagraph);
        firstChunkOfParagraph = false;
        chunk.clear();
        chunkEstimatedChars = 0;
        if (layout.pendingCount() >= BATCH_CHARS) flushText();
      }
    };
    // Chunking can only split BETWEEN runs, but sparse-furigana prose delivers multi-KB
    // plain-text runs (one run spans everything between two ruby anchors -- or the whole
    // forced-split paragraph when a book has no ruby at all). One such run fed to
    // addAnnotatedParagraph() as a unit needs its whole PendingChar expansion (~12x the UTF-8
    // bytes) in stream_ at once: a 16KB paragraph demanded a 218KB stream on a real device and
    // strangled the heap until an unrelated small allocation aborted. Slice ruby-less runs at
    // UTF-8 boundaries so the BATCH_CHARS flush works as designed; ruby runs stay whole (their
    // base is a single annotated word -- slicing would detach the reading).
    // Sliced by CHARACTER count, not bytes: a byte cap lets a pure-ASCII slice carry 3x the
    // characters of a CJK one, overshooting the batch cadence and the stream_ preallocation.
    constexpr size_t RUN_SLICE_CHARS = 170;  // same order as BATCH_CHARS
    for (auto& run : runs) {
      if (failed) return;
      if (run.rubyText.empty() && utf8Chars(run.baseText) > RUN_SLICE_CHARS) {
        const std::string base = std::move(run.baseText);
        size_t pos = 0;
        // Codepoints of this run already emitted as earlier slices. addAnnotatedParagraph()
        // counts positions from the start of the run it is handed, so each slice must carry its
        // own start or they all claim the original run's.
        uint32_t cpConsumed = 0;
        while (pos < base.size() && !failed) {
          size_t end = pos;
          size_t chars = 0;
          while (end < base.size()) {
            if ((static_cast<unsigned char>(base[end]) & 0xC0) != 0x80) {
              if (chars == RUN_SLICE_CHARS) break;
              chars++;
            }
            end++;
          }
          RubyRun slice;
          slice.baseText = base.substr(pos, end - pos);
          slice.style = run.style;
          slice.emphasis = run.emphasis;
          slice.visibleTextOffset = run.visibleTextOffset + cpConsumed;
          cpConsumed += static_cast<uint32_t>(chars);
          pushRun(std::move(slice));
          pos = end;
        }
        continue;
      }
      pushRun(std::move(run));
    }
    if (failed) return;
    if (!chunk.empty()) {
      layout.addAnnotatedParagraph(chunk, !firstChunkOfParagraph);
    }
    runs.clear();  // free this paragraph's text now -- layout owns its own copy in the stream
    if (layout.pendingCount() >= BATCH_CHARS) flushText();
  }

  void onImage(const std::string& src, const uint32_t visibleTextOffset) override {
    if (failed) return;
    // Lay out any buffered text, then FINALIZE the in-progress page before the image page is
    // written. Without this, the image page was written while the half-filled text page stayed
    // pending: the pending page (whose content PRECEDES the image) landed in the cache AFTER
    // the image page, and post-image text silently merged onto it -- confirmed on a real device
    // as dialogue continuing mid-column across a scene-break graphic instead of starting fresh.
    flushText();
    VerticalPage pendingTail;
    if (layout.finalizePendingPage(pendingTail)) writeOne(pendingTail);
    VerticalPage imagePage = makeImagePage(src);
    imagePage.visibleTextOffset = visibleTextOffset;
    writeOne(imagePage);
  }

  static void writePageCallback(void* ctx, VerticalPage&& page) { static_cast<LayoutPageSink*>(ctx)->writeOne(page); }

  // isFinalFlush must be true ONLY for the chapter's true last flush (see the caller in
  // streamParseAndLayout(), after extractor.flushParagraph()) -- every mid-chapter call (the
  // BATCH_CHARS trigger below, onImage()'s pre-image flush) must pass false so a batch boundary
  // that lands mid-page continues that page on the next call instead of finalizing it early. See
  // VerticalParsedText::layoutPages()'s isFinalFlush doc comment for the full rationale.
  void flushText(bool isFinalFlush = false) {
    if (failed) return;
    if (!isFinalFlush && layout.pendingCount() == 0) return;
    // Streaming pages out via callback as they're finalized keeps at most ~2 pages' worth of
    // glyph buffers resident at once instead of the whole batch's -- see PageReadyCallback in
    // VerticalParsedText.h for why this is safe (oikomi only ever looks one page back).
    auto pages = layout.layoutPages(this, &writePageCallback, isFinalFlush);
    layout.reset();
    for (const auto& p : pages) writeOne(p);
  }

  // Page serialization staging capacity: one file.write per page instead of ~10 mutexed
  // SD writes per GLYPH (measured: ~8.3s of a 23s 431-page build was writes). 12KB covers
  // a dense ruby page (~200 glyphs x ~30 bytes plus strings); larger pages fall back to
  // the direct field-wise path.
  static constexpr size_t kPageBufCap = 12 * 1024;

  void writeOne(const VerticalPage& p) {
    pageOffsets.push_back(static_cast<uint32_t>(out.position()));
    // TRANSIENT staging buffer: allocated for this one write, freed before layout resumes.
    // Holding it resident across the whole build deepened the layout's low-heap dips by
    // roughly its own size (device evidence: maxAlloc bottoms 14324 without it vs
    // 9204-10740 with it resident) and made the layout drop glyphs -- content loss for a
    // speed buffer. As a per-page transient the layout never coexists with it, and the
    // same-size alloc/free per page reuses the same hole instead of fragmenting. If the
    // allocation fails at a tight moment, this page just takes the direct field-wise path.
    auto pageBuf = makeUniqueNoThrow<uint8_t[]>(kPageBufCap);
    bool ok = false;
    if (pageBuf) {
      serialization::BufWriter w(pageBuf.get(), kPageBufCap);
      if (writePage(w, p) && !w.overflow) {
        ok = out.write(pageBuf.get(), w.len) == w.len;
      } else if (w.overflow) {
        ok = writePage(out, p);  // page larger than the staging buffer: direct path
      }
    } else {
      ok = writePage(out, p);
    }
    if (!ok) {
      LOG_ERR("VSC", "Failed to write page %zu to cache", pageOffsets.size() - 1);
      failed = true;
    }

    if (ok) servePageRequest(p, static_cast<int>(pageOffsets.size()) - 1);
  }

  // Show-the-page-during-the-build engine, shared by the initial early first render (the
  // request is seeded with the reader's target page at build start) and mid-build page
  // turns: whenever a page finishes writing, serve the newest request -- directly if it IS
  // the page just written, else by reading the already-serialized record back from the
  // cache file. openFileForWrite opens O_RDWR, so the same handle seeks back for the read
  // and returns to the end so the build appends exactly where it left off. A request beyond
  // the built range stays pending and is re-checked after every page until the build reaches
  // it; a request for an image page is dropped (its multi-second decode cannot run
  // mid-build -- the page appears normally once the build completes).
  void servePageRequest(const VerticalPage& justWritten, const int lastBuilt) {
    if (!pageRequest || !earlyRenderFn) return;
    const int req = pageRequest->load(std::memory_order_relaxed);
    if (req < 0 || req > lastBuilt) return;
    // Serving renders a page mid-build, and parts of that path allocate bare -- under
    // -fno-exceptions an OOM there ABORTS (observed on device: __terminate during a
    // mid-build page turn). The deep guards are in place (readPage refuses reads its glyph
    // reserve can't afford; renderVerticalPageBody skips the prewarm when tight), so this
    // outer gate only needs to cover the render's own working set. readPage() applies its own
    // allocation gate; 8K here covers the remaining render transients. A skipped serve stays
    // pending and retries after the next page.
    constexpr uint32_t SERVE_MIN_ALLOC = 8 * 1024;
    if (ESP.getMaxAllocHeap() < SERVE_MIN_ALLOC) {
      // The pending request is the page the reader is looking AT (initial early render or a
      // mid-build turn). Fonts reload lazily, so trade the font caches for the serve -- once
      // per request -- rather than leave it starved: a request whose direct-serve moment is
      // missed can only be satisfied by read-back, which needs MORE contiguous heap, so one
      // missed gate check starved the early render for an entire 32s build (observed: the
      // first page appeared when the build finished instead of ~2-3s in). Same trade
      // makeImagePage() below already makes mid-build.
      if (req != fontReleasedForReq_) {
        fontReleasedForReq_ = req;
        if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
      }
      if (ESP.getMaxAllocHeap() < SERVE_MIN_ALLOC) return;
    }
    if (req == lastBuilt) {
      pageRequest->store(-1, std::memory_order_relaxed);
      if (!justWritten.isImagePage()) earlyRenderFn(earlyRenderCtx, justWritten, req);
      return;
    }
    // A refused read-back under an UNCHANGED heap state will be refused again -- don't
    // repeat the seek+read+log for every subsequent page (observed: one starved backward
    // turn produced a ~50Hz refusal stream for the rest of the build). Retry only when the
    // request changed or the largest free block moved by a 4KB step: keying on the exact
    // maxAlloc defeated the dedup, because build-time maxAlloc jitters between a handful of
    // nearby values every page (measured: 8.7-12.3KB), and each "retry" is a flush+seek+
    // header-read against the SD card mid-build -- one held backward turn hammered the card
    // for the whole build and stretched it from 18.5s to 36.4s while re-degrading pagination.
    const uint64_t attemptKey = (static_cast<uint64_t>(req) << 32) | (ESP.getMaxAllocHeap() >> 12);
    if (attemptKey == lastRefusedAttemptKey) return;
    const size_t endPos = out.position();
    // Commit pending writes before reading earlier records through the same handle: without
    // the flush, a backward seek can hand back stale/garbage bytes for freshly written pages
    // (observed on device: a read-back returned a corrupt glyph count).
    out.flush();
    // A failed seek to the record would leave the handle at the append position, so readPage
    // would parse the just-written page's tail as a header -- garbage. Bail the serve (the
    // request stays pending) rather than render a corrupt page; the write cursor is still at
    // endPos, so the build continues correctly.
    if (!out.seek(pageOffsets[req])) {
      LOG_ERR("VSC", "Failed to seek to page %d for read-back", req);
      out.seek(endPos);
      return;
    }
    const ReadResult okRead = readPage(out, servePage_);
    if (!out.seek(endPos)) {
      LOG_ERR("VSC", "Failed to seek back to build position after page read-back");
      failed = true;
      return;
    }
    if (okRead != ReadResult::Ok) {  // heap-refused or corrupt: request stays pending, retried on heap change
      if (okRead == ReadResult::HeapRefused && req != fontReleasedForReq_) {
        // First heap refusal for this request: free the font caches and leave the dedup key
        // unset so the very next page write retries against the roomier heap.
        fontReleasedForReq_ = req;
        if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseAllFontMemory();
        return;
      }
      lastRefusedAttemptKey = attemptKey;
      return;
    }
    pageRequest->store(-1, std::memory_order_relaxed);
    if (!servePage_.isImagePage()) earlyRenderFn(earlyRenderCtx, servePage_, req);
  }

  VerticalPage makeImagePage(const std::string& src) {
    std::string resolvedSrc = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(chapterDir + src));

    // Determine extension and cached path
    std::string ext;
    const size_t extPos = resolvedSrc.rfind('.');
    if (extPos != std::string::npos) ext = resolvedSrc.substr(extPos);
    const std::string cachedPath = imageBasePath + std::to_string(imgIdx++) + ext;

    // Extract image from EPUB to cache if not already present. A previous attempt that failed
    // partway (e.g. the zip inflate's 32KB ring buffer allocation failing under the same heap
    // pressure this session spent all night fixing elsewhere) can leave behind a file that
    // EXISTS but is empty/truncated -- Storage.exists() alone can't tell a genuinely-cached image
    // from that broken leftover, so a broken file was never retried, permanently blanking that
    // page. Checking size > 0 makes this self-healing for images broken by an earlier session.
    bool needsExtraction = true;
    if (Storage.exists(cachedPath.c_str())) {
      HalFile existingFile;
      if (Storage.openFileForRead("VSC", cachedPath, existingFile) && existingFile.size() > 0) {
        needsExtraction = false;
      }
    }
    if (needsExtraction) {
      // Extraction needs one contiguous 32KB block for the zip inflate window (InflateReader::
      // init(true)) -- confirmed on a real device that this fails on chapters with both many
      // images and dense text, where the font decompressor's hot-group buffer (regrown during
      // this same chapter's column-fitting measurements) is still resident and competing for that
      // headroom. Free it right before the allocation that actually needs it.
      if (auto* fcm = renderer.getFontCacheManager()) {
        fcm->releaseAllFontMemory();
      }
      HalFile cachedFile;
      if (Storage.openFileForWrite("VSC", cachedPath, cachedFile)) {
        bool extracted = false;
        {
          // ...and the reason freeing was not enough: the heap is FRAGMENTED, not full. Measured
          // at the moment of failure: 89992 bytes free but a largest block of 30708, against the
          // 32768 the window needs. Releasing more bytes cannot help when they come back in
          // pieces -- font reclaim moved maxAlloc by 0, and so did suspending the section build.
          //
          // InflateStream::init() already has the answer: it claims the lent framebuffer via
          // buildscratch (state ~11KB + window 32KB fit in 48KB) instead of the heap. That is why
          // the blocking full build's extractions succeed -- it holds a loan for its whole
          // duration -- while this path, which never took one, fails on the same book.
          //
          // Scoped to the extraction call alone: nothing may draw or display while the bytes are
          // lent, and the early-render hook only fires at page boundaries, never inside this call.
          // Restore hands back a white framebuffer, which the next page render repaints anyway;
          // the panel keeps showing its last refreshed image throughout (e-ink is persistent).
          const bool canLendFrameBuffer = renderer.hasFrameBuffer();
          GfxRenderer::FrameBufferLoan loan(renderer);
          // Prefer 16KB chunks when the framebuffer loan is available or the heap is already
          // roomy; SD write throughput is per-chunk-latency bound. 4KB remains the fallback.
          const bool useFastChunks = canLendFrameBuffer || ESP.getMaxAllocHeap() >= 96 * 1024;
          const size_t chunkSize = useFastChunks ? 16384 : 4096;
          extracted = epub.readItemContentsToStream(resolvedSrc, cachedFile, chunkSize);
        }
        cachedFile.flush();
        cachedFile.close();
        if (!extracted) {
          LOG_ERR("VSC", "Failed to extract image %s; removing partial cache file", resolvedSrc.c_str());
          Storage.remove(cachedPath.c_str());
        }
      }
    }

    // Get actual image dimensions. Store natural (unrotated) dimensions --
    // ImageBlock::render handles rotation, scaling, and centering itself.
    int displayW = viewportWidth;
    int displayH = viewportHeight;
    bool rotated = false;
    ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedPath);
    if (decoder) {
      ImageDimensions dims = {0, 0};
      if (decoder->getDimensions(cachedPath, dims) && dims.width > 0 && dims.height > 0) {
        const bool viewportIsPortrait = (viewportHeight > viewportWidth);
        const bool imageIsLandscape = (dims.width > dims.height);
        rotated = (viewportIsPortrait == imageIsLandscape);
        displayW = dims.width;
        displayH = dims.height;
      }
    }

    VerticalPage page;
    page.imagePath = cachedPath;
    // Recorded even when the extraction above succeeded: a cache can record that an artifact was
    // produced, never that it still exists. Costs one short string on image pages only.
    page.imageSrcPath = resolvedSrc;
    page.imageWidth = static_cast<int16_t>(displayW);
    page.imageHeight = static_cast<int16_t>(displayH);
    page.imageRotated = rotated;
    return page;
  }
};

// Byte offset of the pageCount field: everything createSectionFile writes ahead of it. Keep in step
// with that write order. This is a SEEK target, so a stale value silently overwrites the preceding
// field instead of failing -- adding the furigana flag without updating it wrote pageCount over the
// flag, and a chapter whose page count happened to equal the flag passed the parameter check and
// then read its page table from a shifted offset ("empty chapter").
constexpr size_t HEADER_PAGECOUNT_OFFSET = sizeof(uint8_t)     // version
                                           + sizeof(int)       // fontId
                                           + sizeof(uint16_t)  // viewportWidth
                                           + sizeof(uint16_t)  // viewportHeight
                                           + sizeof(uint8_t)   // lineSpacing
                                           + sizeof(uint8_t);  // furiganaFlag

}  // namespace

bool VerticalSection::streamParseAndLayout(HalFile& out, const int fontId, const uint16_t viewportWidth,
                                           const uint16_t viewportHeight, const uint8_t lineSpacing,
                                           const bool furiganaEnabled) {
  lastBuildDroppedForHeap_ = false;
  // Diagnostic: the "sparse page" investigation found maxAlloc already down at the very first
  // paragraph flush, staying flat for the rest of the chapter -- logging both metrics here checks
  // whether that low contiguous budget is a fresh drop from THIS chapter's own parsing, or whether
  // the heap was already this fragmented (from earlier chapters/navigation this session) before
  // this chapter's build even started. free=getFreeHeap() (total) was already logged; maxAlloc=
  // getMaxAllocHeap() (largest contiguous block) is new.
  const uint32_t buildStartMs = millis();
  LOG_INF("VSC", "streamParseAndLayout start spine=%d free=%u maxAlloc=%u", spineIndex, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  // Vertical placement measures each glyph's real ink extents (burasage, half-em pairing, the 3.8
  // squeeze deficits), and those measurements go through the font decompressor. A heap too tight for a
  // glyph group makes them fall back to nominal metrics silently, and the result is written to the
  // section cache where nothing re-examines it. Sample the starved count across the build and treat any
  // increase as heap degradation, which the stale stamp below turns into a rebuild on next open.
  uint32_t starvedGlyphsAtStart = 0;
  if (auto* fcm = renderer.getFontCacheManager()) {
    if (auto* fd = fcm->getDecompressor()) starvedGlyphsAtStart = fd->getStarvedGlyphCount();
  }
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_v" + std::to_string(spineIndex) + ".html";

  bool success = false;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      delay(50);
    }
    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
    HalFile tmpHtml;
    if (!Storage.openFileForWrite("VSC", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    // The chapter HTML is fully on SD before parsing or early rendering starts, so temporarily
    // lend the framebuffer to InflateStream. This keeps the fast 16KB path available when the
    // loan is held or the heap is roomy; the popup remains on the e-ink panel during the loan.
    {
      const bool canLendFrameBuffer = renderer.hasFrameBuffer();
      GfxRenderer::FrameBufferLoan loan(renderer);
      const bool useFastChunks = canLendFrameBuffer || ESP.getMaxAllocHeap() >= 96 * 1024;
      const size_t chunkSize = useFastChunks ? 16384 : PARSE_BUFFER_SIZE;
      success = epub->readItemContentsToStream(localPath, tmpHtml, chunkSize);
    }
    tmpHtml.close();
    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  }

  if (!success) {
    LOG_ERR("VSC", "Failed to stream chapter HTML");
    return false;
  }
  // Diagnostic: bisecting a ~10KB drop seen between chapter start and the first paragraph flush --
  // isolates whether it's the HTML-to-tempfile copy (zip inflate window), the ruby-tag scan, or
  // XML_ParserCreate's own setup.
  LOG_DBG("VSC", "after readItemContentsToStream: maxAlloc=%u", ESP.getMaxAllocHeap());

  // Resolve image paths relative to the chapter's directory in the EPUB.
  const auto& spineItem = epub->getSpineItem(spineIndex);
  std::string chapterDir;
  {
    const size_t slash = spineItem.href.rfind('/');
    if (slash != std::string::npos) chapterDir = spineItem.href.substr(0, slash + 1);
  }
  const std::string imageBasePath = epub->getCachePath() + "/img_v" + std::to_string(spineIndex) + "_";

  VerticalParsedText layout(renderer, fontId, viewportWidth, viewportHeight);
  layout.preallocateStream();
  // Column gap (行間) in EMS -- the unit the constraint is stated in. Ruby is half an em (JLREQ
  // 3.3.3) and is drawn in this gap beside its base column, so with furigana ON the floor is half
  // an em, and the settings span the half-to-full em Japanese body text conventionally uses. With
  // furigana OFF nothing is drawn there, so every setting tightens by a quarter em. (Line height
  // would be the wrong yardstick: it carries Latin leading, 42px against a 28px em here.)
  renderer.ensureSdCardFontReady(fontId, "\xe6\xbc\xa2", 0x01);  // 漢
  const int emPx = std::max(1, renderer.getTextAdvanceX(fontId, "\xe6\xbc\xa2", static_cast<EpdFontFamily::Style>(0)));
  const int clampedLineSpacing = std::clamp<int>(lineSpacing, 0, 2);
  static constexpr int kGapQuarterEmsWithRuby[] = {2, 3, 4};  // Tight 1/2, Normal 3/4, Wide 1 em
  static constexpr int kGapQuarterEmsPlain[] = {1, 2, 3};     // Tight 1/4, Normal 1/2, Wide 3/4 em
  const int gapQuarterEms =
      furiganaEnabled ? kGapQuarterEmsWithRuby[clampedLineSpacing] : kGapQuarterEmsPlain[clampedLineSpacing];
  layout.setColumnGapPx(std::max(2, emPx * gapQuarterEms / 4));

  LayoutPageSink sink(layout, out, pageOffsets_, *epub, renderer, chapterDir, imageBasePath, viewportWidth,
                      viewportHeight);
  sink.earlyRenderFn = earlyRenderFn_;
  sink.earlyRenderCtx = earlyRenderCtx_;
  // Seed the shared page-request slot with the initial early-render target; page turns
  // recorded while the build runs simply overwrite it (latest wins).
  buildPageRequest_.store(earlyRenderFn_ ? earlyRenderTargetPage_ : -1, std::memory_order_relaxed);
  sink.pageRequest = &buildPageRequest_;

  // Styled blocks (borders, start offsets, hanging indents, centering, gaps): collect the
  // vertical-relevant selectors. Streams the on-disk CSS cache -- does NOT materialize the
  // rule map (heap!). The table lives for the whole build (~10-15KB for a full EBPAJ book),
  // so under heap pressure it is bounded down or skipped entirely: correct TEXT layout beats
  // styling fidelity on a tight (X3) heap, and the release below reclaims font memory first.
  std::vector<std::pair<std::string, CssParser::VerticalBlockStyle>> blockStyles;
  if (epub->getCssParser()) {
    if (ESP.getMaxAllocHeap() < 64 * 1024) {
      if (auto* fcm = renderer.getFontCacheManager()) {
        LOG_INF("VSC", "Low heap before styled-block collect (maxAlloc=%u); releasing font memory",
                ESP.getMaxAllocHeap());
        fcm->releaseAllFontMemory();
      }
    }
    // Binary decision, no partial cap: the collector fills in CACHE order, so a reduced cap
    // silently drops whichever selectors happen to sit late in the file (confirmed earlier:
    // .k-solid boxes vanishing at a 64-entry cap). Either the full table fits, or the build
    // runs unstyled AND is stamped stale so a later, roomier open rebuilds it properly.
    const uint32_t maxAllocNow = ESP.getMaxAllocHeap();
    if (maxAllocNow < 48 * 1024) {
      LOG_ERR("VSC", "Heap too tight for styled blocks (maxAlloc=%u); building unstyled, marked for rebuild",
              maxAllocNow);
      lastBuildDroppedForHeap_ = true;  // reuse the stale-stamp path: version 0 -> rebuild next open
    } else {
      epub->getCssParser()->collectVerticalStyles(blockStyles);
      if (!blockStyles.empty()) {
        LOG_DBG("VSC", "%u styled-block selectors active", static_cast<unsigned>(blockStyles.size()));
      }
    }
  }

  TextExtractor extractor;
  extractor.sink = &sink;
  extractor.rubyHarvest = &sink.rubyHarvest;
  extractor.blockStyles = &blockStyles;
  // Pin every buffer that lives across the whole build to its worst case NOW, while the heap
  // is freshest -- mid-build growth (doubling alloc-copy-free) plants persistent blocks in
  // the region the per-flush transients need, shredding the largest contiguous block over the
  // chapter (observed live: maxAlloc 77K -> 4K -> abort on a novel shipped as one 238KB
  // file). The streaming cadence (SOFT_FLUSH_BYTES/SOFT_FLUSH_RUNS) keeps these worst cases
  // small; same rationale as VerticalParsedText::preallocateStream().
  extractor.currentText.reserve(TextExtractor::SOFT_FLUSH_BYTES + 512);
  extractor.currentRuns.reserve(TextExtractor::SOFT_FLUSH_RUNS + 8);
  extractor.rubyBase.reserve(TextExtractor::RUBY_RESERVE_HINT);
  extractor.rubyAnnotation.reserve(TextExtractor::RUBY_RESERVE_HINT);
  pageOffsets_.reserve(640);  // 2.5KB; a 240KB chapter yields ~500 pages

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR("VSC", "OOM: XML parser");
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  LOG_DBG("VSC", "after XML_ParserCreate: maxAlloc=%u", ESP.getMaxAllocHeap());

  XML_SetDefaultHandlerExpand(parser, TextExtractor::defaultHandler);
  XML_SetUserData(parser, &extractor);
  XML_SetElementHandler(parser, TextExtractor::startElement, TextExtractor::endElement);
  XML_SetCharacterDataHandler(parser, TextExtractor::characterData);

  HalFile htmlFile;
  if (!Storage.openFileForRead("VSC", tmpHtmlPath, htmlFile)) {
    destroyXmlParser(parser);
    Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  bool parseOk = true;
  int done;
  do {
    void* const buf = XML_GetBuffer(parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("VSC", "OOM: parse buffer");
      parseOk = false;
      break;
    }
    const size_t len = htmlFile.read(buf, PARSE_BUFFER_SIZE);
    if (len == 0 && htmlFile.available() > 0) {
      LOG_ERR("VSC", "File read error");
      parseOk = false;
      break;
    }
    done = htmlFile.available() == 0;
    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      LOG_ERR("VSC", "XML parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      parseOk = false;
      break;
    }
  } while (!done);

  htmlFile.close();
  destroyXmlParser(parser);
  Storage.remove(tmpHtmlPath.c_str());

  if (!parseOk) return false;

  extractor.flushParagraph();
  sink.flushText(/*isFinalFlush=*/true);

  if (sink.failed) return false;

  // OR, don't assign: the styled-block collect above may already have flagged this build
  // (unstyled fallback under heap pressure).
  // Emergency page splits count as heap degradation too: no content is lost, but pages end at
  // arbitrary fill levels, so the same usable-now/rebuild-next-open path applies (and the same
  // rebuildingFromStale_ guard breaks the loop when a retry degrades again).
  lastBuildDroppedForHeap_ = lastBuildDroppedForHeap_ || layout.everDroppedForHeap() || layout.everSplitForHeap();
  if (auto* fcm = renderer.getFontCacheManager()) {
    if (auto* fd = fcm->getDecompressor()) {
      const uint32_t starved = fd->getStarvedGlyphCount() - starvedGlyphsAtStart;
      if (starved > 0) {
        LOG_ERR("VSC", "%u glyph(s) measured without ink data on low heap; layout is approximate", starved);
        lastBuildDroppedForHeap_ = true;
      }
    }
  }
  // Persist harvested furigana pairs; runs after the parse buffers are freed, so the
  // transient merge buffer doesn't compete with layout's peak memory.
  RubyGlossary::merge(epub->getCachePath(), sink.rubyHarvest);
  LOG_INF("VSC", "streamParseAndLayout: %u ms", millis() - buildStartMs);
  LOG_INF("VSC", "streamParseAndLayout end spine=%d pages=%zu free=%u", spineIndex, pageOffsets_.size(),
          ESP.getFreeHeap());
  return true;
}

bool VerticalSection::createSectionFile(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight,
                                        const uint8_t lineSpacing, const bool furiganaEnabled) {
  const auto vsectionsDir = epub->getCachePath() + "/vsections";
  Storage.mkdir(vsectionsDir.c_str());

  pageOffsets_.clear();
  loadedPageIndex_ = -1;
  pageCount = 0;

  HalFile file;
  if (!Storage.openFileForWrite("VSC", filePath, file)) {
    return false;
  }

  // Header with placeholders; pageCount and the offset-table location aren't known until all
  // pages have been streamed out, so they're patched by the seek-back below. The write mode is
  // O_RDWR (not append), so the seek-back write lands in place.
  serialization::writePod(file, VSECTION_FILE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, lineSpacing);
  const uint8_t furiganaFlag = furiganaEnabled ? 1 : 0;
  serialization::writePod(file, furiganaFlag);
  const uint16_t pageCountPlaceholder = 0;
  const uint32_t indexOffsetPlaceholder = 0;
  serialization::writePod(file, pageCountPlaceholder);
  serialization::writePod(file, indexOffsetPlaceholder);

  if (!streamParseAndLayout(file, fontId, viewportWidth, viewportHeight, lineSpacing, furiganaEnabled)) {
    file.close();
    Storage.remove(filePath.c_str());
    pageOffsets_.clear();
    return false;
  }

  const auto indexOffset = static_cast<uint32_t>(file.position());
  for (const uint32_t off : pageOffsets_) {
    serialization::writePod(file, off);
  }

  pageCount = static_cast<uint16_t>(pageOffsets_.size());
  if (!file.seek(HEADER_PAGECOUNT_OFFSET)) {
    file.close();
    Storage.remove(filePath.c_str());
    pageOffsets_.clear();
    pageCount = 0;
    return false;
  }
  serialization::writePod(file, pageCount);
  serialization::writePod(file, indexOffset);
  // A build that dropped content on low heap produced sparse pages. Keep the file usable for
  // THIS session (offsets are in RAM, pages read back fine) but stamp version 0 so the next
  // open hits the version-mismatch path in loadSectionFile and rebuilds the chapter -- with,
  // ideally, a healthier heap -- instead of the truncation being persisted as a valid cache.
  if (lastBuildDroppedForHeap_ && !rebuildingFromStale_) {
    LOG_ERR("VSC", "Build dropped glyphs on low heap; marking section stale for rebuild on next open");
    if (file.seek(0)) {
      const uint8_t staleVersion = 0;
      serialization::writePod(file, staleVersion);
    }
  } else if (lastBuildDroppedForHeap_) {
    // The retry ALSO dropped: conditions are deterministic, another rebuild would too. Keep the
    // best-effort cache valid -- a few missing glyphs on the densest pages beat re-indexing the
    // whole chapter on every single open.
    LOG_ERR("VSC", "Stale-rebuild dropped glyphs again; keeping best-effort cache to break the rebuild loop");
  }
  file.close();

  // Last page's source position. The horizontal build logs the same number ("SCT: chapter
  // spans"), and the two counters are supposed to agree character for character -- if these
  // diverge by more than roughly one page's worth of text, the two parsers have drifted and
  // cross-mode position restore is silently landing on the wrong page.
  const auto lastStart = getVisibleTextOffsetForPage(static_cast<int>(pageCount) - 1);
  LOG_DBG("VSC", "Cached %u vertical pages (streamed); chapter spans %u chars", pageCount, lastStart.value_or(0));
  return true;
}

bool VerticalSection::loadSectionFile(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight,
                                      const uint8_t lineSpacing, const bool furiganaEnabled) {
  // A missing cache file is the NORMAL case here, not an error: the book-progress counter probes
  // every spine's section on each page turn, and unbuilt chapters simply don't have one yet.
  // openFileForRead would print "File does not exist" per spine per probe -- pure log spam.
  if (!Storage.exists(filePath.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("VSC", filePath, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version != VSECTION_FILE_VERSION) {
    file.close();
    LOG_DBG("VSC", "Version mismatch: %u vs %u", version, VSECTION_FILE_VERSION);
    // Version 0 is the stale stamp from a build that dropped glyphs on low heap. Remember it:
    // if the RETRY build drops again, the conditions are deterministic (same book, same heap
    // shape) and re-stamping would rebuild the chapter on every open forever -- observed live
    // as a ~30s "Indexing" on each open of a whole-book-in-one-file novel.
    if (version == 0) rebuildingFromStale_ = true;
    clearCache();
    return false;
  }

  int cachedFontId;
  uint16_t cachedWidth, cachedHeight;
  uint8_t cachedLineSpacing;
  uint8_t cachedFurigana;
  serialization::readPod(file, cachedFontId);
  serialization::readPod(file, cachedWidth);
  serialization::readPod(file, cachedHeight);
  serialization::readPod(file, cachedLineSpacing);
  serialization::readPod(file, cachedFurigana);

  if (cachedFontId != fontId || cachedWidth != viewportWidth || cachedHeight != viewportHeight ||
      cachedLineSpacing != lineSpacing || cachedFurigana != (furiganaEnabled ? 1 : 0)) {
    file.close();
    LOG_DBG("VSC", "Parameter mismatch, clearing cache");
    clearCache();
    return false;
  }

  uint16_t cachedPageCount;
  uint32_t indexOffset;
  serialization::readPod(file, cachedPageCount);
  serialization::readPod(file, indexOffset);

  pageOffsets_.clear();
  loadedPageIndex_ = -1;

  if (cachedPageCount > 0) {
    if (indexOffset == 0 || !file.seek(indexOffset)) {
      file.close();
      LOG_ERR("VSC", "Bad page index offset in cache");
      clearCache();
      return false;
    }
    pageOffsets_.resize(cachedPageCount);
    const size_t want = static_cast<size_t>(cachedPageCount) * sizeof(uint32_t);
    const size_t got = file.read(reinterpret_cast<uint8_t*>(pageOffsets_.data()), want);
    if (got != want) {
      pageOffsets_.clear();
      file.close();
      LOG_ERR("VSC", "Truncated page index in cache");
      clearCache();
      return false;
    }
  }

  file.close();
  pageCount = cachedPageCount;
  LOG_DBG("VSC", "Opened cache: %u vertical pages (index only, %u bytes resident)", pageCount,
          static_cast<unsigned>(pageOffsets_.size() * sizeof(uint32_t)));
  return true;
}

bool VerticalSection::clearCache() const {
  if (!Storage.exists(filePath.c_str())) {
    return true;
  }
  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("VSC", "Failed to clear cache");
    return false;
  }
  LOG_DBG("VSC", "Cache cleared");
  return true;
}

std::optional<uint32_t> VerticalSection::getVisibleTextOffsetForPage(const int page) const {
  if (page < 0 || page >= static_cast<int>(pageOffsets_.size())) return std::nullopt;
  // Already resident: the loaded page carries the value, no SD access at all. This is the
  // common case (the reader asks about the page it is showing, on every progress save).
  if (loadedPageIndex_ == page) return loadedPage_.visibleTextOffset;
  HalFile file;
  if (!Storage.openFileForRead("VSC", filePath, file)) return std::nullopt;
  if (!file.seek(pageOffsets_[static_cast<size_t>(page)])) return std::nullopt;
  uint32_t offset = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&offset), sizeof(offset)) != static_cast<int>(sizeof(offset))) {
    return std::nullopt;
  }
  return offset;
}

std::optional<int> VerticalSection::getPageForVisibleTextOffset(const uint32_t offset) const {
  if (pageOffsets_.empty()) return std::nullopt;
  HalFile file;
  if (!Storage.openFileForRead("VSC", filePath, file)) return std::nullopt;
  // Reads the first field of one page record. Returns nullopt on a bad read so a truncated
  // file degrades to "cannot resolve" (the caller falls back) rather than to a wrong page.
  auto pageStart = [&](const int page) -> std::optional<uint32_t> {
    if (!file.seek(pageOffsets_[static_cast<size_t>(page)])) return std::nullopt;
    uint32_t v = 0;
    if (file.read(reinterpret_cast<uint8_t*>(&v), sizeof(v)) != static_cast<int>(sizeof(v))) return std::nullopt;
    return v;
  };
  // Page starts are non-decreasing (pages are laid out in document order), so a plain binary
  // search finds the last page starting at or before `offset`.
  int lo = 0;
  int hi = static_cast<int>(pageOffsets_.size()) - 1;
  int best = 0;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    const auto start = pageStart(mid);
    if (!start) return std::nullopt;
    if (*start <= offset) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return best;
}

const VerticalPage* VerticalSection::getPage() const { return getPage(currentPage); }

const VerticalPage* VerticalSection::getPage(int pageIndex) const {
  if (pageIndex < 0 || pageIndex >= static_cast<int>(pageOffsets_.size())) {
    return nullptr;
  }
  if (pageIndex == loadedPageIndex_) {
    return &loadedPage_;
  }

  // Fault the page in from the SD cache. The previous pointer returned by getPage() is
  // invalidated here -- all callers fetch-and-render one page at a time.
  loadedPageIndex_ = -1;
  HalFile file;
  if (!Storage.openFileForRead("VSC", filePath, file)) {
    return nullptr;
  }
  if (!file.seek(pageOffsets_[static_cast<size_t>(pageIndex)])) {
    file.close();
    return nullptr;
  }
  const ReadResult result = readPage(file, loadedPage_);
  file.close();
  // Tell the reader whether a nullptr means "transient low heap, keep the cache" or a genuine
  // failure worth clearing/rebuilding -- see lastReadHeapRefused().
  lastReadHeapRefused_ = (result == ReadResult::HeapRefused);
  if (result != ReadResult::Ok) {
    LOG_ERR("VSC", "Failed to read page %d from cache (%s)", pageIndex,
            result == ReadResult::HeapRefused ? "low heap" : "corrupt");
    return nullptr;
  }
  loadedPageIndex_ = pageIndex;
  return &loadedPage_;
}
