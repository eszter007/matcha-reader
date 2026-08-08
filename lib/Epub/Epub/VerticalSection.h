#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Epub.h"
#include "VerticalParsedText.h"

class GfxRenderer;
class HalFile;

// A vertical-text chapter, backed by an on-SD page cache.
//
// Memory model: pages are NEVER all held in RAM. On an ESP32-C3 (~220KB usable heap) a single
// real Japanese chapter (~30k characters) would need ~1.3MB of laid-out VerticalGlyphs -- the
// original hold-everything design only ever worked in the desktop emulator's 8MB heap. Instead:
//   - createSectionFile() streams: XML parse feeds paragraphs directly into layout, and each
//     batch of laid-out pages is serialized to the cache file immediately and freed.
//   - loadSectionFile() reads only the header + a per-page offset table (4 bytes/page).
//   - getPage() loads the one requested page from SD on demand into a single-page cache.
class VerticalSection {
  std::shared_ptr<Epub> epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;

  // File offset of each serialized page record within the cache file.
  std::vector<uint32_t> pageOffsets_;

  // Single-page read cache backing getPage()'s returned pointer. Mutable because getPage() is
  // const to callers (a read) but faults the page in from SD. The pointer returned by getPage()
  // is invalidated by the next getPage() call for a different index -- all existing callers
  // fetch-and-render one page at a time, never holding two pages.
  //
  // NOT thread-safe, and the slot is shared by BOTH tasks: the render task re-faults it from
  // render()'s prewarm and image-warm tail (seconds long), the main task from the menu / word
  // lookup / bookmark paths. Faulting a page mutates this one std::vector<VerticalGlyph>, so a
  // concurrent pair of getPage() calls frees the glyph buffer under the other task -- observed
  // as a heap_caps_free panic ("target pointer is outside heap areas") on the NEXT clear().
  // Rule: main-task callers must hold a RenderLock across getPage() AND every use of its
  // result; the render task is covered by the lock it renders under.
  mutable VerticalPage loadedPage_;
  mutable int loadedPageIndex_ = -1;
  // Set by getPage() when its last read failed because the glyph vector could not be reserved
  // on a low heap (the on-disk record is fine). Lets the reader retry later instead of clearing
  // a valid cache. See lastReadHeapRefused().
  mutable bool lastReadHeapRefused_ = false;

  bool streamParseAndLayout(HalFile& out, int fontId, uint16_t viewportWidth, uint16_t viewportHeight,
                            uint8_t lineSpacing, bool furiganaEnabled);

  // Set by streamParseAndLayout when the layout dropped chars/glyphs on low heap. The pages that
  // made it to disk are readable (this session keeps working), but createSectionFile stamps the
  // file with version 0 so the next open sees a version mismatch and rebuilds the chapter --
  // instead of the truncation living on disk as a permanently sparse chapter.
  bool lastBuildDroppedForHeap_ = false;
  // Set by loadSectionFile when the on-disk cache carried the version-0 stale stamp (a prior
  // build dropped glyphs). If THIS build drops again, createSectionFile keeps the best-effort
  // cache valid instead of re-stamping: the drop conditions are deterministic per book, so
  // re-stamping meant a full re-index on every open, forever.
  bool rebuildingFromStale_ = false;

  // See setEarlyRenderHook().
  void (*earlyRenderFn_)(void*, const VerticalPage&, int) = nullptr;
  void* earlyRenderCtx_ = nullptr;
  int earlyRenderTargetPage_ = -1;
  // See requestPageDuringBuild(). Written by the loop() task, read by the build on the
  // render task; -1 = no pending request.
  std::atomic<int> buildPageRequest_{-1};

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit VerticalSection(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer)
      : epub(epub),
        spineIndex(spineIndex),
        renderer(renderer),
        filePath(epub->getCachePath() + "/vsections/" + std::to_string(spineIndex) + ".bin") {}

  // Early-first-render hook: when set, createSectionFile() invokes fn(ctx, page, pageIndex)
  // once, right after the page with index targetPage has been laid out and written to the
  // cache -- letting the reader show the user's page a couple of seconds into a whole-chapter
  // build (~17s for a 431-page book) while the remaining pages keep building. Fires for text
  // pages only: an image page would pay its multi-second decode in the middle of the build.
  // Silent background builds simply never set the hook. Function pointer + ctx, not
  // std::function, per the platform binary/heap rules.
  void setEarlyRenderHook(void* ctx, void (*fn)(void*, const VerticalPage&, int), int targetPage) {
    earlyRenderCtx_ = ctx;
    earlyRenderFn_ = fn;
    earlyRenderTargetPage_ = targetPage;
  }

  // Mid-build page turns: the reader's loop() task records the page the user wants while a
  // build is still running on the render task; the build serves it through the early-render
  // hook as soon as that page exists (immediately via cache read-back if it is already
  // written, otherwise the moment it is laid out). Latest request wins -- rapid presses
  // collapse to the final target.
  void requestPageDuringBuild(int pageIndex) { buildPageRequest_.store(pageIndex, std::memory_order_relaxed); }

  // furiganaEnabled is part of the cache key: the column gap only has to clear ruby when ruby is
  // drawn, so turning furigana off tightens the columns and the chapter must be re-laid out.
  bool loadSectionFile(int fontId, uint16_t viewportWidth, uint16_t viewportHeight, uint8_t lineSpacing,
                       bool furiganaEnabled);
  bool createSectionFile(int fontId, uint16_t viewportWidth, uint16_t viewportHeight, uint8_t lineSpacing,
                         bool furiganaEnabled);
  bool clearCache() const;

  // Position of a page's first character within the chapter's visible character data, in the
  // same units Section::getVisibleTextOffsetForPage() reports -- which is what makes a reading
  // position portable between the two layouts. See VerticalPage::visibleTextOffset.
  //
  // Read straight from the page record on SD (its first field). No table is held in RAM and
  // none is written: the reverse lookup binary-searches the page records through the offset
  // table that loadSectionFile already keeps, which costs ~9 four-byte reads on a 500-page
  // chapter and, unlike a second index, nothing at all during the build -- where this layout
  // is already fighting for every contiguous KB.
  std::optional<uint32_t> getVisibleTextOffsetForPage(int page) const;

  // The page holding `offset`: the last page whose own offset is <= it. nullopt if the chapter
  // has no pages or the records cannot be read.
  std::optional<int> getPageForVisibleTextOffset(uint32_t offset) const;
  const VerticalPage* getPage() const;
  const VerticalPage* getPage(int pageIndex) const;
  // True when the most recent getPage() returned nullptr only because the page's glyph vector
  // could not be reserved on a low heap -- the cache is valid; retry, do not clear it.
  bool lastReadHeapRefused() const { return lastReadHeapRefused_; }
};
