#pragma once

#include <cstddef>
#include <cstdint>

// The publisher fonts loaded for the open book, as seen by the layout engines: a CSS
// font-family key (fontFamilyKeyOf) maps to a renderer font id. The reader installs the map
// while a book is open; with none installed every family resolves to 0, the reader font.
namespace bookfonts {

constexpr size_t MAX_FAMILIES = 2;

// Replaces the map. Only BookFonts installs it, and it must match the bookFontsSignature of every
// section in use: layout resolves families through it, and vertical pages draw through it.
// count 0 (or clear()) empties it.
void setFamilies(const uint16_t* familyKeys, const int* fontIds, size_t count);
inline void clear() { setFamilies(nullptr, nullptr, 0); }

// The renderer font id serving this family, or 0 for the reader font.
int fontIdForFamily(uint16_t familyKey);

// True when fontId is one of the publisher fonts in the map.
bool isBookFont(int fontId);

// Vertical text names a font by a 2-bit slot instead of an id: 0 = the layout's own font,
// 1..MAX_FAMILIES = a publisher family, READER_SLOT = the reader font chosen explicitly.
constexpr uint8_t READER_SLOT = 3;
static_assert(MAX_FAMILIES < READER_SLOT, "slots must fit in 2 bits");
// The slot a font-family key selects: its family's, or READER_SLOT when another family is
// loaded instead. 0 when none is, so the property then changes nothing.
uint8_t slotForFamily(uint16_t familyKey);
// The font a slot draws with; 0 for slot 0 and READER_SLOT (the caller's own font).
int fontIdForSlot(uint8_t slot);

}  // namespace bookfonts
