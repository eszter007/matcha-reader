#pragma once

#include <SdCardFontRegistry.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Reader font size is stored as an actual point size (see CrossPointSettings::
// fontPointSize), not an abstract Small/Medium/Large slot. The selectable sizes
// therefore come from whichever family is active: the built-in set below, or the
// .cpfont files a user installed for an SD family.

// The built-in Noto Serif / Noto Sans families are compiled in at exactly these
// point sizes (see the global font objects in main.cpp).
inline constexpr uint8_t BUILTIN_READER_POINT_SIZES[] = {12, 14, 16, 18};

// Point sizes selectable for the active reader row, ascending and deduplicated. Never empty.
//
// The row's own sizes -- the SD family `sdFamilyName` names, or BUILTIN_READER_POINT_SIZES for
// a built-in family -- widened by every size the `standIns` ship. Stand-ins are the families
// hidden from the picker that can still end up rendering this row: its coverage variant and the
// JP companion (SdCardFontSystem::readerStandInFamilies).
//
// A size only a stand-in ships is selectable because a book that stand-in carries renders at
// exactly that size. A book it does not carry falls back to the nearest size the row's own face
// exists at (CrossPointSettings::getBuiltinReaderFontId, SdCardFontFamilyInfo::findNearestSize).
std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName,
                                          const SdCardFontFamilyInfo* const* standIns, size_t standInCount);

// Closest entry in `sizes` (ascending, `count` > 0) to `pt`; ties resolve to the
// smaller size. Takes a raw range rather than a vector because getReaderFontId()
// runs inside the page render loop and must not allocate.
uint8_t snapToNearestPointSize(const uint8_t* sizes, size_t count, uint8_t pt);

inline uint8_t snapToNearestPointSize(const std::vector<uint8_t>& sizes, const uint8_t pt) {
  return sizes.empty() ? pt : snapToNearestPointSize(sizes.data(), sizes.size(), pt);
}
