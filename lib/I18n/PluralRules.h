#pragma once

#include <cstdint>

// Which form of a count message a whole number takes, by the CLDR plural rules for whole numbers.
// A count message lists its forms separated by '|' in the order of its language's categories:
//   None          one form only (Japanese, Chinese, Vietnamese, Indonesian)
//   OneOther      one | other              1 | everything else (English, German, ...)
//   ZeroOneOther  one | other              0 and 1 | everything else (French, Portuguese (Brazil), Persian)
//   EastSlavic    one | few | many         21, 31... | 2-4, 22-24... | 5-20, 25-30... (Russian, Ukrainian, Belarusian)
//   Polish        one | few | many         1 only | 2-4, 22-24... | everything else
//   CzechSlovak   one | few | other        1 | 2-4 | everything else
//   Lithuanian    one | few | other        1, 21... (not 11) | 2-9, 22-29... (not 12-19) | 0, 10-20, 30...
//   Bosnian       one | few | other        1, 21... (not 11) | 2-4, 22-24... (not 12-14) | everything else
//   Slovenian     one | two | few | other  1, 101... | 2, 102... | 3-4, 103-104... | everything else
//   Romanian      one | few | other        1 | 0, 2-19, 101-119... | 20-100, 120...
//   Arabic        zero | one | two | few | many | other   0 | 1 | 2 | 3-10 | 11-99 | 100-102...
//   Hebrew        one | two | other        1 | 2 | everything else
namespace plural {

enum class Rule : uint8_t {
  None,
  OneOther,
  ZeroOneOther,
  EastSlavic,
  Polish,
  CzechSlovak,
  Lithuanian,
  Bosnian,
  Slovenian,
  Romanian,
  Arabic,
  Hebrew,
};

// The index of the form for n under `rule` (negative counts use their absolute value).
inline uint8_t form(const Rule rule, int n) {
  if (n < 0) n = -n;
  const int mod10 = n % 10;
  const int mod100 = n % 100;
  switch (rule) {
    case Rule::None:
      return 0;
    case Rule::OneOther:
      return n == 1 ? 0 : 1;
    case Rule::ZeroOneOther:
      return n <= 1 ? 0 : 1;
    case Rule::EastSlavic:
    case Rule::Bosnian:
      if (mod10 == 1 && mod100 != 11) return 0;
      if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return 1;
      return 2;
    case Rule::Polish:
      if (n == 1) return 0;
      if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14)) return 1;
      return 2;
    case Rule::CzechSlovak:
      if (n == 1) return 0;
      return n >= 2 && n <= 4 ? 1 : 2;
    case Rule::Lithuanian:
      if (mod100 >= 11 && mod100 <= 19) return 2;
      if (mod10 == 1) return 0;
      return mod10 >= 2 ? 1 : 2;
    case Rule::Slovenian:
      if (mod100 == 1) return 0;
      if (mod100 == 2) return 1;
      return mod100 == 3 || mod100 == 4 ? 2 : 3;
    case Rule::Romanian:
      if (n == 1) return 0;
      return n == 0 || (mod100 >= 1 && mod100 <= 19) ? 1 : 2;
    case Rule::Arabic:
      if (n <= 2) return static_cast<uint8_t>(n);
      if (mod100 >= 3 && mod100 <= 10) return 3;
      return mod100 >= 11 ? 4 : 5;
    case Rule::Hebrew:
      return n <= 2 && n >= 1 ? static_cast<uint8_t>(n - 1) : 2;
  }
  return 0;
}

}  // namespace plural
