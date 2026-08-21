#include "FootnoteTextExtractor.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <expat.h>

#include <cstring>

namespace {

constexpr size_t PARSE_CHUNK = 1024;

// Fallback staging, used only when the target chapter has no cached HTML. It costs a full ZIP
// inflate plus an SD write of the WHOLE file (measured 1138ms against 406ms for the parse, which
// stops the moment the anchor's text is collected), so a chapter's notes -- nearly all pointing
// at one target -- would pay it per lookup. Keep the staged file and reuse it while the target
// is unchanged; the panel drops it on exit via releaseStaging().
std::string stagedPath;  // temp file currently holding a staged item ("" = nothing staged)
std::string stagedItem;  // spine item href staged at stagedPath

struct ExtractState {
  const char* anchor = nullptr;  // empty string = collect <body>
  std::string* out = nullptr;
  size_t maxBytes = 0;
  int depth = 0;
  int captureDepth = -1;  // -1 = not collecting
  bool done = false;
  bool lastWasSpace = true;  // collapse whitespace runs; also trims leading space
  XML_Parser parser = nullptr;

  bool collecting() const { return captureDepth >= 0 && !done; }
};

const char* getAttr(const char** atts, const char* name) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcasecmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void XMLCALL startElement(void* userData, const char* name, const char** atts) {
  auto* st = static_cast<ExtractState*>(userData);
  if (!st->collecting() && !st->done && st->captureDepth < 0) {
    if (st->anchor[0] == '\0') {
      if (strcasecmp(name, "body") == 0) st->captureDepth = st->depth;
    } else {
      const char* id = getAttr(atts, "id");
      if (id && strcmp(id, st->anchor) == 0) st->captureDepth = st->depth;
    }
  }
  st->depth++;
}

void XMLCALL endElement(void* userData, const char* /*name*/) {
  auto* st = static_cast<ExtractState*>(userData);
  st->depth--;
  if (st->collecting() && st->depth == st->captureDepth) {
    // Anchor on an empty inline marker (<a id="x"/> before the note body): almost no text was
    // inside the anchored element itself -- widen the capture to the enclosing block so the
    // note text that FOLLOWS the marker is collected instead of returning an empty panel.
    if (st->out->size() < 8 && st->captureDepth > 0) {
      st->captureDepth--;
      return;
    }
    st->done = true;
    XML_StopParser(st->parser, XML_FALSE);
  }
}

void XMLCALL characterData(void* userData, const char* s, const int len) {
  auto* st = static_cast<ExtractState*>(userData);
  if (!st->collecting()) return;
  for (int i = 0; i < len; i++) {
    const char c = s[i];
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!st->lastWasSpace) {
        st->out->push_back(' ');
        st->lastWasSpace = true;
      }
      continue;
    }
    st->out->push_back(c);
    st->lastWasSpace = false;
    if (st->out->size() >= st->maxBytes) {
      // Cap hit: back off to the last full UTF-8 sequence and stop.
      while (!st->out->empty() && (static_cast<unsigned char>(st->out->back()) & 0xC0) == 0x80) {
        st->out->pop_back();
      }
      if (!st->out->empty() && static_cast<unsigned char>(st->out->back()) >= 0xC0) st->out->pop_back();
      st->out->append("\xe2\x80\xa6");  // ellipsis
      st->done = true;
      XML_StopParser(st->parser, XML_FALSE);
      return;
    }
  }
}

}  // namespace

namespace FootnoteText {

bool extract(Epub& epub, const int currentSpineIndex, const std::string& href, std::string& out,
             const size_t maxBytes) {
  out.clear();

  std::string anchor;
  const size_t hashPos = href.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < href.size()) anchor = href.substr(hashPos + 1);

  const bool sameFile = !href.empty() && href[0] == '#';
  const int spineIdx = sameFile ? currentSpineIndex : epub.resolveHrefToSpineIndex(href);
  if (spineIdx < 0) {
    LOG_DBG("FNX", "Could not resolve footnote href: %s", href.c_str());
    return false;
  }
  const std::string itemHref = epub.getSpineItem(spineIdx).href;

  // The section builder already keeps every chapter it has laid out as raw inflated HTML under
  // <cache>/html/<spine>.html, keyed on the book alone and known-complete once it exists. A
  // footnote target that has been read therefore needs no inflate at all -- which is also what
  // makes the panel survive a tight heap, since the inflate is what fails first there.
  // Otherwise stage the item ourselves, reusing the previous lookup's staging when the target
  // is unchanged (a chapter's notes nearly all share one).
  const std::string cachedHtml = epub.getCachePath() + "/html/" + std::to_string(spineIdx) + ".html";
  const unsigned long stageStart = millis();
  const bool cacheHit = Storage.exists(cachedHtml.c_str());
  const std::string parsePath = cacheHit ? cachedHtml : epub.getCachePath() + "/fn_tmp.html";
  const bool restaged = !cacheHit && (stagedPath != parsePath || stagedItem != itemHref);
  if (restaged) {
    releaseStaging();
    HalFile tmp;
    if (!Storage.openFileForWrite("FNX", parsePath, tmp)) return false;
    if (!epub.readItemContentsToStream(itemHref, tmp, PARSE_CHUNK)) {
      tmp.close();
      Storage.remove(parsePath.c_str());
      LOG_DBG("FNX", "Failed to stream %s", itemHref.c_str());
      return false;
    }
    stagedPath = parsePath;
    stagedItem = itemHref;
  }
  const unsigned long stageMs = millis() - stageStart;

  ExtractState st;
  st.anchor = anchor.c_str();
  st.out = &out;
  st.maxBytes = maxBytes;

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    releaseStaging();
    return false;
  }
  st.parser = parser;
  XML_SetUserData(parser, &st);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  const unsigned long parseStart = millis();
  bool ok = true;
  {
    HalFile f;
    if (!Storage.openFileForRead("FNX", parsePath, f)) {
      ok = false;
    } else {
      char buf[PARSE_CHUNK];
      for (;;) {
        const int len = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        const bool last = len < static_cast<int>(sizeof(buf));
        if (XML_Parse(parser, buf, len < 0 ? 0 : len, last) == XML_STATUS_ERROR) {
          // XML_StopParser surfaces as an ABORTED error -- that's our own early exit.
          if (XML_GetErrorCode(parser) != XML_ERROR_ABORTED) {
            LOG_DBG("FNX", "Parse error in %s: %s", itemHref.c_str(), XML_ErrorString(XML_GetErrorCode(parser)));
            ok = st.done;  // text collected before a late parse error is still usable
          }
          break;
        }
        if (last || st.done) break;
      }
    }
  }
  XML_ParserFree(parser);
  const char* source = cacheHit ? "html cache" : (restaged ? "streamed" : "reused");
  LOG_DBG("FNX", "Extract %s: stage=%lums (%s) parse=%lums", href.c_str(), stageMs, source, millis() - parseStart);

  while (!out.empty() && out.back() == ' ') out.pop_back();
  return ok && !out.empty();
}

void releaseStaging() {
  if (stagedPath.empty()) return;
  Storage.remove(stagedPath.c_str());
  stagedPath.clear();
  stagedItem.clear();
}

}  // namespace FootnoteText
