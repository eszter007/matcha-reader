#include "Furigana.h"

#include <algorithm>
#include <cstdint>

namespace furigana {

namespace {

uint32_t nextCp(const std::string_view s, size_t& at) {
  const auto b = static_cast<unsigned char>(s[at]);
  const int len = b < 0x80 ? 1 : (b >> 5) == 0x6 ? 2 : (b >> 4) == 0xE ? 3 : (b >> 3) == 0x1E ? 4 : 1;
  uint32_t cp = len == 1 ? b : b & (0xFF >> (len + 1));
  for (int i = 1; i < len && at + i < s.size(); i++) cp = (cp << 6) | (static_cast<unsigned char>(s[at + i]) & 0x3F);
  at = std::min(at + len, s.size());  // a cut-off character never steps past the end
  return cp;
}

void appendCp(std::string& out, const uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool isKana(const uint32_t cp) { return cp >= 0x3041 && cp <= 0x30FF; }

uint32_t fold(const uint32_t cp) { return cp >= 0x30A1 && cp <= 0x30F6 ? cp - 0x60 : cp; }

std::string folded(const std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t at = 0; at < text.size();) appendCp(out, fold(nextCp(text, at)));
  return out;
}

}  // namespace

void appendRuby(std::string& out, const std::string_view base, const std::string_view reading) {
  if (!out.empty() && out.back() != ']') out += ' ';
  out.append(base);
  out += '[';
  out.append(reading);
  out += ']';
}

std::string kanaOf(const std::string_view text) {
  std::string out;
  size_t plainStart = 0;  // where the text since the last space or ']' began
  for (size_t i = 0; i < text.size(); i++) {
    const char c = text[i];
    if (c == ' ') {
      plainStart = i + 1;
    } else if (c == '[') {
      const size_t close = text.find(']', i);
      if (close == std::string_view::npos) break;
      // The base was copied as plain text: replace it with its reading.
      out.resize(out.size() - (i - plainStart));
      out.append(text.substr(i + 1, close - i - 1));
      i = close;
      plainStart = close + 1;
    } else {
      out += c;
    }
  }
  return folded(out);
}

bool matchesReading(const std::string_view text, const std::string_view readings) {
  const std::string kana = kanaOf(text);
  if (kana.empty()) return false;
  static constexpr std::string_view SEP = "\xe3\x80\x81";  // 、
  size_t from = 0;
  while (from <= readings.size()) {
    const size_t to = readings.find(SEP, from);
    const std::string_view one =
        readings.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from);
    if (folded(one) == kana) return true;
    if (to == std::string_view::npos) break;
    from = to + SEP.size();
  }
  return false;
}

std::string adaptToHeadword(const std::string_view surface, const std::string_view headword) {
  std::string out;
  size_t pos = 0;  // in headword
  bool anyRuby = false;
  size_t plainStart = 0;
  std::string plain;  // surface plain text since the last space or ']'
  const auto takePlain = [&](const std::string_view text) {
    // Plain kana is kept while the headword repeats it; inflected endings stop the walk.
    size_t i = 0;
    while (i < text.size() && pos < headword.size()) {
      size_t ti = i, hi = pos;
      if (nextCp(text, ti) != nextCp(headword, hi)) break;
      i = ti;
      pos = hi;
    }
    out.append(text.substr(0, i));
    return i == text.size();
  };
  for (size_t i = 0; i < surface.size(); i++) {
    const char c = surface[i];
    if (c == ' ' || c == '[') {
      if (c == '[') {
        const size_t close = surface.find(']', i);
        if (close == std::string_view::npos) return {};
        // The text since the last boundary is this ruby's base.
        const std::string_view base = surface.substr(plainStart, i - plainStart);
        if (headword.substr(pos, base.size()) != base) break;
        appendRuby(out, base, surface.substr(i + 1, close - i - 1));
        pos += base.size();
        anyRuby = true;
        i = close;
      }
      plainStart = i + 1;
      plain.clear();
      continue;
    }
    // Plain text: provisional until a '[' shows it was a base.
    const size_t next = surface.find_first_of(" [", i);
    const size_t end = next == std::string_view::npos ? surface.size() : next;
    if (next != std::string_view::npos && surface[next] == '[') {
      i = next - 1;  // all of it is the coming base
      continue;
    }
    if (!takePlain(surface.substr(i, end - i))) break;
    i = end - 1;
    plainStart = end;
  }
  if (!anyRuby) return {};
  // The rest of the headword (an inflected ending's dictionary form) must be kana.
  for (size_t at = pos; at < headword.size();) {
    if (!isKana(nextCp(headword, at))) return {};
  }
  out.append(headword.substr(pos));
  return out;
}

}  // namespace furigana
