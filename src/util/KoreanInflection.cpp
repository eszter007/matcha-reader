#include "KoreanInflection.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace KoreanInflection {
namespace {

using Word = std::u32string;

// Hangul syllable = 0xAC00 + (initial * 21 + vowel) * 28 + final.
constexpr char32_t kBase = 0xAC00;
constexpr char32_t kLast = 0xD7A3;
// Vowel indices.
enum : int {
  A = 0,
  AE = 1,
  EO = 4,
  E = 5,
  YEO = 6,
  O = 8,
  WA = 9,
  WAE = 10,
  OE = 11,
  U = 13,
  WO = 14,
  EU = 18,
  I = 20
};
// Final consonant indices.
enum : int { NONE = 0, F_N = 4, F_D = 7, F_L = 8, F_M = 16, F_B = 17, F_S = 19, F_SS = 20, F_H = 27 };
// Initial consonant indices.
enum : int { I_L = 5, I_NG = 11, I_H = 18 };

bool hangul(const char32_t c) { return c >= kBase && c <= kLast; }
int initialOf(const char32_t c) { return static_cast<int>((c - kBase) / 588); }
int vowelOf(const char32_t c) { return static_cast<int>(((c - kBase) % 588) / 28); }
int finalOf(const char32_t c) { return static_cast<int>((c - kBase) % 28); }
char32_t compose(const int l, const int v, const int t) { return kBase + static_cast<char32_t>((l * 21 + v) * 28 + t); }
char32_t withFinal(const char32_t c, const int t) { return compose(initialOf(c), vowelOf(c), t); }

Word decode(const std::string& s) {
  Word out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (i + n > s.size()) break;
    char32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
    for (size_t k = 1; k < n; k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

std::string encode(const Word& w) {
  std::string out;
  out.reserve(w.size() * 3);
  for (const char32_t cp : w) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

bool endsWith(const Word& w, const Word& suffix) {
  return w.size() > suffix.size() && w.compare(w.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct Out {
  const std::string& surface;
  std::vector<std::string>& list;
  void add(const Word& w) {
    if (list.size() >= MAX_VARIANTS || w.empty()) return;
    std::string s = encode(w);
    if (s == surface || std::find(list.begin(), list.end(), s) != list.end()) return;
    list.push_back(std::move(s));
  }
  bool full() const { return list.size() >= MAX_VARIANTS; }
};

// Noun particles, longest first within each group. `afterConsonant`/`afterVowel` say which
// final the noun before them may have (0 either, 1 consonant only, 2 vowel only, 3 vowel or ㄹ).
struct Particle {
  const char* text;
  uint8_t after;
};
// clang-format off
constexpr Particle kParticles[] = {
    {"에서부터", 0}, {"에게서", 0}, {"한테서", 0}, {"으로부터", 1}, {"로부터", 3}, {"으로서", 1}, {"으로써", 1},
    {"로서", 3}, {"로써", 3}, {"께서는", 0}, {"께서", 0}, {"에서는", 0}, {"에서도", 0}, {"에서", 0}, {"에게는", 0},
    {"에게", 0}, {"한테", 0}, {"까지는", 0}, {"까지도", 0}, {"까지", 0}, {"부터는", 0}, {"부터", 0}, {"처럼", 0},
    {"같이", 0}, {"보다는", 0}, {"보다", 0}, {"마다", 0}, {"조차", 0}, {"마저", 0}, {"밖에", 0}, {"이라도", 1},
    {"라도", 2}, {"이나마", 1}, {"이나", 1}, {"이랑", 1}, {"랑", 2}, {"하고", 0}, {"으로는", 1}, {"으로", 1},
    {"로는", 3}, {"로", 3}, {"에는", 0}, {"에도", 0}, {"에", 0}, {"께", 0}, {"은", 1}, {"는", 2}, {"이", 1},
    {"가", 2}, {"을", 1}, {"를", 2}, {"과", 1}, {"와", 2}, {"의", 0}, {"도", 0}, {"만", 0}, {"나", 2}, {"요", 0},
    {"들", 0}, {"들이", 0}, {"들은", 0}, {"들을", 0}, {"들의", 0}, {"들도", 0}, {"들에게", 0},
};
// The copula 이다 after a noun: 학생이에요, 의사예요, 학생입니다. `after` as above.
constexpr Particle kCopula[] = {
    {"이었습니다", 1}, {"였습니다", 2}, {"이었어요", 1}, {"였어요", 2}, {"이었다", 1}, {"였다", 2}, {"이에요", 1},
    {"예요", 2}, {"입니다", 0}, {"입니까", 0}, {"이세요", 1}, {"이시다", 1}, {"이라서", 1}, {"라서", 2},
    {"이라고", 1}, {"라고", 2}, {"이지만", 1}, {"지만", 2}, {"인데요", 0}, {"인데", 0}, {"이니까", 1},
    {"이면", 1}, {"이고", 1}, {"이야", 1}, {"야", 2}, {"이다", 1}, {"인", 0}, {"이지", 1}, {"이죠", 1}, {"죠", 2},
};
// Predicate endings that follow the stem as written. `after`: 0 any stem, 1 stem ending in a
// consonant (the 으 forms), 2 stem ending in a vowel.
constexpr Particle kEndings[] = {
    {"었었습니다", 0}, {"습니다", 1}, {"습니까", 1}, {"습시다", 1}, {"는답니다", 0}, {"답니다", 0}, {"는다고", 0},
    {"다고", 0}, {"냐고", 0}, {"자고", 0}, {"는다", 1}, {"다", 0}, {"는데요", 0}, {"는데", 0}, {"은데요", 1},
    {"은데", 1}, {"지요", 0}, {"죠", 0}, {"지만", 0}, {"지", 0}, {"네요", 0}, {"네", 0}, {"군요", 0}, {"군", 0},
    {"구나", 0}, {"고요", 0}, {"고서", 0}, {"고", 0}, {"게요", 0}, {"게", 0}, {"도록", 0}, {"거나", 0}, {"든지", 0},
    {"다가", 0}, {"자마자", 0}, {"기에", 0}, {"기로", 0}, {"기도", 0}, {"기는", 0}, {"기를", 0}, {"기가", 0},
    {"기", 0}, {"냐", 0}, {"니", 0}, {"자", 0}, {"으세요", 1}, {"세요", 2}, {"으십시오", 1}, {"십시오", 2},
    {"으시오", 1}, {"시오", 2}, {"으면서", 1}, {"면서", 2}, {"으며", 1}, {"며", 2}, {"으면", 1}, {"면", 2},
    {"으니까", 1}, {"니까", 2}, {"으니", 1}, {"으러", 1}, {"러", 2}, {"으려고", 1}, {"려고", 2}, {"으려면", 1},
    {"려면", 2}, {"던데", 0}, {"던", 0}, {"더니", 0}, {"는지", 0}, {"은지", 1}, {"는가", 0}, {"은가", 1},
    {"는군요", 0}, {"잖아요", 0}, {"잖아", 0}, {"거든요", 0}, {"거든", 0}, {"는", 0}, {"은", 1}, {"을", 1},
    {"음", 1}, {"을게요", 1}, {"을게", 1}, {"을래요", 1}, {"을래", 1}, {"을까요", 1}, {"을까", 1}, {"을수록", 1},
    {"을지", 1}, {"을지도", 1},
};
// Endings whose first sound is a final consonant on the stem's last syllable: 간 (가+ㄴ),
// 갈게 (가+ㄹ게), 갑니다 (가+ㅂ니다), 감 (가+ㅁ).
struct JamoEnding {
  int final;
  const char* rest;
};
constexpr JamoEnding kJamoEndings[] = {
    {F_B, "니다"}, {F_B, "니까"}, {F_B, "시다"}, {F_L, "게요"}, {F_L, "게"}, {F_L, "래요"}, {F_L, "래"},
    {F_L, "까요"}, {F_L, "까"}, {F_L, "수록"}, {F_L, "지도"}, {F_L, "지"}, {F_N, "데요"}, {F_N, "데"},
    {F_N, "지"}, {F_N, "가"}, {F_N, "다고"}, {F_N, "다"}, {F_N, ""}, {F_L, ""}, {F_M, ""},
};
// After the 어/아 vowel of a stem: 먹어, 먹어요, 먹어서, 먹어도, 먹어야, 먹어라, 먹어 보다.
constexpr const char* kAfterVowel[] = {"요", "서", "도", "야", "야지", "라", "다가", "줘", "주세요", "버리다", ""};
// clang-format on

void addPredicate(Out& out, const Word& stem, int depth);

// The stem whose last syllable + 어/아 contracted into `c` (가+아 → 가, 오+아 → 와, 주+어 → 줘,
// 되+어 → 돼, 하+여 → 해, 시+어 → 셔, 쓰+어 → 써). Each result replaces the last syllable of
// `before + c`; ㅂ, 르 and ㅎ irregulars also change the syllable before it.
void addContracted(Out& out, const Word& before, const char32_t c, const int depth) {
  if (!hangul(c) || finalOf(c) != NONE) return;
  const int v = vowelOf(c);
  const int l = initialOf(c);
  const auto stemWith = [&](const char32_t last) {
    Word s = before;
    s.push_back(last);
    addPredicate(out, s, depth);
  };
  switch (v) {
    case A:
      stemWith(c);                     // 가 (가다)
      stemWith(compose(l, EU, NONE));  // 바빠 → 바쁘
      if (l == I_L && !before.empty() && hangul(before.back()) && finalOf(before.back()) == F_L) {
        Word s = before;  // 몰라 → 모르
        s.back() = withFinal(s.back(), NONE);
        s.push_back(compose(I_L, EU, NONE));
        addPredicate(out, s, depth);
      }
      break;
    case EO:
      stemWith(c);                     // 서 (서다)
      stemWith(compose(l, EU, NONE));  // 써 → 쓰, 커 → 크
      if (l == I_L && !before.empty() && hangul(before.back()) && finalOf(before.back()) == F_L) {
        Word s = before;  // 불러 → 부르
        s.back() = withFinal(s.back(), NONE);
        s.push_back(compose(I_L, EU, NONE));
        addPredicate(out, s, depth);
      }
      break;
    case WA:
      stemWith(compose(l, O, NONE));  // 와 → 오, 봐 → 보
      if (l == I_NG && !before.empty() && hangul(before.back())) {
        Word s = before;  // 도와 → 돕
        s.back() = withFinal(s.back(), F_B);
        addPredicate(out, s, depth);
      }
      break;
    case WO:
      stemWith(compose(l, U, NONE));  // 줘 → 주, 배워 → 배우
      if (l == I_NG && !before.empty() && hangul(before.back())) {
        Word s = before;  // 더워 → 덥
        s.back() = withFinal(s.back(), F_B);
        addPredicate(out, s, depth);
      }
      break;
    case WAE:
      stemWith(compose(l, OE, NONE));  // 돼 → 되
      break;
    case YEO:
      stemWith(compose(l, I, NONE));  // 셔 → 시, 려 → 리, 쳐 → 치
      break;
    case AE:
      if (l == I_H) stemWith(compose(I_H, A, NONE));  // 해 → 하
      stemWith(c);                                    // 내 (내다), 보내
      stemWith(compose(l, EO, F_H));                  // 그래 → 그렇
      stemWith(compose(l, A, F_H));                   // 빨개 → 빨갛
      break;
    case E:
      stemWith(c);
      break;
    default:
      break;
  }
}

// `stem` is everything before a predicate ending; emits stem+다 and peels tense, honorific and
// the 하다 verb's noun.
void addPredicate(Out& out, const Word& stem, const int depth) {
  if (stem.empty() || out.full() || depth > 3 || !hangul(stem.back())) return;
  Word dict = stem;
  dict.push_back(U'다');
  out.add(dict);
  const char32_t last = stem.back();
  const Word before(stem.begin(), stem.end() - 1);
  // 겠 (future/conjecture), 시 (honorific): drop and keep going.
  if (last == U'겠' || last == U'시') addPredicate(out, before, depth + 1);
  // Past: 었/았/였 written out (먹었 → 먹, 하였 → 하), or ㅆ on a contracted syllable (갔 → 가,
  // 했 → 하, 셨 → 시, 봤 → 보).
  if (last == U'었' || last == U'았' || last == U'였') {
    addPredicate(out, before, depth + 1);
  } else if (finalOf(last) == F_SS) {
    addContracted(out, before, withFinal(last, NONE), depth + 1);
  }
  // 공부하 → 공부: the noun of a 하다 verb.
  if (last == U'하' && stem.size() > 1) out.add(before);
}

// The stem before a jamo ending (간 → 가, 산 → 살 by ㄹ dropping, 더운 → 덥, 그런 → 그렇).
void addJamoStem(Out& out, const Word& w) {
  for (const auto& e : kJamoEndings) {
    const Word rest = decode(e.rest);
    if (w.size() < rest.size() + 1) continue;
    if (!rest.empty() && w.compare(w.size() - rest.size(), rest.size(), rest) != 0) continue;
    const size_t at = w.size() - rest.size() - 1;
    const char32_t c = w[at];
    if (!hangul(c) || finalOf(c) != e.final) continue;
    Word stem(w.begin(), w.begin() + static_cast<long>(at));
    const char32_t open = withFinal(c, NONE);
    stem.push_back(open);
    addPredicate(out, stem, 0);
    stem.back() = withFinal(open, F_L);  // ㄹ dropped before ㄴ ㅂ ㅅ: 사는/삽니다 → 살
    addPredicate(out, stem, 0);
    if (vowelOf(open) == U && initialOf(open) == I_NG && at > 0) {  // 더운 → 덥
      Word s(w.begin(), w.begin() + static_cast<long>(at));
      s.back() = withFinal(s.back(), F_B);
      addPredicate(out, s, 0);
    }
    if (vowelOf(open) == EO || vowelOf(open) == A) {  // 그런 → 그렇
      stem.back() = withFinal(open, F_H);
      addPredicate(out, stem, 0);
    }
    if (out.full()) return;
  }
}

// 어/아 forms: 먹어(요), 가(요) (contracted), 들어 → 듣, 나아 → 낫.
void addVowelForms(Out& out, const Word& w) {
  for (const char* tail : kAfterVowel) {
    const Word t = decode(tail);
    if (!t.empty() && !endsWith(w, t)) continue;
    const Word r(w.begin(), w.end() - static_cast<long>(t.size()));
    if (r.empty()) continue;
    const char32_t c = r.back();
    const Word before(r.begin(), r.end() - 1);
    if ((c == U'어' || c == U'아' || c == U'여') && r.size() > 1) {
      addPredicate(out, before, 0);  // 먹어 → 먹, 하여 → 하
      if (hangul(before.back())) {
        Word s = before;
        if (finalOf(s.back()) == F_L) {  // 들어 → 듣
          s.back() = withFinal(s.back(), F_D);
          addPredicate(out, s, 0);
        }
        if (finalOf(before.back()) == NONE) {  // 나아 → 낫
          s = before;
          s.back() = withFinal(s.back(), F_S);
          addPredicate(out, s, 0);
        }
      }
    }
    addContracted(out, before, c, 0);
    if (out.full()) return;
  }
}

bool finalAllows(const char32_t c, const uint8_t after) {
  if (!hangul(c)) return after == 0;
  const int t = finalOf(c);
  switch (after) {
    case 1:
      return t != NONE;
    case 2:
      return t == NONE;
    case 3:
      return t == NONE || t == F_L;
    default:
      return true;
  }
}

// Code points in a UTF-8 string: its non-continuation bytes.
size_t codepointCount(const char* s) {
  size_t n = 0;
  for (; *s != '\0'; s++) n += (static_cast<unsigned char>(*s) & 0xC0) != 0x80;
  return n;
}

void addSuffixed(Out& out, const Word& w, const Particle* table, const size_t count, const bool predicate) {
  // Longest suffix first: a longer match is the more specific reading. Suffixes are matched on the
  // UTF-8 surface, so a lookup decodes only the ones that match.
  const std::string& surface = out.surface;
  std::vector<std::pair<size_t, size_t>> order;  // (length in code points, index)
  order.reserve(count);
  for (size_t i = 0; i < count; i++) order.emplace_back(codepointCount(table[i].text), i);
  std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& [len, i] : order) {
    const size_t bytes = std::strlen(table[i].text);
    if (len >= w.size() || bytes >= surface.size() ||
        surface.compare(surface.size() - bytes, bytes, table[i].text) != 0) {
      continue;
    }
    const Word suffix = decode(table[i].text);
    const Word stem(w.begin(), w.end() - static_cast<long>(len));
    if (!finalAllows(stem.back(), table[i].after)) continue;
    if (predicate) {
      addPredicate(out, stem, 0);
      // ㄹ drops before an ending that opens with ㄴ ㅂ or ㅅ: 사는 → 살다, 아세요 → 알다.
      const int opening = initialOf(suffix.front());
      if (hangul(suffix.front()) && finalOf(stem.back()) == NONE && (opening == 2 || opening == 7 || opening == 9)) {
        Word dropped = stem;
        dropped.back() = withFinal(dropped.back(), F_L);
        addPredicate(out, dropped, 0);
      }
    } else {
      out.add(stem);
    }
    if (out.full()) return;
  }
}

}  // namespace

void variants(const std::string& word, std::vector<std::string>& out) {
  out.clear();
  const Word w = decode(word);
  if (w.empty() || !hangul(w.back())) return;
  out.reserve(MAX_VARIANTS);
  Out o{word, out};
  addSuffixed(o, w, kParticles, sizeof(kParticles) / sizeof(kParticles[0]), false);
  addSuffixed(o, w, kCopula, sizeof(kCopula) / sizeof(kCopula[0]), false);
  addSuffixed(o, w, kEndings, sizeof(kEndings) / sizeof(kEndings[0]), true);
  addVowelForms(o, w);
  addJamoStem(o, w);
  // A bare stem with a past or honorific marker and nothing after (먹었, 가셨): rare, but cheap.
  addPredicate(o, w, 1);
}

}  // namespace KoreanInflection
