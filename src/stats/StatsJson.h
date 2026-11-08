#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// Token helpers shared by the stats server's streaming JSON parsers.
namespace statsjson {
inline bool equals(const char* value, const size_t len, const char* expected) {
  return len == strlen(expected) && memcmp(value, expected, len) == 0;
}

// "YYYY-MM-DD" into out.year/month/day. False for anything else; on a bad month or day the
// fields are already written, so callers reset out themselves.
template <typename T>
bool parseDate(const char* v, const size_t len, T& out) {
  if (len != 10 || v[4] != '-' || v[7] != '-') return false;
  for (const size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
    if (v[i] < '0' || v[i] > '9') return false;
  }
  out.year = static_cast<uint16_t>((v[0] - '0') * 1000 + (v[1] - '0') * 100 + (v[2] - '0') * 10 + (v[3] - '0'));
  out.month = static_cast<uint8_t>((v[5] - '0') * 10 + (v[6] - '0'));
  out.day = static_cast<uint8_t>((v[8] - '0') * 10 + (v[9] - '0'));
  return out.month >= 1 && out.month <= 12 && out.day >= 1 && out.day <= 31;
}
}  // namespace statsjson
