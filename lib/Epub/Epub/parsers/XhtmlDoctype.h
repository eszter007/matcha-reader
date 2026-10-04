#pragma once

#include <cstddef>
#include <cstring>

// Expat treats an entity it has no declaration for (&nbsp;, &mdash;) as a fatal error unless the
// document names an external DTD: only then does it hand the reference to the default handler,
// where the chapter parsers resolve the HTML names. A chapter with no DOCTYPE, or with HTML5's
// bare <!DOCTYPE html>, names none, so one &nbsp; made the whole book "invalid". The parsers
// feed such a chapter a DOCTYPE that does (it is never fetched; the build has no DTD support).
namespace xhtml {

inline constexpr char kExternalDoctype[] =
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">";

struct DoctypePatch {
  bool inject = false;  // feed `keep` bytes, then kExternalDoctype, then resume `skip` bytes later
  size_t keep = 0;
  size_t skip = 0;
};

// Decide from the start of a chapter (its first read) whether it needs the DOCTYPE fed in.
// Anything undecidable -- the prolog runs past the chunk, an internal subset -- is left alone.
inline DoctypePatch findDoctypePatch(const char* data, const size_t len) {
  const auto startsWith = [data, len](const size_t at, const char* lit) {
    const size_t n = std::strlen(lit);
    if (at + n > len) return false;
    for (size_t i = 0; i < n; i++) {
      char c = data[at + i];
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
      if (c != lit[i]) return false;
    }
    return true;
  };
  const auto find = [data, len](size_t from, const char* lit) {
    const size_t n = std::strlen(lit);
    for (; from + n <= len; from++) {
      if (std::memcmp(data + from, lit, n) == 0) return from;
    }
    return len;
  };

  size_t i = 0;
  while (i < len) {
    if (data[i] != '<') {
      i++;  // BOM, whitespace between prolog items
      continue;
    }
    if (startsWith(i, "<?")) {
      const size_t end = find(i, "?>");
      if (end == len) return {};
      i = end + 2;
    } else if (startsWith(i, "<!--")) {
      const size_t end = find(i, "-->");
      if (end == len) return {};
      i = end + 3;
    } else if (startsWith(i, "<!DOCTYPE")) {
      const size_t end = find(i, ">");
      if (end == len) return {};
      for (size_t k = i; k < end; k++) {
        if (data[k] == '[' || startsWith(k, "PUBLIC") || startsWith(k, "SYSTEM")) return {};
      }
      return {true, i, end + 1 - i};
    } else {
      return {true, i, 0};  // the root element, with no DOCTYPE before it
    }
  }
  return {};
}

}  // namespace xhtml
