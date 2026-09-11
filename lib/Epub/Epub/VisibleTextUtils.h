#pragma once

#include <cstddef>
#include <string_view>

namespace VisibleTextUtils {

inline bool equalsTag(const std::string_view name, const std::string_view tag) {
  if (name.size() != tag.size()) return false;
  for (std::size_t i = 0; i < name.size(); i++) {
    const char c = name[i] >= 'A' && name[i] <= 'Z' ? static_cast<char>(name[i] + ('a' - 'A')) : name[i];
    if (c != tag[i]) return false;
  }
  return true;
}

inline bool isNonVisibleElement(const std::string_view name) {
  return equalsTag(name, "head") || equalsTag(name, "style") || equalsTag(name, "script") || equalsTag(name, "title") ||
         equalsTag(name, "rp");
}

// One rule for the attribute-driven subtrees the layout parser leaves out of visibleTextOffset:
// the HTML hidden attribute (ChapterHtmlSlimParser.cpp:1330) and pagebreak markers (:1962-1970).
// Every counter that produces or consumes those offsets -- the horizontal parser, the vertical
// extractor, the KOSync XPath resolver and the reverse streamer -- has to agree, or an anchor
// drifts by the length of the skipped text. Attribute names are case-insensitive in HTML.
inline bool isSkippedSubtreeAttribute(const std::string_view name, const std::string_view value) {
  // Attribute NAMES are case-insensitive in HTML; the values are compared exactly, as
  // ChapterHtmlSlimParser does -- it produces the offsets everything else is measured against, so
  // a laxer rule here would skip text the layout counted.
  if (equalsTag(name, "hidden")) return true;
  if (equalsTag(name, "role") && value == "doc-pagebreak") return true;
  if (equalsTag(name, "epub:type") && value == "pagebreak") return true;
  return false;
}

}  // namespace VisibleTextUtils
