#include "AnkiHtml.h"

#include <Epub/htmlEntities.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace ankihtml {
namespace {

enum TagFlag : uint8_t {
  VOID = 1,   // always self-closed
  DROP = 2,   // removed with its content
  RAW = 4,    // content is raw text up to the matching end tag
  STRIP = 8,  // tag removed, content kept
};

struct TagInfo {
  std::string_view name;
  uint8_t flags;
};

// Sorted by name for binary search. Tags not listed are stripped (content kept).
constexpr TagInfo TAGS[] = {
    {"a", 0},          {"abbr", 0},          {"address", 0},      {"area", VOID},
    {"article", 0},    {"aside", 0},         {"audio", DROP},     {"b", 0},
    {"base", VOID},    {"bdi", 0},           {"bdo", 0},          {"big", 0},
    {"blockquote", 0}, {"body", STRIP},      {"br", VOID},        {"button", 0},
    {"canvas", DROP},  {"caption", 0},       {"center", 0},       {"cite", 0},
    {"code", 0},       {"col", VOID},        {"colgroup", 0},     {"data", 0},
    {"dd", 0},         {"del", 0},           {"details", 0},      {"dfn", 0},
    {"dialog", 0},     {"div", 0},           {"dl", 0},           {"dt", 0},
    {"em", 0},         {"embed", VOID | DROP}, {"fieldset", 0},   {"figcaption", 0},
    {"figure", 0},     {"font", 0},          {"footer", 0},       {"form", 0},
    {"h1", 0},         {"h2", 0},            {"h3", 0},           {"h4", 0},
    {"h5", 0},         {"h6", 0},            {"head", DROP},      {"header", 0},
    {"hr", VOID},      {"html", STRIP},      {"i", 0},            {"iframe", DROP | RAW},
    {"img", VOID},     {"input", VOID},      {"ins", 0},          {"kbd", 0},
    {"label", 0},      {"legend", 0},        {"li", 0},           {"link", VOID},
    {"main", 0},       {"map", 0},           {"mark", 0},         {"math", DROP},
    {"meta", VOID},    {"nav", 0},           {"noscript", DROP | RAW}, {"object", DROP},
    {"ol", 0},         {"optgroup", 0},      {"option", 0},       {"p", 0},
    {"param", VOID},   {"picture", 0},       {"pre", 0},          {"q", 0},
    {"rb", 0},         {"rp", 0},            {"rt", 0},           {"rtc", 0},
    {"ruby", 0},       {"s", 0},             {"samp", 0},         {"script", DROP | RAW},
    {"section", 0},    {"select", 0},        {"small", 0},        {"source", VOID},
    {"span", 0},       {"strike", 0},        {"strong", 0},       {"style", DROP | RAW},
    {"sub", 0},        {"summary", 0},       {"sup", 0},          {"svg", DROP},
    {"table", 0},      {"tbody", 0},         {"td", 0},           {"template", DROP},
    {"textarea", DROP | RAW}, {"tfoot", 0},  {"th", 0},           {"thead", 0},
    {"time", 0},       {"title", DROP | RAW}, {"tr", 0},          {"track", VOID},
    {"tt", 0},         {"u", 0},             {"ul", 0},           {"var", 0},
    {"video", DROP},   {"wbr", VOID},
};
constexpr int TAG_COUNT = static_cast<int>(std::size(TAGS));
constexpr int NO_TAG = -1;

constexpr bool tagsSorted() {
  for (int i = 1; i < TAG_COUNT; ++i) {
    if (!(TAGS[i - 1].name < TAGS[i].name)) return false;
  }
  return true;
}
static_assert(tagsSorted(), "TAGS must be sorted");
static_assert(TAG_COUNT < 255, "tag ids are uint8_t");

constexpr int findTag(std::string_view name) {
  int lo = 0;
  int hi = TAG_COUNT;
  while (lo < hi) {
    const int mid = (lo + hi) / 2;
    if (TAGS[mid].name == name) return mid;
    if (TAGS[mid].name < name) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return NO_TAG;
}

constexpr uint8_t tagId(std::string_view name) { return static_cast<uint8_t>(findTag(name)); }

// Start tags that close an open <p>.
constexpr uint8_t P_CLOSERS[] = {
    tagId("address"), tagId("article"), tagId("aside"),   tagId("blockquote"), tagId("center"),
    tagId("details"), tagId("dialog"),  tagId("div"),     tagId("dl"),         tagId("fieldset"),
    tagId("figcaption"), tagId("figure"), tagId("footer"), tagId("form"),      tagId("h1"),
    tagId("h2"),      tagId("h3"),      tagId("h4"),      tagId("h5"),         tagId("h6"),
    tagId("header"),  tagId("hr"),      tagId("main"),    tagId("nav"),        tagId("ol"),
    tagId("p"),       tagId("pre"),     tagId("section"), tagId("table"),      tagId("ul"),
    tagId("li"),      tagId("dd"),      tagId("dt"),
};
// Elements an implied </p> does not reach past.
constexpr uint8_t P_BARRIERS[] = {
    tagId("td"),      tagId("th"),     tagId("table"),   tagId("li"),     tagId("dd"),
    tagId("dt"),      tagId("ul"),     tagId("ol"),      tagId("dl"),     tagId("div"),
    tagId("blockquote"), tagId("section"), tagId("article"), tagId("aside"), tagId("header"),
    tagId("footer"),  tagId("nav"),    tagId("main"),    tagId("figure"), tagId("details"),
    tagId("button"),  tagId("ruby"),   tagId("caption"),
};
constexpr uint8_t HEADINGS[] = {tagId("h1"), tagId("h2"), tagId("h3"), tagId("h4"), tagId("h5"), tagId("h6")};
constexpr uint8_t P_ONLY[] = {tagId("p")};
constexpr uint8_t LI[] = {tagId("li")};
constexpr uint8_t LI_BARRIERS[] = {tagId("ul"), tagId("ol"), tagId("table"), tagId("td"), tagId("th")};
constexpr uint8_t DT_DD[] = {tagId("dt"), tagId("dd")};
constexpr uint8_t DT_DD_BARRIERS[] = {tagId("dl"), tagId("table"), tagId("td"), tagId("th")};
constexpr uint8_t TR[] = {tagId("tr")};
constexpr uint8_t TR_BARRIERS[] = {tagId("table"), tagId("thead"), tagId("tbody"), tagId("tfoot")};
constexpr uint8_t TD_TH[] = {tagId("td"), tagId("th")};
constexpr uint8_t TD_BARRIERS[] = {tagId("tr"), tagId("table")};
constexpr uint8_t TSECTIONS[] = {tagId("thead"), tagId("tbody"), tagId("tfoot")};
constexpr uint8_t TABLE[] = {tagId("table")};
constexpr uint8_t OPTION[] = {tagId("option")};
constexpr uint8_t OPTGROUP_OPTION[] = {tagId("optgroup"), tagId("option")};
constexpr uint8_t OPTION_BARRIERS[] = {tagId("select"), tagId("optgroup")};
constexpr uint8_t SELECT[] = {tagId("select")};
constexpr uint8_t RUBY_TEXT[] = {tagId("rb"), tagId("rt"), tagId("rp")};
constexpr uint8_t RUBY_TEXT_RTC[] = {tagId("rb"), tagId("rt"), tagId("rp"), tagId("rtc")};
constexpr uint8_t RUBY[] = {tagId("ruby"), tagId("rtc")};
constexpr uint8_t RUBY_ONLY[] = {tagId("ruby")};

template <size_t N>
constexpr bool contains(const uint8_t (&set)[N], uint8_t id) {
  for (const uint8_t v : set) {
    if (v == id) return true;
  }
  return false;
}

constexpr size_t MAX_DEPTH = 64;
constexpr size_t MAX_ATTRS = 12;
constexpr size_t MAX_TAG_NAME = 15;
constexpr size_t MAX_ENTITY_NAME = 32;

constexpr bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
constexpr bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
constexpr bool isDigit(char c) { return c >= '0' && c <= '9'; }
constexpr char toLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (toLower(a[i]) != toLower(b[i])) return false;
  }
  return true;
}

bool containsIgnoreCase(std::string_view hay, std::string_view needle) {
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    if (equalsIgnoreCase(hay.substr(i, needle.size()), needle)) return true;
  }
  return false;
}

// XML name without ':' (namespace prefixes would be undeclared). Non-ASCII bytes are accepted.
bool isXmlName(std::string_view name) {
  if (name.empty()) return false;
  const auto c0 = static_cast<unsigned char>(name[0]);
  if (!(isAlpha(name[0]) || name[0] == '_' || c0 >= 0x80)) return false;
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (!(isAlpha(c) || isDigit(c) || c == '_' || c == '-' || c == '.' || u >= 0x80)) return false;
  }
  return true;
}

bool isXmlChar(uint32_t cp) {
  return cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD) ||
         (cp >= 0x10000 && cp <= 0x10FFFF);
}

class Converter {
 public:
  // `frag` or `css` may be null to skip that output.
  Converter(std::string_view html, std::string* frag, std::string* css, size_t maxOut)
      : in(html), frag(frag), css(css), maxOut(maxOut) {}

  bool run() {
    while (pos < in.size() && !overflow) {
      if (in[pos] == '<') {
        handleLt();
      } else {
        const size_t next = in.find('<', pos);
        const size_t end = next == std::string_view::npos ? in.size() : next;
        if (visible()) emitText(in.substr(pos, end - pos), false);
        pos = end;
      }
    }
    while (depth > 0 && !overflow) popOne();
    return !overflow;
  }

 private:
  std::string_view in;
  std::string* frag;
  std::string* css;
  size_t maxOut;
  size_t pos = 0;
  bool overflow = false;
  std::array<uint8_t, MAX_DEPTH> stack{};
  size_t depth = 0;
  // Index of the outermost open element being dropped with its content, or -1.
  int hiddenAt = -1;

  bool visible() const { return frag != nullptr && hiddenAt < 0; }

  void put(std::string* s, std::string_view text) {
    if (overflow) return;
    if (s->size() + text.size() > maxOut) {
      overflow = true;
      return;
    }
    s->append(text);
  }
  void out(std::string_view text) {
    if (visible()) put(frag, text);
  }
  void outLowerName(size_t off, size_t len) {
    for (size_t k = 0; k < len; ++k) {
      const char c = toLower(in[off + k]);
      out(std::string_view(&c, 1));
    }
  }

  // Escapes text (or an attribute value) and resolves entities.
  void emitText(std::string_view text, bool inAttr) {
    size_t runStart = 0;
    auto flush = [&](size_t upTo) {
      if (upTo > runStart) out(text.substr(runStart, upTo - runStart));
    };
    for (size_t i = 0; i < text.size(); ++i) {
      const char c = text[i];
      const auto u = static_cast<unsigned char>(c);
      const char* repl = nullptr;
      size_t consumed = 1;
      char numBuf[5];
      if (c == '&') {
        consumed = entity(text.substr(i), repl, numBuf);
      } else if (c == '<') {
        repl = "&lt;";
      } else if (c == '>') {
        repl = "&gt;";
      } else if (c == '"' && inAttr) {
        repl = "&quot;";
      } else if (u < 0x20 && c != '\t' && c != '\n' && c != '\r') {
        repl = "";
      } else {
        continue;
      }
      flush(i);
      out(repl);
      i += consumed - 1;
      runStart = i + 1;
    }
    flush(text.size());
  }

  // `s` starts with '&'. Sets `repl` and returns the bytes consumed.
  static size_t entity(std::string_view s, const char*& repl, char (&numBuf)[5]) {
    repl = "&amp;";
    size_t i = 1;
    if (i < s.size() && s[i] == '#') {
      ++i;
      const bool hex = i < s.size() && (s[i] == 'x' || s[i] == 'X');
      if (hex) ++i;
      const size_t digitsStart = i;
      uint32_t cp = 0;
      while (i < s.size() && i - digitsStart < 8) {
        const char c = toLower(s[i]);
        uint32_t d;
        if (isDigit(c)) {
          d = static_cast<uint32_t>(c - '0');
        } else if (hex && c >= 'a' && c <= 'f') {
          d = static_cast<uint32_t>(c - 'a' + 10);
        } else {
          break;
        }
        cp = cp * (hex ? 16 : 10) + d;
        ++i;
      }
      if (i == digitsStart || i >= s.size() || s[i] != ';' || !isXmlChar(cp)) return 1;
      // Encode as UTF-8 so control-range and other references need no XML-side checks.
      if (cp < 0x80) {
        numBuf[0] = static_cast<char>(cp);
        numBuf[1] = '\0';
        if (cp == '&') repl = "&amp;";
        else if (cp == '<') repl = "&lt;";
        else if (cp == '>') repl = "&gt;";
        else if (cp == '"') repl = "&quot;";
        else repl = numBuf;
      } else {
        size_t n = 0;
        if (cp < 0x800) {
          numBuf[n++] = static_cast<char>(0xC0 | (cp >> 6));
        } else if (cp < 0x10000) {
          numBuf[n++] = static_cast<char>(0xE0 | (cp >> 12));
          numBuf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        } else {
          numBuf[n++] = static_cast<char>(0xF0 | (cp >> 18));
          numBuf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
          numBuf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        }
        numBuf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        numBuf[n] = '\0';
        repl = numBuf;
      }
      return i + 1;
    }
    while (i < s.size() && i <= MAX_ENTITY_NAME && (isAlpha(s[i]) || isDigit(s[i]))) ++i;
    if (i == 1 || i >= s.size() || s[i] != ';') return 1;
    const std::string_view name = s.substr(1, i - 1);
    if (name == "amp") repl = "&amp;";
    else if (name == "lt") repl = "&lt;";
    else if (name == "gt") repl = "&gt;";
    else if (name == "quot") repl = "&quot;";
    else if (name == "apos") repl = "&apos;";
    else {
      const char* v = lookupHtmlEntity(s.data(), i + 1);
      if (v == nullptr) return 1;
      repl = v;
    }
    return i + 1;
  }

  void skipTo(std::string_view marker) {
    const size_t at = in.find(marker, pos);
    pos = at == std::string_view::npos ? in.size() : at + marker.size();
  }

  void handleLt() {
    const size_t rest = in.size() - pos;
    const char next = rest > 1 ? in[pos + 1] : '\0';
    if (isAlpha(next)) {
      startTag();
    } else if (next == '/' && rest > 2 && isAlpha(in[pos + 2])) {
      endTag();
    } else if (in.substr(pos, 4) == "<!--") {
      pos += 4;
      skipTo("-->");
    } else if (next == '!' || next == '?' || next == '/') {
      skipTo(">");
    } else {
      out("&lt;");
      ++pos;
    }
  }

  // Reads a tag name at `pos`; returns its table index or NO_TAG.
  int readTagName() {
    const size_t start = pos;
    while (pos < in.size() && !isSpace(in[pos]) && in[pos] != '/' && in[pos] != '>') ++pos;
    const size_t len = pos - start;
    if (len > MAX_TAG_NAME) return NO_TAG;
    char lower[MAX_TAG_NAME];
    for (size_t i = 0; i < len; ++i) lower[i] = toLower(in[start + i]);
    return findTag(std::string_view(lower, len));
  }

  void startTag() {
    const size_t tagStart = pos;
    ++pos;
    const int id = readTagName();
    const std::string_view nameText = in.substr(tagStart + 1, pos - tagStart - 1);
    const uint8_t flags = id == NO_TAG ? STRIP : TAGS[id].flags;

    // Attributes are parsed in place; offsets into `in` keep dedupe off the heap.
    struct Attr {
      uint32_t nameOff;
      uint32_t valOff;
      uint32_t valLen;
      uint8_t nameLen;
      bool hasValue;
    };
    std::array<Attr, MAX_ATTRS> attrs;
    size_t attrCount = 0;
    bool iconFont = false;
    while (pos < in.size()) {
      while (pos < in.size() && (isSpace(in[pos]) || in[pos] == '/')) ++pos;
      if (pos >= in.size()) break;
      if (in[pos] == '>') {
        ++pos;
        break;
      }
      const size_t nameOff = pos;
      ++pos;  // a leading '=' belongs to the name
      while (pos < in.size() && !isSpace(in[pos]) && in[pos] != '/' && in[pos] != '>' && in[pos] != '=') ++pos;
      const size_t nameLen = pos - nameOff;
      while (pos < in.size() && isSpace(in[pos])) ++pos;
      size_t valOff = pos;
      size_t valLen = 0;
      bool hasValue = false;
      if (pos < in.size() && in[pos] == '=') {
        ++pos;
        while (pos < in.size() && isSpace(in[pos])) ++pos;
        hasValue = true;
        if (pos < in.size() && (in[pos] == '"' || in[pos] == '\'')) {
          const char q = in[pos++];
          valOff = pos;
          const size_t close = in.find(q, pos);
          pos = close == std::string_view::npos ? in.size() : close;
          valLen = pos - valOff;
          if (pos < in.size()) ++pos;
        } else {
          valOff = pos;
          while (pos < in.size() && !isSpace(in[pos]) && in[pos] != '>') ++pos;
          valLen = pos - valOff;
        }
      }
      const std::string_view name = in.substr(nameOff, nameLen);
      if (!isXmlName(name) || nameLen > 255 || (nameLen >= 2 && equalsIgnoreCase(name.substr(0, 2), "on"))) continue;
      bool dup = false;
      for (size_t a = 0; a < attrCount && !dup; ++a) {
        dup = equalsIgnoreCase(in.substr(attrs[a].nameOff, attrs[a].nameLen), name);
      }
      if (dup || attrCount == MAX_ATTRS) continue;
      if (hasValue && equalsIgnoreCase(name, "class")) {
        const std::string_view v = in.substr(valOff, valLen);
        iconFont = containsIgnoreCase(v, "material-icons") || containsIgnoreCase(v, "material-symbols");
      }
      attrs[attrCount++] = {static_cast<uint32_t>(nameOff), static_cast<uint32_t>(valOff),
                            static_cast<uint32_t>(valLen), static_cast<uint8_t>(nameLen), hasValue};
    }

    if (flags & RAW) {
      skipRawText(nameText, id == tagId("style") && hiddenAt < 0);
      return;
    }
    if (flags & STRIP) return;
    const auto tag = static_cast<uint8_t>(id);
    const bool drop = (flags & DROP) || iconFont;
    if ((flags & VOID) && drop) return;
    closeImplied(tag);
    if (!(flags & VOID) && depth == MAX_DEPTH) {
      // No room to track it: drop the tag, and for dropped elements skip to the end tag.
      if (drop) skipRawText(nameText, false);
      return;
    }

    if (drop) {
      // Nothing inside this element is emitted until it closes.
      if (hiddenAt < 0) hiddenAt = static_cast<int>(depth);
      stack[depth++] = tag;
      return;
    }
    out("<");
    out(TAGS[tag].name);
    for (size_t a = 0; a < attrCount; ++a) {
      out(" ");
      outLowerName(attrs[a].nameOff, attrs[a].nameLen);
      out("=\"");
      if (attrs[a].hasValue) {
        emitText(in.substr(attrs[a].valOff, attrs[a].valLen), true);
      } else {
        outLowerName(attrs[a].nameOff, attrs[a].nameLen);
      }
      out("\"");
    }
    out((flags & VOID) ? "/>" : ">");
    if (flags & VOID) return;
    stack[depth++] = tag;
  }

  // Skips raw text up to the matching end tag; appends it to `css` when `collect`.
  void skipRawText(std::string_view name, bool collect) {
    const size_t start = pos;
    size_t end = in.size();
    size_t after = in.size();
    for (size_t i = in.find("</", pos); i != std::string_view::npos; i = in.find("</", i + 1)) {
      const size_t n = i + 2 + name.size();
      if (n > in.size() || !equalsIgnoreCase(in.substr(i + 2, name.size()), name)) continue;
      if (n < in.size() && !isSpace(in[n]) && in[n] != '/' && in[n] != '>') continue;
      end = i;
      const size_t gt = in.find('>', n);
      after = gt == std::string_view::npos ? in.size() : gt + 1;
      break;
    }
    if (collect && css != nullptr) {
      put(css, in.substr(start, end - start));
      put(css, "\n");
    }
    pos = after;
  }

  template <size_t N, size_t M>
  void closeNearest(const uint8_t (&targets)[N], const uint8_t (&barriers)[M]) {
    for (size_t i = depth; i-- > 0;) {
      if (contains(targets, stack[i])) {
        while (depth > i) popOne();
        return;
      }
      if (contains(barriers, stack[i])) return;
    }
  }

  void closeImplied(uint8_t tag) {
    if (contains(P_CLOSERS, tag)) closeNearest(P_ONLY, P_BARRIERS);
    if (contains(HEADINGS, tag) && depth > 0 && contains(HEADINGS, stack[depth - 1])) popOne();
    if (tag == tagId("li")) closeNearest(LI, LI_BARRIERS);
    if (contains(DT_DD, tag)) closeNearest(DT_DD, DT_DD_BARRIERS);
    if (tag == tagId("tr")) closeNearest(TR, TR_BARRIERS);
    if (contains(TD_TH, tag)) closeNearest(TD_TH, TD_BARRIERS);
    if (contains(TSECTIONS, tag)) closeNearest(TSECTIONS, TABLE);
    if (tag == tagId("option")) closeNearest(OPTION, OPTION_BARRIERS);
    if (tag == tagId("optgroup")) closeNearest(OPTGROUP_OPTION, SELECT);
    if (contains(RUBY_TEXT, tag)) closeNearest(RUBY_TEXT, RUBY);
    if (tag == tagId("rtc")) closeNearest(RUBY_TEXT_RTC, RUBY_ONLY);
  }

  void popOne() {
    --depth;
    if (hiddenAt >= 0 && static_cast<size_t>(hiddenAt) == depth) {
      hiddenAt = -1;
      return;
    }
    out("</");
    out(TAGS[stack[depth]].name);
    out(">");
  }

  void endTag() {
    pos += 2;
    const int id = readTagName();
    skipTo(">");
    if (id == NO_TAG) return;
    const auto tag = static_cast<uint8_t>(id);
    if (tag == tagId("br")) {
      out("<br/>");
      return;
    }
    for (size_t i = depth; i-- > 0;) {
      if (stack[i] == tag) {
        while (depth > i) popOne();
        return;
      }
    }
  }
};

}  // namespace

bool toXhtml(std::string_view html, std::string& out, size_t maxOut) {
  out.clear();
  // One up-front block: card HTML grows little when made well-formed.
  out.reserve(std::min(maxOut, html.size() + html.size() / 8 + 32));
  Converter c(html, &out, nullptr, maxOut);
  if (!c.run()) {
    out.clear();
    return false;
  }
  return true;
}

bool extractStyles(std::string_view html, std::string& css, size_t maxOut) {
  css.clear();
  Converter c(html, nullptr, &css, maxOut);
  if (!c.run()) {
    css.clear();
    return false;
  }
  return true;
}

}  // namespace ankihtml
