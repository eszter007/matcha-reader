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

// Point sizes selectable for the active reader font, ascending and deduplicated.
// Never returns empty.
//
//  - `sdFamilyName` names a family the registry knows: exactly the sizes it ships.
//  - otherwise a built-in family is active: BUILTIN_READER_POINT_SIZES widened by every
//    size `companion` ships. `companion` is the JP extension paired with that built-in
//    (SdCardFontSystem::builtinJpCompanion), null when the card has none installed.
//
// A size only the companion ships is still selectable on a built-in row: a Japanese book
// renders from the companion at exactly that size, and Latin text falls back to the nearest
// size the built-in face exists at (CrossPointSettings::getBuiltinReaderFontId).
std::vector<uint8_t> readerFontPointSizes(const SdCardFontRegistry* registry, const char* sdFamilyName,
                                          const SdCardFontFamilyInfo* companion);

// Closest entry in `sizes` (ascending, `count` > 0) to `pt`; ties resolve to the
// smaller size. Takes a raw range rather than a vector because getReaderFontId()
// runs inside the page render loop and must not allocate.
uint8_t snapToNearestPointSize(const uint8_t* sizes, size_t count, uint8_t pt);

inline uint8_t snapToNearestPointSize(const std::vector<uint8_t>& sizes, const uint8_t pt) {
  return sizes.empty() ? pt : snapToNearestPointSize(sizes.data(), sizes.size(), pt);
}

// Nearest size a built-in family can be asked for, over the same set readerFontPointSizes()
// offers for one: BUILTIN_READER_POINT_SIZES widened by `companion`'s installed sizes.
// Allocation-free, so it can run on the settings-load and font-reload paths.
uint8_t snapToBuiltinPointSize(const SdCardFontFamilyInfo* companion, uint8_t pt);
