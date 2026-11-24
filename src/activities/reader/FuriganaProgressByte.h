#pragma once

#include <cstdint>

namespace EpubReaderUtils {

// progress.bin byte 7 holds the book's furigana choices. Low nibble: the on/off override (0xF
// unset, 0 off, 1 on). High nibble: 1 + the Furigana Size override (0 or 0xF unset). Files
// written before sizes existed hold 0xFF, 0 or 1, which read the same as before.
inline int8_t packFurigana(const int8_t on, const int8_t size) {
  const uint8_t low = on < 0 ? 0x0F : static_cast<uint8_t>(on ? 1 : 0);
  const uint8_t high = size >= 0 ? static_cast<uint8_t>(size + 1) : (on < 0 ? 0x0F : 0);
  return static_cast<int8_t>(static_cast<uint8_t>((high << 4) | low));
}
inline void unpackFurigana(const uint8_t stored, int8_t& on, int8_t& size) {
  const uint8_t low = stored & 0x0F;
  const uint8_t high = stored >> 4;
  on = low == 0x0F ? -1 : static_cast<int8_t>(low != 0 ? 1 : 0);
  size = high >= 1 && high <= 4 ? static_cast<int8_t>(high - 1) : -1;
}

}  // namespace EpubReaderUtils
