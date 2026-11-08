#pragma once
#include <cstdint>
#include <cstring>

#include "BookStats.h"

// Little-endian field packing shared by the stats files on SD (bookstats/*.bin,
// book_activity.bin, bookstats_download.bin). Packed through a byte buffer, not a struct cast:
// ESP32-C3 faults on unaligned multi-byte loads and a record at an arbitrary file offset has no
// alignment guarantee.
namespace statsrecords {
inline void putU16(uint8_t* p, const uint16_t v) { memcpy(p, &v, 2); }
inline void putU32(uint8_t* p, const uint32_t v) { memcpy(p, &v, 4); }
inline uint16_t getU16(const uint8_t* p) {
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
inline uint32_t getU32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

// A date is year(2) month(1) day(1).
constexpr size_t DATE_BYTES = 4;
inline void putDate(uint8_t* p, const BookDay& d) {
  putU16(p, d.year);
  p[2] = d.month;
  p[3] = d.day;
}
inline BookDay getDate(const uint8_t* p) { return {getU16(p), p[2], p[3], 0}; }

// A reading day is date(4) minutes(2).
constexpr size_t DAY_BYTES = 6;
inline void putDay(uint8_t* p, const BookDay& d) {
  putDate(p, d);
  putU16(p + DATE_BYTES, d.minutes);
}
inline BookDay getDay(const uint8_t* p) {
  BookDay d = getDate(p);
  d.minutes = getU16(p + DATE_BYTES);
  return d;
}

inline int daysInMonth(const uint16_t y, const uint8_t m) {
  static constexpr int DAYS[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 0;
  if (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) return 29;
  return DAYS[m];
}
}  // namespace statsrecords
