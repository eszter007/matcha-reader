#pragma once

#include <expat.h>

#include <cstdint>
#include <string>

// Replacement text for a gaiji inline image (class="gaiji"): EBPAJ books use
// tiny per-glyph images for characters outside the font. Rendering them as
// images would split the paragraph and insert a near-empty image page, so the
// parsers emit text instead: the alt attribute when present, else a codepoint
// encoded in the file name (EBPAJ "u3396-g.png" convention), else the geta
// mark 〓 -- the traditional print placeholder for an unavailable glyph.
inline std::string gaijiReplacementText(const std::string& src, const std::string& alt) {
  if (!alt.empty()) return alt;

  const size_t slash = src.find_last_of('/');
  const char* p = src.c_str() + (slash == std::string::npos ? 0 : slash + 1);
  if (*p == 'u' || *p == 'U') {
    const char* h = p + 1;
    if (*h == '+') h++;
    uint32_t cp = 0;
    int digits = 0;
    while (digits < 6) {
      const char c = *h;
      uint32_t v;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      else break;
      cp = cp * 16 + v;
      h++;
      digits++;
    }
    const bool delimited = (*h == '-' || *h == '.' || *h == '_');
    if (digits >= 4 && delimited && cp >= 0x80 && cp <= 0x10FFFF) {
      std::string out;
      if (cp < 0x800) {
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
      return out;
    }
  }
  return "\xE3\x80\x93";  // 〓 U+3013 GETA MARK
}

// Safely tear down an expat parser: stop processing, clear callbacks, free, and null the pointer.
inline void destroyXmlParser(XML_Parser& parser) {
  if (!parser) return;
  XML_StopParser(parser, XML_FALSE);
  XML_SetElementHandler(parser, nullptr, nullptr);
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  parser = nullptr;
}
