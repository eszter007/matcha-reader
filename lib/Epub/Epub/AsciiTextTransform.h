#pragma once

#include "css/CssStyle.h"

// Apply the CSS text-transform subset without interpreting UTF-8 continuation bytes as
// characters. Only ASCII A-Z/a-z are changed; every byte >= 0x80 passes through untouched.
//
// `capitalizeWordStart` is false when this buffer continues a word split at an inline element
// or the parser's fixed-size word boundary, so capitalize does not invent a second capital.
inline void applyAsciiTextTransform(char* text, const CssTextTransform transform, const bool capitalizeWordStart) {
  if (text == nullptr || transform == CssTextTransform::None) return;

  auto* p = reinterpret_cast<unsigned char*>(text);
  if (transform == CssTextTransform::Uppercase) {
    for (; *p != '\0'; ++p) {
      if (*p >= 'a' && *p <= 'z') *p -= ('a' - 'A');
    }
    return;
  }
  if (transform == CssTextTransform::Lowercase) {
    for (; *p != '\0'; ++p) {
      if (*p >= 'A' && *p <= 'Z') *p += ('a' - 'A');
    }
    return;
  }
  if (!capitalizeWordStart) return;

  // Capitalize the first ASCII letter after ASCII punctuation. If a non-ASCII codepoint comes
  // first, leave the word alone: without a Unicode case table we cannot know whether it is a
  // letter, and changing a later ASCII byte would not implement "first typographic letter".
  for (; *p != '\0'; ++p) {
    if (*p >= 0x80) return;
    if (*p >= 'a' && *p <= 'z') {
      *p -= ('a' - 'A');
      return;
    }
    if (*p >= 'A' && *p <= 'Z') return;
  }
}
