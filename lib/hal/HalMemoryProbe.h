#pragma once

#include <cstdint>

// TEMPORARY diagnostic for the font-investigation branch. Brackets JP companion-font
// selection to find out how much of its heap cost is SdCardFont::load() itself vs.
// the candidate try/reject loop around it in SdCardFontSystem::ensureJpFallback().
// Not for merge -- remove once the font-memory investigation concludes.
namespace HalMemoryProbe {

// Record a labelled heap sample (free heap + largest allocatable block). No allocation.
void sample(const char* label);

// Append all samples recorded since the last flush to /heap-report.txt, then clear them.
void flush(const char* context);

}  // namespace HalMemoryProbe
