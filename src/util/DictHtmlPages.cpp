#include "DictHtmlPages.h"

#include <Arduino.h>
#include <Epub/css/CssParser.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

#include "CrossPointSettings.h"

namespace {

// Normalized XHTML staged here for the file-driven parser; truncated on each
// use, removed after the parse.
constexpr const char* TMP_HTML_PATH = "/.crosspoint/dicthtml.tmp";

// Keep enough contiguous heap for the parser's 16KB SD-font advance scratch
// plus page/layout allocations. Falling back to plain text is cheaper than
// entering a throwing allocation path under pressure.
constexpr size_t MIN_STYLED_FREE_HEAP = 40 * 1024;
constexpr size_t MIN_STYLED_MAX_ALLOC = 20 * 1024;

// Bound retained layout independently of input bytes: compact markup can emit
// far more objects than its source size suggests.
constexpr size_t MAX_STYLED_PAGES = 64;
constexpr size_t MAX_STYLED_PAGE_ELEMENTS = 512;

class BufferedFileWriter {
 public:
  explicit BufferedFileWriter(HalFile& file) : file(file) {}

  bool append(const char c) { return append(&c, 1); }

  bool append(const char* data, size_t len) {
    while (len > 0) {
      const size_t available = sizeof(buffer) - used;
      const size_t chunk = std::min(available, len);
      memcpy(buffer + used, data, chunk);
      used += chunk;
      data += chunk;
      len -= chunk;
      if (used == sizeof(buffer) && !flush()) return false;
    }
    return true;
  }

  bool append(const char* text) { return append(text, strlen(text)); }

  bool flush() {
    if (used == 0) return true;
    if (file.write(buffer, used) != used) return false;
    used = 0;
    return true;
  }

 private:
  HalFile& file;
  // Fixed stack staging avoids an expansion-sized XHTML heap allocation.
  char buffer[128] = {};
  size_t used = 0;
};

// HTML void elements: legal without a closing tag in HTML, but must be
// self-closed to be well-formed XML.
bool isVoidElement(const char* name, const size_t len) {
  static constexpr const char* VOID_ELEMENTS[] = {"area",  "base", "br",   "col",   "embed",  "hr",    "img",
                                                  "input", "link", "meta", "param", "source", "track", "wbr"};
  return std::any_of(std::begin(VOID_ELEMENTS), std::end(VOID_ELEMENTS),
                     [name, len](const char* v) { return strlen(v) == len && strncmp(v, name, len) == 0; });
}

// True for a well-formed entity reference at html[pos] ('&'): &name; &#123;
// or &#x1F;. On success *end is the index of the ';'.
bool isEntityRef(const std::string& html, const size_t pos, size_t* end) {
  size_t j = pos + 1;
  const size_t n = html.size();
  if (j < n && html[j] == '#') {
    j++;
    if (j < n && (html[j] == 'x' || html[j] == 'X')) j++;
    const size_t digits = j;
    while (j < n && std::isxdigit(static_cast<unsigned char>(html[j]))) j++;
    if (j == digits) return false;
  } else {
    const size_t letters = j;
    while (j < n && std::isalnum(static_cast<unsigned char>(html[j]))) j++;
    if (j == letters) return false;
  }
  if (j >= n || html[j] != ';') return false;
  *end = j;
  return true;
}

// StarDict HTML is tag soup; expat is a strict XML parser. Produce a
// well-formed XHTML document from the fragment: wrap it in a root element,
// lowercase tag names (XML is case-sensitive and the parser matches
// lowercase), self-close void elements (<br> → <br/>), drop stray void
// closers (</br>) and <!…>/<?…> constructs, and escape '&'/'<' characters
// that are not part of markup. Structural damage this cannot repair
// (mismatched tags, unquoted attribute values) surfaces as a parse error and
// the caller falls back to the plain-text path.
// True when everything between `pos` (just past a <li…> tag) and that item's </li> is a single
// <div>…</div>. Only then is unwrapping that div safe: it is the item's whole content, so
// removing the block cannot run the item's text together with a sibling.
bool liContentIsSingleDiv(const std::string& html, const size_t pos) {
  if (html.compare(pos, 5, "<div>") != 0) return false;
  size_t depth = 1;
  size_t k = pos + 5;
  const size_t n = html.size();
  while (k < n && depth > 0) {
    if (html.compare(k, 5, "<div>") == 0) {
      depth++;
      k += 5;
    } else if (html.compare(k, 6, "</div>") == 0) {
      depth--;
      k += 6;
    } else {
      k++;
    }
  }
  return depth == 0 && html.compare(k, 5, "</li>") == 0;
}

bool writeNormalizedXhtml(const std::string& html, HalFile& file) {
  BufferedFileWriter out(file);
  // text-indent:0 on the root: an unstyled block falls back to a first-line indent, which set the
  // entry's text in from the panel's edge while the headword above it sat flush, so the two did
  // not line up. Inherited by every block below (getCombinedBlockStyle passes it down).
  if (!out.append("<html><body><div style=\"text-indent:0\">")) return false;

  const size_t n = html.size();
  size_t i = 0;
  // Nesting state for the sense list. The outermost <ol> holds one sense per <li>; nested lists
  // hold that sense's sub-glosses and must not be treated the same way.
  int listDepth = 0;
  int senseCount = 0;           // top-level items seen; the first shares the page with the header
  bool glossOpen = false;       // a sense's bold gloss block is open
  bool divUnwrapped = false;    // an item's sole <div> wrapper was dropped; drop its closer too
  bool grammarPending = false;  // inside the part-of-speech block; its closer gets a blank line
  while (i < n) {
    const char c = html[i];
    if (c == '<' && i + 1 < n && (html[i + 1] == '!' || html[i + 1] == '?')) {
      // Comment, doctype or processing instruction: drop it entirely.
      const bool isComment = html.compare(i, 4, "<!--") == 0;
      const size_t j = isComment ? html.find("-->", i + 4) : html.find('>', i);
      i = (j == std::string::npos) ? n : j + (isComment ? 3 : 1);
      continue;
    }
    if (c == '<' && i + 1 < n && (html[i + 1] == '/' || std::isalpha(static_cast<unsigned char>(html[i + 1])))) {
      // Find the tag end, honouring quoted attribute values.
      size_t j = i + 1;
      char quote = 0;
      while (j < n) {
        const char d = html[j];
        if (quote) {
          if (d == quote) quote = 0;
        } else if (d == '"' || d == '\'') {
          quote = d;
        } else if (d == '>') {
          break;
        }
        j++;
      }
      if (j == n) {  // unterminated tag: treat the '<' as literal text
        if (!out.append("&lt;")) return false;
        i++;
        continue;
      }

      const bool closing = html[i + 1] == '/';
      const size_t nameStart = i + (closing ? 2 : 1);
      size_t nameEnd = nameStart;
      char nameBuf[16];
      size_t nameLen = 0;
      while (nameEnd < j && std::isalnum(static_cast<unsigned char>(html[nameEnd]))) {
        if (nameLen < sizeof(nameBuf) - 1) {
          nameBuf[nameLen++] = static_cast<char>(std::tolower(static_cast<unsigned char>(html[nameEnd])));
        }
        nameEnd++;
      }
      const bool isVoid = isVoidElement(nameBuf, nameLen);
      if (closing && isVoid) {  // "</br>" — no XML equivalent, drop it
        i = j + 1;
        continue;
      }

      // <font> is presentational and long dead in HTML, but StarDict dictionaries lean on it:
      // FreeDict/WikDict marks pronunciation with <font color="gray"> and part-of-speech with
      // <font class="grammar">. Translate both to a <span> the layout engine actually styles --
      // pronunciation a size down, part-of-speech italic -- so an entry reads as an entry
      // instead of one flat wall of body text.
      if (nameLen == 4 && strncmp(nameBuf, "font", 4) == 0) {
        if (closing) {
          if (!out.append("</span>")) return false;
        } else {
          // Searched in place: a substr() here would heap-allocate once per <font> tag, and a
          // definition carries one per pronunciation and one per part-of-speech.
          const size_t grammarAt = html.find("grammar", nameEnd);
          const bool isGrammar = grammarAt != std::string::npos && grammarAt < j;
          if (!out.append(isGrammar ? "<span style=\"font-style:italic\">" : "<span style=\"font-size:0.75em\">")) {
            return false;
          }
          if (isGrammar) grammarPending = true;
        }
        i = j + 1;
        continue;
      }

      const bool isList = nameLen == 2 && (strncmp(nameBuf, "ol", 2) == 0 || strncmp(nameBuf, "ul", 2) == 0);
      const bool isLi = nameLen == 2 && strncmp(nameBuf, "li", 2) == 0;
      const bool isDiv = nameLen == 3 && strncmp(nameBuf, "div", 3) == 0;

      // A sense's gloss runs until its first block child (the translations) or the end of the
      // item, whichever comes first.
      if (glossOpen && ((!closing && (isList || isDiv)) || (closing && isLi))) {
        if (!out.append("</b></div>")) return false;
        glossOpen = false;
      }

      // The outermost list holds one SENSE per item; the lists nested inside it hold that
      // sense's translations. They are rendered differently, so they are rewritten differently:
      // the outer list loses its markup entirely (a sense is titled by its gloss, not numbered),
      // while nested lists stay real lists and keep their numbering.
      if (isList) {
        if (closing) {
          const bool outermost = listDepth == 1;
          listDepth = std::max(0, listDepth - 1);
          if (outermost) {
            if (!out.append("</div>")) return false;
            i = j + 1;
            continue;
          }
        } else {
          listDepth++;
          if (listDepth == 1) {
            if (!out.append("<div>")) return false;
            i = j + 1;
            continue;
          }
        }
      }

      if (isLi && listDepth == 1) {
        if (closing) {
          if (!out.append("</div>")) return false;  // closes the sense
        } else {
          // Every sense after the first starts its own page, so a lookup is read one definition
          // at a time. The first stays with the pronunciation and part of speech above it.
          if (!out.append(senseCount == 0 ? "<div>" : "<div style=\"page-break-before:always\">")) return false;
          senseCount++;
          // The item's own text is its gloss; bold it in its own block so the translations
          // beneath start on a fresh line without a blank one between.
          if (!out.append("<div><b>")) return false;
          glossOpen = true;
        }
        i = j + 1;
        continue;
      }

      // <li><div>text</div></li>: the block wrapper puts the item's text on the line BELOW its
      // own number. Unwrapping is only safe when that div is the item's entire content -- when
      // the item also carries a gloss ("instance or occurrence<div>Mal</div>"), removing the
      // block runs the two together.
      if (closing && isDiv && divUnwrapped && html.compare(j + 1, 5, "</li>") == 0) {
        divUnwrapped = false;
        i = j + 1;
        continue;
      }

      if (!out.append('<')) return false;
      if (closing && !out.append('/')) return false;
      if (!out.append(nameBuf, nameLen)) return false;
      if (!out.append(html.data() + nameEnd, j - nameEnd)) return false;  // attributes verbatim
      if (!closing && isVoid && html[j - 1] != '/' && !out.append('/')) return false;
      if (!out.append('>')) return false;
      i = j + 1;
      // Paired with the </div></li> case above: drop the wrapper when it is the whole item.
      if (!closing && isLi && liContentIsSingleDiv(html, i)) {
        i += 5;
        divUnwrapped = true;
      }
      // The part of speech is a heading for everything under it; give it a blank line.
      if (closing && isDiv && grammarPending) {
        grammarPending = false;
        if (!out.append("<br/>")) return false;
      }
      continue;
    }
    if (c == '<') {  // stray '<' in text ("x < y")
      if (!out.append("&lt;")) return false;
      i++;
      continue;
    }
    if (c == '&') {
      size_t entityEnd = 0;
      if (isEntityRef(html, i, &entityEnd)) {
        if (!out.append(html.data() + i, entityEnd - i + 1)) return false;
        i = entityEnd + 1;
      } else {  // bare ampersand ("Tom & Jerry")
        if (!out.append("&amp;")) return false;
        i++;
      }
      continue;
    }
    if (!out.append(c)) return false;
    i++;
  }

  return out.append("</div></body></html>") && out.flush();
}

}  // namespace

bool buildDictionaryHtmlPages(GfxRenderer& renderer, const std::string& definition, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const int fontId,
                              std::vector<std::unique_ptr<Page>>& pagesOut) {
  if (ESP.getFreeHeap() < MIN_STYLED_FREE_HEAP || ESP.getMaxAllocHeap() < MIN_STYLED_MAX_ALLOC) {
    LOG_ERR("DHTML", "Low heap for styled definition (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }

  {
    HalFile tmp = Storage.open(TMP_HTML_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!tmp) {
      LOG_ERR("DHTML", "Cannot create %s", TMP_HTML_PATH);
      return false;
    }
    if (!writeNormalizedXhtml(definition, tmp)) {
      LOG_ERR("DHTML", "Short write to %s", TMP_HTML_PATH);
      return false;
    }
  }  // destructor closes the file before the parser reopens the same path

  pagesOut.clear();
  // One fixed allocation (256 bytes on C3); pages must outlive the parser.
  pagesOut.reserve(MAX_STYLED_PAGES);

  bool ok = false;
  bool resourceLimitHit = false;
  size_t retainedElements = 0;
  {
    const std::string tmpPath = TMP_HTML_PATH;  // the parser stores a reference
    // A rule-less parser, present only so the element handlers parse style="" attributes at all:
    // they are guarded on cssParser being non-null, and the dictionary's normalized XHTML carries
    // its whole presentation inline (the sense page breaks, the italic part of speech, the
    // smaller pronunciation). Without it every one of those was silently dropped.
    const CssParser inlineStyleParser{""};
    // Heap-allocated as Section does — the parser object is far too large for
    // a stack local. Null epub is safe: imageRendering=2 suppresses <img>
    // handling, the only path that dereferences it.
    auto parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
        // No extra paragraph spacing: a dictionary entry is a dense list of senses, and the
        // reader's paragraph gap (tuned for prose) pushed a three-line entry over two pages.
        nullptr, tmpPath, renderer, fontId, SETTINGS.getReaderLineCompression(),
        /*extraParagraphSpacing=*/0, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
        SETTINGS.hyphenationEnabled, SETTINGS.focusReadingEnabled,
        // Furigana is per-book reader state (EpubReaderActivity::useFurigana), not a global
        // setting: a dictionary definition is not the book, so it renders without ruby.
        /*furiganaEnabled=*/false,
        [&pagesOut, &resourceLimitHit, &retainedElements](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) {
          if (resourceLimitHit) return;
          const size_t pageElements = page->elements.size();
          if (pagesOut.size() >= MAX_STYLED_PAGES || pageElements > MAX_STYLED_PAGE_ELEMENTS - retainedElements ||
              ESP.getFreeHeap() < MIN_STYLED_FREE_HEAP || ESP.getMaxAllocHeap() < MIN_STYLED_MAX_ALLOC) {
            resourceLimitHit = true;
            pagesOut.clear();
            return;
          }
          retainedElements += pageElements;
          pagesOut.push_back(std::move(page));
        },
        /*embeddedStyle=*/false, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/2,
        /*tocAnchors=*/std::vector<std::string>{}, /*popupFn=*/std::function<void()>{}, &inlineStyleParser);
    if (!parser) {
      LOG_ERR("DHTML", "OOM: ChapterHtmlSlimParser");
    } else {
      ok = parser->parseAndBuildPages();  // closes the file on both outcomes
    }
  }
  Storage.remove(TMP_HTML_PATH);

  if (resourceLimitHit) {
    LOG_ERR("DHTML", "Styled definition exceeded page heap budget");
  }
  if (!ok || resourceLimitHit || pagesOut.empty()) {
    pagesOut.clear();
    return false;
  }
  return true;
}
