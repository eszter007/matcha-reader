#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Writes a date by a pattern: YYYY, MM and DD are zero-padded, M and D are not, N is `monthName`, lowercased
// when it is not the first token ("2026. október"); every other byte is copied as is, so a pattern can carry
// UTF-8 text ("YYYY年M月D日"). Truncates to fit `size`; callers size buffers so that never happens.
// Display only: stored and uploaded dates keep a fixed format whatever the UI language.
inline void formatDate(char* out, const size_t size, const char* pattern, const uint16_t year, const uint8_t month,
                       const uint8_t day, const char* monthName = nullptr) {
  if (size == 0) return;
  size_t n = 0;
  const auto put = [&](const char* s) {
    for (; *s && n + 1 < size; s++) out[n++] = *s;
  };
  char num[8];
  for (const char* p = pattern ? pattern : ""; *p;) {
    if (*p == 'N') {
      const size_t at = n;
      put(monthName ? monthName : "");
      if (p != pattern && at < n && out[at] >= 'A' && out[at] <= 'Z') out[at] = static_cast<char>(out[at] - 'A' + 'a');
      p++;
      continue;
    }
    if (strncmp(p, "YYYY", 4) == 0) {
      snprintf(num, sizeof(num), "%04u", static_cast<unsigned>(year));
      p += 4;
    } else if (*p == 'M' || *p == 'D') {
      const bool padded = p[1] == p[0];
      snprintf(num, sizeof(num), padded ? "%02u" : "%u", static_cast<unsigned>(*p == 'M' ? month : day));
      p += padded ? 2 : 1;
    } else {
      if (n + 1 < size) out[n++] = *p;
      p++;
      continue;
    }
    put(num);
  }
  out[n] = '\0';
}
