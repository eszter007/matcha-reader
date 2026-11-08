#pragma once

// Heap guards shared by the vertical text stream (VerticalParsedText.cpp) and its layout
// (VerticalLayout.cpp). Internal to those two files.

#include <Arduino.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Epub/VerticalBatch.h"

namespace {
// Headroom required ON TOP of a reserve() before it is attempted. Below it, skip the reserve and
// let the vector grow incrementally: many small allocations are likelier to succeed than one large
// one.
//
// Keep this small. The margin is cushion for OTHER allocations during a build (SD write buffers,
// log lines -- low KBs; the rest of the app is quiescent), not for the reserve itself, which the
// getMaxAllocHeap() check already guarantees. A margin large enough to refuse a legitimate bulk
// reserve is worse than none: incremental doubling then costs hundreds of reallocations for the
// same data and fragments the heap ~22KB beyond the single allocation it withheld.
constexpr uint32_t MIN_FREE_HEAP_FOR_RESERVE = 4 * 1024;

// Cushion left for the rest of the app when growing a vector one element at a time, as opposed
// to MIN_FREE_HEAP_FOR_RESERVE's cushion for a bulk reserve.
constexpr uint32_t SMALL_ALLOC_MARGIN = 8 * 1024;

// Every reserve() here is one contiguous allocation that ABORTS the process on failure under
// -fno-exceptions, so nothing may be requested without first checking it fits. `margin` is what
// the request must leave behind for everything else.
inline bool heapCanAfford(const size_t bytes, const uint32_t margin) {
  return vertical_batch::largestBlockFor(bytes) >= bytes + margin;
}

// Cushion for the per-run reserves in addAnnotatedParagraph, which are a few hundred bytes each --
// far below MIN_FREE_HEAP_FOR_RESERVE, sized for the multi-KB page and stream reserves. Charging
// those a 4 KB margin refuses requests that fit with room to spare, and the doubling growth that
// follows costs more allocations than the one it withheld: exactly what MIN_FREE_HEAP_FOR_RESERVE's
// own comment warns against. This has only to cover what another task may take between the check
// and the reserve; the fit itself is already guaranteed by getMaxAllocHeap().
constexpr uint32_t PER_RUN_RESERVE_MARGIN = 1024;

// Bulk reserve obeying that rule: skipped when the request would not fit, leaving the vector to
// grow incrementally rather than aborting the process.
template <typename T>
void reserveIfAffordable(std::vector<T>& vec, const size_t count) {
  if (count <= vec.capacity()) return;  // reserve() would not allocate; skip the heap query
  // A count big enough to wrap the byte figure -- either in the multiply here or in
  // heapCanAfford's margin add -- would compare as a SMALL request, pass the check, and abort in
  // reserve(): the very failure this helper exists to prevent. Refuse it instead; the vector then
  // grows incrementally, which is the same fallback an unaffordable request already takes.
  if (count > (SIZE_MAX - PER_RUN_RESERVE_MARGIN) / sizeof(T)) return;
  if (heapCanAfford(count * sizeof(T), PER_RUN_RESERVE_MARGIN)) vec.reserve(count);
}

// Upper bound on the entries a UTF-8 decode of `text` can append: one per decode step (one per
// codepoint for well-formed UTF-8; malformed/truncated bytes still count as one step). One pass,
// no allocation. Japanese sits at ~3 bytes per codepoint, where reserving by byte count instead
// asks for ~3x the heap.
size_t countUtf8Codepoints(const std::string& text) {
  size_t count = 0;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c0 = static_cast<unsigned char>(text[i]);
    size_t consumed = 1;
    if (c0 < 0x80) {
      consumed = 1;
    } else if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
      consumed = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
      consumed = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
      consumed = 4;
    }
    i += consumed;
    count++;
  }
  return count;
}

// Make room for one more push_back. Tries a doubling first (amortised growth), then a small
// linear step at each margin in turn, most protective first -- so a heap too tight for the
// doubled request still gets a cheap retry instead of latching. Without that fallback one
// refusal blocked every later element, since the doubled request never shrinks on its own:
// confirmed on device as the real cause of "sparse" pages.
// A margin of 0 is a deliberate last resort where the alternative is losing content outright.
template <typename T, size_t N>
bool growForOnePush(std::vector<T>& vec, const uint32_t (&margins)[N], const size_t linearStep) {
  if (vec.size() < vec.capacity()) return true;  // no reallocation needed, cheap path
  const size_t doubled = vec.capacity() == 0 ? 1 : vec.capacity() * 2;
  if (heapCanAfford(doubled * sizeof(T), margins[0])) {
    vec.reserve(doubled);
    return true;
  }
  const size_t linear = vec.capacity() + linearStep;
  for (size_t i = 0; i < N; i++) {
    if (heapCanAfford(linear * sizeof(T), margins[i])) {
      vec.reserve(linear);
      return true;
    }
  }
  return false;
}

// Can one more element be appended to a per-run scratch vector without an allocation the heap
// cannot serve? Small elements and a small step, so the margin is PER_RUN_RESERVE_MARGIN rather
// than SMALL_ALLOC_MARGIN, which is sized for the multi-KB stream and page buffers.
template <typename T>
bool canGrowScratch(std::vector<T>& vec) {
  static constexpr uint32_t MARGINS[] = {PER_RUN_RESERVE_MARGIN};
  constexpr size_t LINEAR_GROWTH_STEP = 32;
  return growForOnePush(vec, MARGINS, LINEAR_GROWTH_STEP);
}
}  // namespace
