#pragma once

#include <VectorFontSupport.h>

#include <cstdint>
#include <memory>

class Epub;
class GfxRenderer;
class TtfEpdFont;

// The publisher fonts (@font-face) of the open book, on boards with vector fonts (PSRAM).
// Up to MAX_FAMILIES families load, each with the styles the book ships; their files are held
// in PSRAM, de-obfuscated, and drawn by FreeType at the reader's point size. A glyph a family
// lacks comes from the reader font. On boards without vector fonts every call is a no-op.
class BookFonts {
 public:
  static constexpr int MAX_FAMILIES = 2;  // bookfonts::MAX_FAMILIES

  BookFonts();
  ~BookFonts();
  BookFonts(const BookFonts&) = delete;
  BookFonts& operator=(const BookFonts&) = delete;

  // Loads the book's families at pointSize, with readerFontId as their fallback, and installs
  // them in bookfonts' map. Keeps what is loaded when the same book, size and fallback
  // ask again. Returns the signature of what loaded (0 = nothing) for the section caches.
  uint32_t load(const Epub& epub, GfxRenderer& renderer, uint8_t pointSize, int readerFontId);

  // Releases every family and empties the map.
  void unload(GfxRenderer& renderer);

  // What load() last returned.
  uint32_t signature() const { return signature_; }

 private:
#if CROSSPOINT_VECTOR_FONTS
  struct Family;
  std::unique_ptr<Family> families_[MAX_FAMILIES];
#endif
  uint32_t signature_ = 0;
  uint32_t loadedFor_ = 0;  // hash of the book, size and fallback the families were built for
};

extern BookFonts bookFonts;
