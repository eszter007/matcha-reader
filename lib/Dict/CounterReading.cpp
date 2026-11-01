#include "CounterReading.h"

#include <algorithm>
#include <cstring>

namespace CounterReading {

namespace {

// The last spoken element of a number decides how the counter after it sounds: 1本 いっぽん,
// 3本 さんぼん, 100本 ひゃっぽん, 3000本 さんぜんぼん. Elements are the units digit, or 十, 百,
// 千, 万 (億 and 兆 count as 万) when the number ends in a round one.
enum Element : uint8_t { E1 = 1, E2, E3, E4, E5, E6, E7, E8, E9, E10, E100, E1000, E10000 };

// Elements after which the counter takes its geminated form (いっ + ぽん).
constexpr uint8_t G1 = 1, G6 = 2, G8 = 4, G10 = 8, G100 = 16;
constexpr uint8_t GEM_ALL = G1 | G6 | G8 | G10 | G100;
constexpr uint8_t GEM_1810 = G1 | G8 | G10;
// Elements after which it takes its voiced form (さん + ぼん).
constexpr uint8_t V3 = 1, V4 = 2, V1000 = 4, V10000 = 8;
constexpr uint8_t VOICE_ALL = V3 | V1000 | V10000;
// Readings of 4, 7 and 9 that some counters take instead of よん, なな, きゅう.
constexpr uint8_t YO4 = 1, SHI4 = 2, SHICHI7 = 4, KU9 = 8;
constexpr uint8_t CLOCK = YO4 | SHICHI7 | KU9;

struct Exact {
  uint8_t number;
  const char* reading;  // whole number + counter
};

struct Counter {
  const char* text;
  const char* plain;   // after any other element
  const char* gem;     // after a geminating element, or nullptr
  uint8_t gemMask;
  const char* voiced;  // after a voicing element, or nullptr
  uint8_t voicedMask;
  uint8_t variants;
  const Exact* exact;  // whole-number exceptions, nullptr-terminated, or nullptr
};

constexpr const char* kDigit[10] = {"", "いち", "に", "さん", "よん", "ご", "ろく", "なな", "はち", "きゅう"};

constexpr Exact kNichi[] = {{2, "ふつか"},         {3, "みっか"},   {4, "よっか"},   {5, "いつか"},
                            {6, "むいか"},         {7, "なのか"},   {8, "ようか"},   {9, "ここのか"},
                            {10, "とおか"},        {14, "じゅうよっか"}, {20, "はつか"},
                            {24, "にじゅうよっか"}, {0, nullptr}};
constexpr Exact kNin[] = {{1, "ひとり"}, {2, "ふたり"}, {0, nullptr}};
constexpr Exact kSai[] = {{20, "はたち"}, {0, nullptr}};
constexpr Exact kTsu[] = {{1, "ひとつ"}, {2, "ふたつ"}, {3, "みっつ"}, {4, "よっつ"},   {5, "いつつ"},
                          {6, "むっつ"}, {7, "ななつ"}, {8, "やっつ"}, {9, "ここのつ"}, {10, "とお"},
                          {0, nullptr}};
constexpr Exact kNinmae[] = {{1, "いちにんまえ"}, {2, "ににんまえ"}, {0, nullptr}};

// Native counters count 1 and 2 with ひと and ふた.
#define WAGO(name, r)                                               \
  constexpr Exact name[] = {{1, "ひと" r}, {2, "ふた" r}, {0, nullptr}}
WAGO(kHako, "はこ");
WAGO(kTaba, "たば");
WAGO(kKuchi, "くち");
WAGO(kKumi, "くみ");
WAGO(kBan, "ばん");
WAGO(kMa, "ま");
WAGO(kFukuro, "ふくろ");
WAGO(kKire, "きれ");
WAGO(kSara, "さら");
WAGO(kHeya, "へや");
WAGO(kKoto, "こと");
WAGO(kTsubu, "つぶ");
WAGO(kMakuri, "まく");
#undef WAGO

// Longer texts first where one is a prefix of another (日間 before 日), so the first hit is the
// longest.
constexpr Counter kCounters[] = {
    // Dates and time
    {"日間", "にちかん", nullptr, 0, nullptr, 0, 0, nullptr},  // read via the 日 table, see read()
    {"日", "にち", nullptr, 0, nullptr, 0, 0, kNichi},
    {"月", "がつ", nullptr, 0, nullptr, 0, SHI4 | SHICHI7 | KU9, nullptr},
    {"ヶ月", "かげつ", "かげつ", GEM_ALL, nullptr, 0, 0, nullptr},
    {"か月", "かげつ", "かげつ", GEM_ALL, nullptr, 0, 0, nullptr},
    {"カ月", "かげつ", "かげつ", GEM_ALL, nullptr, 0, 0, nullptr},
    {"ヵ月", "かげつ", "かげつ", GEM_ALL, nullptr, 0, 0, nullptr},
    {"箇月", "かげつ", "かげつ", GEM_ALL, nullptr, 0, 0, nullptr},
    {"年間", "ねんかん", nullptr, 0, nullptr, 0, YO4, nullptr},
    {"年生", "ねんせい", nullptr, 0, nullptr, 0, YO4, nullptr},
    {"年", "ねん", nullptr, 0, nullptr, 0, YO4, nullptr},
    {"時間", "じかん", nullptr, 0, nullptr, 0, CLOCK, nullptr},
    {"時半", "じはん", nullptr, 0, nullptr, 0, CLOCK, nullptr},
    {"時限", "じげん", nullptr, 0, nullptr, 0, CLOCK, nullptr},
    {"時", "じ", nullptr, 0, nullptr, 0, CLOCK, nullptr},
    {"分間", "ふんかん", "ぷんかん", GEM_ALL, "ぷんかん", V3 | V4, 0, nullptr},
    {"分", "ふん", "ぷん", GEM_ALL, "ぷん", V3 | V4, 0, nullptr},
    {"秒", "びょう", nullptr, 0, nullptr, 0, 0, nullptr},
    {"週間", "しゅうかん", "しゅうかん", GEM_1810, nullptr, 0, 0, nullptr},
    {"週", "しゅう", "しゅう", GEM_1810, nullptr, 0, 0, nullptr},
    {"世紀", "せいき", "せいき", GEM_1810, nullptr, 0, 0, nullptr},
    {"周年", "しゅうねん", "しゅうねん", GEM_1810, nullptr, 0, 0, nullptr},
    {"学期", "がっき", nullptr, 0, nullptr, 0, 0, nullptr},
    {"代", "だい", nullptr, 0, nullptr, 0, 0, nullptr},
    {"期", "き", "き", GEM_1810, nullptr, 0, 0, nullptr},
    // People and age
    {"人前", "にんまえ", nullptr, 0, nullptr, 0, YO4, kNinmae},
    {"人", "にん", nullptr, 0, nullptr, 0, YO4, kNin},
    {"名", "めい", nullptr, 0, nullptr, 0, 0, nullptr},
    {"歳", "さい", "さい", GEM_1810, nullptr, 0, 0, kSai},
    {"才", "さい", "さい", GEM_1810, nullptr, 0, 0, kSai},
    // Things and animals
    {"つ", "つ", nullptr, 0, nullptr, 0, 0, kTsu},
    {"個", "こ", "こ", GEM_ALL, nullptr, 0, 0, nullptr},
    {"箇", "こ", "こ", GEM_ALL, nullptr, 0, 0, nullptr},
    {"本", "ほん", "ぽん", GEM_ALL, "ぼん", VOICE_ALL, 0, nullptr},
    {"枚", "まい", nullptr, 0, nullptr, 0, 0, nullptr},
    {"冊", "さつ", "さつ", GEM_1810, nullptr, 0, 0, nullptr},
    {"匹", "ひき", "ぴき", GEM_ALL, "びき", VOICE_ALL, 0, nullptr},
    {"頭", "とう", "とう", GEM_1810, nullptr, 0, 0, nullptr},
    {"羽", "わ", "ぱ", G6 | G8 | G10 | G100, "ば", V3, 0, nullptr},
    {"杯", "はい", "ぱい", GEM_ALL, "ばい", VOICE_ALL, 0, nullptr},
    {"台", "だい", nullptr, 0, nullptr, 0, 0, nullptr},
    {"隻", "せき", "せき", GEM_1810, nullptr, 0, 0, nullptr},
    {"着", "ちゃく", "ちゃく", GEM_1810, nullptr, 0, 0, nullptr},
    {"足", "そく", "そく", GEM_1810, "ぞく", V3 | V1000, 0, nullptr},
    {"粒", "つぶ", nullptr, 0, nullptr, 0, 0, kTsubu},
    {"箱", "はこ", nullptr, 0, nullptr, 0, 0, kHako},
    {"束", "たば", nullptr, 0, nullptr, 0, 0, kTaba},
    {"袋", "ふくろ", nullptr, 0, nullptr, 0, 0, kFukuro},
    {"切れ", "きれ", nullptr, 0, nullptr, 0, 0, kKire},
    {"皿", "さら", nullptr, 0, nullptr, 0, 0, kSara},
    {"品", "ひん", "ぴん", GEM_ALL, nullptr, 0, 0, nullptr},
    {"点", "てん", "てん", GEM_1810, nullptr, 0, 0, nullptr},
    {"発", "はつ", "ぱつ", GEM_ALL, "ぱつ", V3 | V4, 0, nullptr},
    {"曲", "きょく", "きょく", GEM_ALL, nullptr, 0, 0, nullptr},
    {"通", "つう", "つう", GEM_1810, nullptr, 0, 0, nullptr},
    {"票", "ひょう", "ぴょう", GEM_ALL, "びょう", V3, 0, nullptr},
    {"筆", "ひつ", "ぴつ", GEM_ALL, "ぴつ", V3, 0, nullptr},
    {"口", "くち", nullptr, 0, nullptr, 0, 0, kKuchi},
    {"組", "くみ", nullptr, 0, nullptr, 0, 0, kKumi},
    {"言", "こと", nullptr, 0, nullptr, 0, 0, kKoto},
    {"幕", "まく", nullptr, 0, nullptr, 0, 0, kMakuri},
    // Places and buildings
    {"部屋", "へや", nullptr, 0, nullptr, 0, 0, kHeya},
    {"階", "かい", "かい", GEM_ALL, "がい", VOICE_ALL, 0, nullptr},
    {"軒", "けん", "けん", GEM_ALL, "げん", VOICE_ALL, 0, nullptr},
    {"棟", "とう", "とう", GEM_1810, nullptr, 0, 0, nullptr},
    {"戸", "こ", "こ", GEM_ALL, nullptr, 0, 0, nullptr},
    {"間", "ま", nullptr, 0, nullptr, 0, 0, kMa},
    {"畳", "じょう", nullptr, 0, nullptr, 0, YO4, nullptr},
    {"坪", "つぼ", nullptr, 0, nullptr, 0, 0, nullptr},
    {"晩", "ばん", nullptr, 0, nullptr, 0, 0, kBan},
    {"泊", "はく", "ぱく", GEM_ALL, "ぱく", V3 | V4, 0, nullptr},
    {"席", "せき", "せき", GEM_1810, nullptr, 0, 0, nullptr},
    {"社", "しゃ", "しゃ", G1 | G8 | G10 | G100, nullptr, 0, 0, nullptr},
    // Order, frequency, text
    {"回", "かい", "かい", GEM_ALL, nullptr, 0, 0, nullptr},
    {"度", "ど", nullptr, 0, nullptr, 0, 0, nullptr},
    {"倍", "ばい", nullptr, 0, nullptr, 0, 0, nullptr},
    {"番", "ばん", nullptr, 0, nullptr, 0, 0, nullptr},
    {"号", "ごう", nullptr, 0, nullptr, 0, 0, nullptr},
    {"件", "けん", "けん", GEM_ALL, nullptr, 0, 0, nullptr},
    {"歩", "ほ", "ぽ", GEM_ALL, "ぽ", V3 | V4, 0, nullptr},
    {"段", "だん", nullptr, 0, nullptr, 0, 0, nullptr},
    {"列", "れつ", nullptr, 0, nullptr, 0, 0, nullptr},
    {"行", "ぎょう", nullptr, 0, nullptr, 0, 0, nullptr},
    {"文字", "もじ", nullptr, 0, nullptr, 0, 0, nullptr},
    {"字", "じ", nullptr, 0, nullptr, 0, YO4, nullptr},
    {"語", "ご", nullptr, 0, nullptr, 0, 0, nullptr},
    {"問", "もん", nullptr, 0, nullptr, 0, 0, nullptr},
    {"円", "えん", nullptr, 0, nullptr, 0, YO4, nullptr},
    // Units (katakana stays katakana)
    {"センチメートル", "センチメートル", "センチメートル", GEM_1810, nullptr, 0, 0, nullptr},
    {"センチ", "センチ", "センチ", GEM_1810, nullptr, 0, 0, nullptr},
    {"ミリメートル", "ミリメートル", nullptr, 0, nullptr, 0, 0, nullptr},
    {"ミリリットル", "ミリリットル", nullptr, 0, nullptr, 0, 0, nullptr},
    {"ミリ", "ミリ", nullptr, 0, nullptr, 0, 0, nullptr},
    {"キロメートル", "キロメートル", nullptr, 0, nullptr, 0, 0, nullptr},
    {"キログラム", "キログラム", nullptr, 0, nullptr, 0, 0, nullptr},
    {"キロ", "キロ", nullptr, 0, nullptr, 0, 0, nullptr},
    {"メートル", "メートル", nullptr, 0, nullptr, 0, 0, nullptr},
    {"グラム", "グラム", nullptr, 0, nullptr, 0, 0, nullptr},
    {"リットル", "リットル", nullptr, 0, nullptr, 0, 0, nullptr},
    {"トン", "トン", nullptr, 0, nullptr, 0, 0, nullptr},
    {"ドル", "ドル", nullptr, 0, nullptr, 0, 0, nullptr},
    {"ページ", "ページ", "ページ", GEM_1810, nullptr, 0, 0, nullptr},
    {"パーセント", "パーセント", "パーセント", GEM_1810, nullptr, 0, 0, nullptr},
    {"ヘクタール", "ヘクタール", "ヘクタール", GEM_1810, nullptr, 0, 0, nullptr},
};

// Latin-script units and the counter they are read as. Longer first (mm before m).
struct Unit {
  const char* text;
  const char* counter;
};
constexpr Unit kUnits[] = {
    {"km", "キロメートル"}, {"kg", "キログラム"}, {"cm", "センチメートル"}, {"mm", "ミリメートル"},
    {"ml", "ミリリットル"}, {"m", "メートル"},    {"g", "グラム"},          {"%", "パーセント"},
    {"％", "パーセント"},   {"℃", "度"},          {"ｋｍ", "キロメートル"}, {"ｋｇ", "キログラム"},
    {"ｃｍ", "センチメートル"},
};

struct Multiplier {
  const char* text;
  uint64_t value;
};
constexpr Multiplier kMultipliers[] = {{"兆", 1000000000000ull}, {"億", 100000000ull}, {"万", 10000}, {"千", 1000}};

constexpr char kMe[] = "目";
constexpr uint64_t kMaxNumber = 9999999999999999ull;  // below 1京

bool startsWith(const std::string_view s, const char* p) {
  const size_t n = std::strlen(p);
  return s.size() >= n && s.compare(0, n, p) == 0;
}

// Drops the last kana (3 bytes) and adds っ: いち -> いっ, じゅう -> じゅっ, ひゃく -> ひゃっ.
void geminate(std::string& s) {
  if (s.size() >= 3) s.resize(s.size() - 3);
  s += "っ";
}

// Reading of n split into everything before the last spoken element and that element.
void split(uint64_t n, std::string& prefix, std::string& tail, Element& key);

void readNumber(const uint64_t n, std::string& out) {
  if (n == 0) return;
  std::string prefix, tail;
  Element key;
  split(n, prefix, tail, key);
  out += prefix;
  out += tail;
}

void split(const uint64_t n, std::string& prefix, std::string& tail, Element& key) {
  constexpr struct {
    uint64_t value;
    const char* reading;
  } kBig[] = {{1000000000000ull, "ちょう"}, {100000000ull, "おく"}, {10000, "まん"}};
  const uint32_t low = static_cast<uint32_t>(n % 10000);
  if (low == 0) {
    // Ends on a big unit: 5万 -> ご + まん.
    for (const auto& big : kBig) {
      if (n % big.value != 0 || n < big.value) continue;
      const uint64_t group = n / big.value % 10000;
      if (group == 0) continue;
      readNumber(n - group * big.value, prefix);
      std::string g;
      readNumber(group, g);
      // 1兆 いっちょう, 8兆 はっちょう, 10兆 じゅっちょう.
      if (big.value == 1000000000000ull && (group % 10 == 1 || group % 10 == 8 || group % 100 == 10)) geminate(g);
      prefix += g;
      tail = big.reading;
      key = E10000;
      return;
    }
  }
  // Groups above the lowest are read plainly, each with its unit.
  if (n >= 10000) {
    for (const auto& big : kBig) {
      const uint64_t group = n / big.value % 10000;
      if (group == 0) continue;
      std::string g;
      readNumber(group, g);
      if (big.value == 1000000000000ull && (group % 10 == 1 || group % 10 == 8 || group % 100 == 10)) geminate(g);
      prefix += g;
      prefix += big.reading;
    }
  }
  const uint32_t th = low / 1000, h = low / 100 % 10, t = low / 10 % 10, u = low % 10;
  // Everything in the low group above the deciding element, read plainly.
  const auto above = [&](const uint32_t keep) {
    const uint32_t thk = keep / 1000, hk = keep / 100 % 10, tk = keep / 10 % 10;
    if (thk == 1) prefix += "せん";
    else if (thk == 3) prefix += "さんぜん";
    else if (thk == 8) prefix += "はっせん";
    else if (thk) (prefix += kDigit[thk]) += "せん";
    if (hk == 1) prefix += "ひゃく";
    else if (hk == 3) prefix += "さんびゃく";
    else if (hk == 6) prefix += "ろっぴゃく";
    else if (hk == 8) prefix += "はっぴゃく";
    else if (hk) (prefix += kDigit[hk]) += "ひゃく";
    if (tk == 1) prefix += "じゅう";
    else if (tk) (prefix += kDigit[tk]) += "じゅう";
  };
  if (u) {
    above(low - u);
    tail = kDigit[u];
    key = static_cast<Element>(u);
  } else if (t) {
    above(low - t * 10);
    if (t > 1) prefix += kDigit[t];
    tail = "じゅう";
    key = E10;
  } else if (h) {
    above(low - h * 100);
    if (h == 3) {
      prefix += "さん";
      tail = "びゃく";
    } else if (h == 6) {
      prefix += "ろっ";
      tail = "ぴゃく";
    } else if (h == 8) {
      prefix += "はっ";
      tail = "ぴゃく";
    } else {
      if (h > 1) prefix += kDigit[h];
      tail = "ひゃく";
    }
    key = E100;
  } else {
    if (th == 3) {
      prefix += "さん";
      tail = "ぜん";
    } else if (th == 8) {
      prefix += "はっ";
      tail = "せん";
    } else {
      if (th > 1) prefix += kDigit[th];
      tail = "せん";
    }
    key = E1000;
  }
}

uint8_t gemBit(const Element key) {
  switch (key) {
    case E1:
      return G1;
    case E6:
      return G6;
    case E8:
      return G8;
    case E10:
      return G10;
    case E100:
      return G100;
    default:
      return 0;
  }
}

uint8_t voiceBit(const Element key) {
  switch (key) {
    case E3:
      return V3;
    case E4:
      return V4;
    case E1000:
      return V1000;
    case E10000:
      return V10000;
    default:
      return 0;
  }
}

const Counter* findCounter(const std::string_view text) {
  for (const Counter& c : kCounters) {
    if (text == c.text) return &c;
  }
  return nullptr;
}

const Exact* findExact(const Exact* table, const uint64_t number) {
  if (!table) return nullptr;
  for (const Exact* e = table; e->reading; e++) {
    if (e->number == number) return e;
  }
  return nullptr;
}

// Reading of number + counter, without 目 or multipliers.
bool readCounter(const uint64_t number, const Counter& c, std::string& out) {
  if (const Exact* e = findExact(c.exact, number)) {
    out += e->reading;
    return true;
  }
  std::string prefix, tail;
  Element key;
  split(number, prefix, tail, key);
  if (key == E4 && (c.variants & YO4)) tail = "よ";
  if (key == E4 && (c.variants & SHI4)) tail = "し";
  if (key == E7 && (c.variants & SHICHI7)) tail = "しち";
  if (key == E9 && (c.variants & KU9)) tail = "く";
  out += prefix;
  if (c.gem && (c.gemMask & gemBit(key))) {
    geminate(tail);
    out += tail;
    out += c.gem;
  } else if (c.voiced && (c.voicedMask & voiceBit(key))) {
    out += tail;
    out += c.voiced;
  } else {
    out += tail;
    out += c.plain;
  }
  return true;
}

}  // namespace

bool parseNumber(const std::string_view digits, uint64_t& out) {
  out = 0;
  if (digits.empty()) return false;
  for (size_t i = 0; i < digits.size();) {
    uint64_t d;
    const auto c = static_cast<unsigned char>(digits[i]);
    if (c >= '0' && c <= '9') {
      d = c - '0';
      i += 1;
    } else if (c == 0xEF && i + 2 < digits.size() && static_cast<unsigned char>(digits[i + 1]) == 0xBC &&
               static_cast<unsigned char>(digits[i + 2]) >= 0x90 && static_cast<unsigned char>(digits[i + 2]) <= 0x99) {
      d = static_cast<unsigned char>(digits[i + 2]) - 0x90;  // full-width ０-９
      i += 3;
    } else {
      return false;
    }
    out = out * 10 + d;
    if (out > kMaxNumber) return false;
  }
  return true;
}

size_t matchCounter(const std::string_view text) {
  size_t at = 0;
  for (const Multiplier& m : kMultipliers) {
    if (startsWith(text, m.text)) {
      at = std::strlen(m.text);
      break;
    }
  }
  const std::string_view rest = text.substr(at);
  size_t best = 0;
  for (const Counter& c : kCounters) {
    if (startsWith(rest, c.text)) best = std::max(best, std::strlen(c.text));
  }
  for (const Unit& u : kUnits) {
    if (startsWith(rest, u.text)) best = std::max(best, std::strlen(u.text));
  }
  if (best == 0) return at;  // a multiplier alone (3万) is still a number
  at += best;
  if (startsWith(text.substr(at), kMe)) at += sizeof(kMe) - 1;
  return at;
}

size_t multiplierLength(const std::string_view text) {
  for (const Multiplier& m : kMultipliers) {
    if (startsWith(text, m.text)) return std::strlen(m.text);
  }
  return 0;
}

const char* plainReading(std::string_view counter) {
  counter.remove_prefix(multiplierLength(counter));
  if (counter.size() > sizeof(kMe) - 1 && counter.substr(counter.size() - (sizeof(kMe) - 1)) == kMe) {
    counter.remove_suffix(sizeof(kMe) - 1);
  }
  if (const char* word = unitWord(counter)) counter = word;
  // 日's dictionary entry read にち is the noun "Sunday"; its counter sense is read か.
  if (counter == "日" || counter == "日間") return nullptr;
  const Counter* c = findCounter(counter);
  return c ? c->plain : nullptr;
}

const char* unitWord(const std::string_view counter) {
  for (const Unit& u : kUnits) {
    if (counter == u.text) return u.counter;
  }
  return nullptr;
}

bool read(uint64_t number, std::string_view counter, std::string& out) {
  out.clear();
  if (number == 0 || number > kMaxNumber) return false;
  for (const Multiplier& m : kMultipliers) {
    if (startsWith(counter, m.text)) {
      if (number > kMaxNumber / m.value) return false;
      number *= m.value;
      counter.remove_prefix(std::strlen(m.text));
      break;
    }
  }
  bool ordinal = false;  // 目: 3日目, 2回目
  if (counter.size() > sizeof(kMe) - 1 && counter.substr(counter.size() - (sizeof(kMe) - 1)) == kMe) {
    ordinal = true;
    counter.remove_suffix(sizeof(kMe) - 1);
  }
  if (const char* word = unitWord(counter)) counter = word;
  if (counter.empty()) {
    readNumber(number, out);
    return !out.empty();
  }
  const Counter* c = findCounter(counter);
  if (!c) return false;

  if (counter == "日" || counter == "日間") {
    // 1日 is the 1st (ついたち) or one day (いちにち); counted or ordinal days are いちにち.
    if (number == 1) {
      out = ordinal || counter == "日間" ? "いちにち" : "ついたち・いちにち";
    } else {
      readCounter(number, *findCounter("日"), out);
    }
    if (counter == "日間") out += "かん";
  } else {
    readCounter(number, *c, out);
  }
  if (ordinal) out += "め";
  return true;
}

}  // namespace CounterReading
