#include "VerticalSection.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>
#include <expat.h>

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
constexpr uint8_t VSECTION_FILE_VERSION = 74;  // v74: force re-parse so the furigana glossary harvests existing books
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
  virtual void onImage(const std::string& src) = 0;
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
      currentRuns.push_back(RubyRun{currentText, {}, currentStyle(), hasEmphasis()});
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

  // paragraphEnds=false streams a partial paragraph: the sink lays it out with no break
  // recorded, and the next emit continues it seamlessly (continuesPrevious=true).
  void emitRuns(const bool paragraphEnds) {
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
    static constexpr const char* blockTags[] = {"p",  "div", "h1", "h2",         "h3",      "h4",
                                                "h5", "h6",  "li", "blockquote", "section", "article"};
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
        self->currentText += gaijiReplacementText(src ? src : "", alt ? alt : "");
      } else if (src && src[0] != '\0') {
        // Complete the paragraph built so far, then emit the image in document order. (For the
        // rare mid-paragraph image this places the partial text before the image, where the old
        // accumulate-then-interleave code placed the image before the whole paragraph; identical
        // for the usual block-level images.)
        self->flushParagraph();
        if (self->sink) self->sink->onImage(std::string(src));
      }
    }
    if (strcasecmp(name, "br") == 0 || strcasecmp(name, "br/") == 0) {
      if (!self->inRuby) {
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
                                            self->currentStyle(), self->hasEmphasis()});
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
        self->currentRuns.push_back(RubyRun{std::move(self->rubyBase), {}, self->currentStyle(), self->hasEmphasis()});
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
    if (self->skipDepth >= 0) return;
    if (self->inRp) return;
    if (self->inRt) {
      self->rubyAnnotation.append(s, static_cast<size_t>(len));
    } else if (self->inRuby) {
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
        self->rubyBase.append(resolved);
      } else {
        self->currentText.append(resolved);
      }
    }
  }
};

}  // namespace

namespace {

// ---- Page (de)serialization (cache format v37) -----------------------------------------------
// File layout:
//   header: u8 version, i32 fontId, u16 viewportWidth, u16 viewportHeight,
//           u16 pageCount, u32 indexOffset          (pageCount/indexOffset patched post-stream)
//   page records (variable length, written as pages are laid out)
//   footer at indexOffset: pageCount x u32 file offset of each page record
// The footer lets loadSectionFile() open a chapter by reading only the header + 4 bytes/page,
// and getPage() seek straight to one page -- pages are never all resident in RAM.

bool writePage(HalFile& file, const VerticalPage& page) {
  const bool isImg = page.isImagePage();
  serialization::writePod(file, isImg);
  if (isImg) {
    serialization::writeString(file, page.imagePath);
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
  for (uint8_t bi = 0; bi < boxCount; bi++) {
    serialization::writePod(file, page.boxes[bi].x);
    serialization::writePod(file, page.boxes[bi].y);
    serialization::writePod(file, page.boxes[bi].w);
    serialization::writePod(file, page.boxes[bi].h);
    serialization::writePod(file, page.boxes[bi].edges);
  }

  for (const auto& g : page.glyphs) {
    serialization::writePod(file, g.codepoint);
    serialization::writePod(file, g.column);
    serialization::writePod(file, g.row);
    serialization::writePod(file, g.x);
    serialization::writePod(file, g.y);
    serialization::writePod(file, g.paragraphIndex);
    serialization::writePod(file, g.byteOffset);
    serialization::writePod(file, g.renderKind);
    serialization::writePod(file, g.style);
    serialization::writePod(file, g.emphasis);

    if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
      const auto runLen = static_cast<uint16_t>(g.rotatedRunText.size());
      serialization::writePod(file, runLen);
      if (runLen > 0) {
        file.write(reinterpret_cast<const uint8_t*>(g.rotatedRunText.data()), runLen);
      }
    }

    const auto rubyLen = static_cast<uint16_t>(g.rubyText.size());
    serialization::writePod(file, rubyLen);
    if (rubyLen > 0) {
      file.write(reinterpret_cast<const uint8_t*>(g.rubyText.data()), rubyLen);
    }
  }
  return true;
}

bool readPage(HalFile& file, VerticalPage& page) {
  page.glyphs.clear();
  page.boxes.clear();
  page.imagePath.clear();

  bool isImg = false;
  serialization::readPod(file, isImg);
  if (isImg) {
    serialization::readString(file, page.imagePath);
    serialization::readPod(file, page.imageWidth);
    serialization::readPod(file, page.imageHeight);
    serialization::readPod(file, page.imageRotated);
    return !page.imagePath.empty();
  }

  uint32_t glyphCount;
  serialization::readPod(file, glyphCount);
  serialization::readPod(file, page.columnCount);
  serialization::readPod(file, page.rowsPerColumn);

  uint8_t boxCount = 0;
  serialization::readPod(file, boxCount);
  if (boxCount > 8) {
    LOG_ERR("VSC", "Corrupt page record: %u boxes", boxCount);
    return false;
  }
  page.boxes.reserve(boxCount);
  for (uint8_t bi = 0; bi < boxCount; bi++) {
    VerticalBoxRect r;
    serialization::readPod(file, r.x);
    serialization::readPod(file, r.y);
    serialization::readPod(file, r.w);
    serialization::readPod(file, r.h);
    serialization::readPod(file, r.edges);
    page.boxes.push_back(r);
  }

  // One page is bounded by screen geometry (a few hundred cells); a corrupt count must not
  // drive a huge reserve on a heap that can't take it.
  constexpr uint32_t MAX_GLYPHS_PER_PAGE = 4096;
  if (glyphCount > MAX_GLYPHS_PER_PAGE) {
    LOG_ERR("VSC", "Corrupt page record: %u glyphs", glyphCount);
    return false;
  }
  page.glyphs.reserve(glyphCount);

  for (uint32_t gi = 0; gi < glyphCount; gi++) {
    VerticalGlyph g;
    serialization::readPod(file, g.codepoint);
    serialization::readPod(file, g.column);
    serialization::readPod(file, g.row);
    serialization::readPod(file, g.x);
    serialization::readPod(file, g.y);
    serialization::readPod(file, g.paragraphIndex);
    serialization::readPod(file, g.byteOffset);
    serialization::readPod(file, g.renderKind);
    serialization::readPod(file, g.style);
    serialization::readPod(file, g.emphasis);

    if (g.renderKind == VerticalGlyph::RotatedRun || g.renderKind == VerticalGlyph::UprightRun) {
      uint16_t runLen;
      serialization::readPod(file, runLen);
      if (runLen > 0) {
        g.rotatedRunText.resize(runLen);
        file.read(reinterpret_cast<uint8_t*>(g.rotatedRunText.data()), runLen);
      }
    }

    uint16_t rubyLen;
    serialization::readPod(file, rubyLen);
    if (rubyLen > 0) {
      g.rubyText.resize(rubyLen);
      file.read(reinterpret_cast<uint8_t*>(g.rubyText.data()), rubyLen);
    }
    page.glyphs.push_back(std::move(g));
  }
  return true;
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

  void onImage(const std::string& src) override {
    if (failed) return;
    // Lay out any buffered text, then FINALIZE the in-progress page before the image page is
    // written. Without this, the image page was written while the half-filled text page stayed
    // pending: the pending page (whose content PRECEDES the image) landed in the cache AFTER
    // the image page, and post-image text silently merged onto it -- confirmed on a real device
    // as dialogue continuing mid-column across a scene-break graphic instead of starting fresh.
    flushText();
    VerticalPage pendingTail;
    if (layout.finalizePendingPage(pendingTail)) writeOne(pendingTail);
    writeOne(makeImagePage(src));
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
    // TEMP diagnostics: bisect which stage of the flush plants persistent allocations
    // (maxAlloc collapsed 82K -> 2K over one chapter on a real device; strip with the rest).
    const uint32_t maBefore = ESP.getMaxAllocHeap();
    // Streaming pages out via callback as they're finalized keeps at most ~2 pages' worth of
    // glyph buffers resident at once instead of the whole batch's -- see PageReadyCallback in
    // VerticalParsedText.h for why this is safe (oikomi only ever looks one page back).
    auto pages = layout.layoutPages(this, &writePageCallback, isFinalFlush);
    const uint32_t maLayout = ESP.getMaxAllocHeap();
    layout.reset();
    for (const auto& p : pages) writeOne(p);
    const uint32_t maAfter = ESP.getMaxAllocHeap();
    if (maAfter + 2048 < maBefore) {
      LOG_DBG("VSC", "flushText maxAlloc: %u -> %u (layout) -> %u (reset+write), free=%u", maBefore, maLayout, maAfter,
              ESP.getFreeHeap());
    }
  }

  void writeOne(const VerticalPage& p) {
    pageOffsets.push_back(static_cast<uint32_t>(out.position()));
    if (!writePage(out, p)) {
      LOG_ERR("VSC", "Failed to write page %zu to cache", pageOffsets.size() - 1);
      failed = true;
    }
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
        const bool extracted = epub.readItemContentsToStream(resolvedSrc, cachedFile, 4096);
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
    page.imageWidth = static_cast<int16_t>(displayW);
    page.imageHeight = static_cast<int16_t>(displayH);
    page.imageRotated = rotated;
    return page;
  }
};

// Byte offset of the pageCount field in the header: u8 version + i32 fontId + 2x u16 viewport.
constexpr size_t HEADER_PAGECOUNT_OFFSET = 1 + sizeof(int) + 2 * sizeof(uint16_t);

}  // namespace

bool VerticalSection::streamParseAndLayout(HalFile& out, const int fontId, const uint16_t viewportWidth,
                                           const uint16_t viewportHeight) {
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
    success = epub->readItemContentsToStream(localPath, tmpHtml, PARSE_BUFFER_SIZE);
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

  const bool hasRuby = fileContainsRubyTag(tmpHtmlPath);
  LOG_DBG("VSC", "after fileContainsRubyTag: maxAlloc=%u", ESP.getMaxAllocHeap());

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
  const int lineH = renderer.getLineHeight(fontId);
  layout.setColumnGapPx((lineH / 3) < 4 ? 4 : (lineH / 3));
  if (hasRuby) {
    layout.setColumnGapPx(lineH * 2 / 3);
    layout.setRightPaddingPx((lineH / 2) < 2 ? 2 : (lineH / 2));
  }

  LayoutPageSink sink(layout, out, pageOffsets_, *epub, renderer, chapterDir, imageBasePath, viewportWidth,
                      viewportHeight);

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
  lastBuildDroppedForHeap_ = lastBuildDroppedForHeap_ || layout.everDroppedForHeap();
  // Persist harvested furigana pairs; runs after the parse buffers are freed, so the
  // transient merge buffer doesn't compete with layout's peak memory.
  RubyGlossary::merge(epub->getCachePath(), sink.rubyHarvest);
  LOG_INF("VSC", "streamParseAndLayout: %u ms", millis() - buildStartMs);
  LOG_INF("VSC", "streamParseAndLayout end spine=%d pages=%zu free=%u", spineIndex, pageOffsets_.size(),
          ESP.getFreeHeap());
  return true;
}

bool VerticalSection::createSectionFile(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
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
  const uint16_t pageCountPlaceholder = 0;
  const uint32_t indexOffsetPlaceholder = 0;
  serialization::writePod(file, pageCountPlaceholder);
  serialization::writePod(file, indexOffsetPlaceholder);

  if (!streamParseAndLayout(file, fontId, viewportWidth, viewportHeight)) {
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
  if (lastBuildDroppedForHeap_) {
    LOG_ERR("VSC", "Build dropped glyphs on low heap; marking section stale for rebuild on next open");
    if (file.seek(0)) {
      const uint8_t staleVersion = 0;
      serialization::writePod(file, staleVersion);
    }
  }
  file.close();

  LOG_DBG("VSC", "Cached %u vertical pages (streamed)", pageCount);
  return true;
}

bool VerticalSection::loadSectionFile(const int fontId, const uint16_t viewportWidth, const uint16_t viewportHeight) {
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
    clearCache();
    return false;
  }

  int cachedFontId;
  uint16_t cachedWidth, cachedHeight;
  serialization::readPod(file, cachedFontId);
  serialization::readPod(file, cachedWidth);
  serialization::readPod(file, cachedHeight);

  if (cachedFontId != fontId || cachedWidth != viewportWidth || cachedHeight != viewportHeight) {
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
  const bool ok = readPage(file, loadedPage_);
  file.close();
  if (!ok) {
    LOG_ERR("VSC", "Failed to read page %d from cache", pageIndex);
    return nullptr;
  }
  loadedPageIndex_ = pageIndex;
  return &loadedPage_;
}
