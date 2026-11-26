#pragma once

#include <VectorFontSupport.h>

#include <cstddef>
#include <cstdint>

// The publisher fonts loaded for the open book, as seen by the layout engines: a CSS
// font-family key (fontFamilyKeyOf) maps to a renderer font id. The reader installs the map
// while a book is open; with none installed every family resolves to 0, the reader font.
namespace bookfonts {

// Boards without vector fonts never load one; the map stays minimal there.
constexpr size_t MAX_FAMILIES = CROSSPOINT_VECTOR_FONTS ? 30 : 1;

// Replaces the map. Only BookFonts installs it, and it must match the bookFontsSignature of every
// section in use: layout resolves families through it, and vertical pages draw through it.
// count 0 (or clear()) empties it.
void setFamilies(const uint16_t* familyKeys, const int* fontIds, size_t count);
inline void clear() { setFamilies(nullptr, nullptr, 0); }

// The renderer font id serving this family, or 0 for the reader font.
int fontIdForFamily(uint16_t familyKey);

// True when fontId is one of the publisher fonts in the map.
bool isBookFont(int fontId);

// Vertical text names a font by a 5-bit slot instead of an id: 0 = the layout's own font,
// 1..MAX_FAMILIES = a publisher family, READER_SLOT = the reader font chosen explicitly.
constexpr uint8_t READER_SLOT = 31;
static_assert(MAX_FAMILIES < READER_SLOT, "slots must fit in 5 bits");
// The slot a font-family key selects: its family's, or READER_SLOT when another family is
// loaded instead. 0 when none is, so the property then changes nothing.
uint8_t slotForFamily(uint16_t familyKey);
// The slot of a loaded publisher font id; 0 when fontId is not one.
uint8_t slotForFontId(int fontId);
// The font a slot draws with; 0 for slot 0 and READER_SLOT (the caller's own font).
int fontIdForSlot(uint8_t slot);
// Identifies the installed families as slotForFamily sees them: equal stamps map every key alike.
uint64_t slotStamp();

}  // namespace bookfonts
