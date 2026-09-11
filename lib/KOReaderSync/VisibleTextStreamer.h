#pragma once

// The visible-text counting side of KOSync position mapping: the XPath parse helpers and the
// streamer that walks a chapter's bytes to turn a KOReader XPath into a visible-codepoint offset.
//
// It lives in its own header so the round trip can be tested on the host: ProgressMapper.cpp
// itself pulls in Epub, Section and the renderer, none of which a counting test needs. The rules
// here must stay in step with ChapterHtmlSlimParser (which produces the offsets) and
// ChapterXPathResolver (which produces the anchors) -- see VisibleTextUtils.

#include <Epub/VisibleTextUtils.h>
#include <Epub/htmlEntities.h>
#include <Print.h>
#include <Utf8.h>
#include <strings.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace koreader_sync {

int parseIndex(const std::string& xpath, const char* prefix, bool last = false) {
  const size_t prefixLen = strlen(prefix);
  const size_t pos = last ? xpath.rfind(prefix) : xpath.find(prefix);
  if (pos == std::string::npos) return -1;
  const size_t numStart = pos + prefixLen;
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return -1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return -1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

int parseCharOffset(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()");
  const size_t dotPos = (textPos != std::string::npos) ? xpath.find('.', textPos) : xpath.rfind('.');
  if (dotPos == std::string::npos || dotPos + 1 >= xpath.size()) return 0;
  int val = 0;
  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 0;
    val = val * 10 + (xpath[i] - '0');
  }
  return val;
}

// Parse the N from text()[N] in the XPath (1-based; defaults to 1 if absent or 1).
int parseTextNodeIndex(const std::string& xpath) {
  const size_t textPos = xpath.rfind("text()[");
  if (textPos == std::string::npos) return 1;
  const size_t numStart = textPos + 7;  // strlen("text()[")
  const size_t numEnd = xpath.find(']', numStart);
  if (numEnd == std::string::npos || numEnd == numStart) return 1;
  int val = 0;
  for (size_t i = numStart; i < numEnd; i++) {
    if (xpath[i] < '0' || xpath[i] > '9') return 1;
    val = val * 10 + (xpath[i] - '0');
  }
  return val > 0 ? val : 1;
}

bool isChapterStartXPath(const std::string& xpath) {
  if (xpath.find("/p[") != std::string::npos || xpath.find("/li[") != std::string::npos) {
    return false;
  }

  static constexpr char kDocFragment[] = "/body/DocFragment[";
  const size_t docFragPos = xpath.find(kDocFragment);
  if (docFragPos == std::string::npos) {
    return false;
  }
  const size_t docFragEnd = xpath.find(']', docFragPos + strlen(kDocFragment));
  if (docFragEnd == std::string::npos) {
    return false;
  }
  if (docFragEnd + 1 == xpath.size()) {
    return true;
  }
  if (xpath[docFragEnd + 1] == '.') {
    if (docFragEnd + 2 >= xpath.size()) {
      return false;
    }
    for (size_t i = docFragEnd + 2; i < xpath.size(); i++) {
      if (xpath[i] != '0') return false;
    }
    return true;
  }

  static constexpr char kDocBody[] = "]/body";
  const size_t docBodyPos = xpath.find(kDocBody);
  if (docBodyPos == std::string::npos) {
    return false;
  }
  size_t bodyContentStart = docBodyPos + strlen(kDocBody);
  if (bodyContentStart == xpath.size()) {
    return true;
  }
  if (xpath[bodyContentStart] != '/') {
    return false;
  }
  bodyContentStart++;
  if (bodyContentStart == xpath.size()) {
    return true;
  }

  const size_t dotPos = xpath.rfind('.');
  if (dotPos == std::string::npos || dotPos <= bodyContentStart || dotPos + 1 >= xpath.size()) {
    return false;
  }
  size_t terminalEnd = dotPos;
  static constexpr char kTextNode[] = "/text()";
  const size_t textNodePos = xpath.rfind(kTextNode, dotPos);
  if (textNodePos != std::string::npos && textNodePos >= bodyContentStart) {
    terminalEnd = textNodePos;
  }
  if (xpath.find('/', bodyContentStart) < terminalEnd) {
    return false;
  }

  for (size_t i = dotPos + 1; i < xpath.size(); i++) {
    if (xpath[i] != '0') return false;
  }
  return parseTextNodeIndex(xpath) <= 1;
}

bool isBodyTextXPath(const std::string& xpath) {
  static constexpr char kBodyFrag[] = "/body/DocFragment[";
  const size_t fragPos = xpath.find(kBodyFrag);
  if (fragPos == std::string::npos) return false;
  const size_t afterBracket = xpath.find(']', fragPos + strlen(kBodyFrag));
  if (afterBracket == std::string::npos) return false;
  static constexpr char kBody[] = "/body/";
  if (xpath.compare(afterBracket + 1, strlen(kBody), kBody) != 0) return false;
  const size_t contentPos = afterBracket + 1 + strlen(kBody);
  return xpath.compare(contentPos, strlen("text()"), "text()") == 0;
}

// Parsed representation of one step in the XPath ancestry.
struct XPathStep {
  char tag[12];      // element name, null-terminated
  int siblingIndex;  // 1-based sibling index, or 0 if unspecified (match any)
};

static constexpr int MAX_XPATH_DEPTH = 16;

// Parse the XPath segment between /body/DocFragment[N]/body/ and the terminal position
// into an ordered sequence of steps. Returns step count, 0 on failure.
// Example input: "/body/DocFragment[1]/body/div[1]/ul/li[4]/text()[1].51"
// Fills steps with: {div,1}, {ul,1}, {li,4}
int parseXPathSteps(const std::string& xpath, XPathStep steps[MAX_XPATH_DEPTH]) {
  static const char kBodyFrag[] = "/body/DocFragment[";
  const size_t fragPos = xpath.find(kBodyFrag);
  if (fragPos == std::string::npos) return 0;
  const size_t afterBracket = xpath.find(']', fragPos + strlen(kBodyFrag));
  if (afterBracket == std::string::npos) return 0;
  static const char kBody[] = "/body/";
  if (xpath.compare(afterBracket + 1, strlen(kBody), kBody) != 0) return 0;
  size_t pos = afterBracket + 1 + strlen(kBody);

  size_t stepsEnd = xpath.rfind("/text()");
  if (stepsEnd == std::string::npos) {
    stepsEnd = xpath.rfind('.');
    if (stepsEnd == std::string::npos || stepsEnd <= pos || stepsEnd + 1 >= xpath.size()) return 0;
    for (size_t i = stepsEnd + 1; i < xpath.size(); i++) {
      if (xpath[i] < '0' || xpath[i] > '9') return 0;
    }
  }
  if (stepsEnd <= pos) return 0;

  int count = 0;
  while (pos < stepsEnd && count < MAX_XPATH_DEPTH) {
    const size_t slash = xpath.find('/', pos);
    const size_t segEnd = (slash < stepsEnd) ? slash : stepsEnd;

    XPathStep& step = steps[count];
    const size_t bracket = xpath.find('[', pos);
    const size_t nameEnd = (bracket != std::string::npos && bracket < segEnd) ? bracket : segEnd;
    const size_t nameLen = nameEnd - pos;
    if (nameLen == 0 || nameLen >= sizeof(step.tag)) return 0;
    memcpy(step.tag, xpath.c_str() + pos, nameLen);
    step.tag[nameLen] = '\0';

    if (bracket != std::string::npos && bracket < segEnd) {
      const size_t closeBracket = xpath.find(']', bracket + 1);
      if (closeBracket == std::string::npos || closeBracket > segEnd) return 0;
      int idx = 0;
      for (size_t i = bracket + 1; i < closeBracket; i++) {
        if (xpath[i] < '0' || xpath[i] > '9') return 0;
        idx = idx * 10 + (xpath[i] - '0');
      }
      step.siblingIndex = idx;
    } else {
      step.siblingIndex = 0;
    }

    count++;
    pos = (slash < stepsEnd) ? slash + 1 : stepsEnd;
  }
  return count;
}

class ParagraphStreamer final : public Print {
  size_t bytesWritten = 0;
  bool globalInTag = false;
  bool globalInEntity = false;
  static constexpr size_t MAX_ENTITY_SIZE = 16;
  char entityBuffer[MAX_ENTITY_SIZE] = {};
  size_t entityLen = 0;
  bool prevCR = false;  // last counted visible byte was a CR (XML line-ending normalization)

  // Forward mode: count <p> paragraphs at a byte offset (legacy, used by generateXPath)
  size_t fwdTarget;
  int fwdResult = 0;
  bool fwdCaptured = false;

  // Reverse mode shared state
  int revChar;
  bool revPFound = false;
  bool revDone = false;
  int revVisChars = 0;
  size_t totalVisChars = 0;
  size_t targetVisChars = 0;

  // --- Legacy reverse mode (paragraph index only, no ancestry) ---
  int revParagraph = 0;
  int pCount = 0;
  int paragraphAtMatch = 0;
  int liCount = 0;
  int liCountAtMatch = 0;
  int targetTextNode = 1;
  int currentTextNode = 0;
  int paragraphHtmlDepth = -1;

  // --- Ancestry-aware reverse mode ---
  const XPathStep* steps = nullptr;
  int stepCount = 0;
  int siblingCounters[MAX_XPATH_DEPTH] = {};
  bool insideStep[MAX_XPATH_DEPTH] = {};
  int htmlDepth = 0;
  int bodyHtmlDepth = -1;
  int stepEnteredAtDepth[MAX_XPATH_DEPTH] = {};
  bool relaxFirstStepDepth = false;

  // Tag name accumulation
  enum TagParseState { TAG_IDLE, TAG_IN_NAME, TAG_ATTRS } tagState = TAG_IDLE;
  bool tagIsClose = false;
  char tagName[12] = {};
  int tagNameLen = 0;

  int matchedDepth = 0;

  // Anchor ID capture
  static constexpr int MAX_ANCHOR_ID = 64;
  char capturedAnchorId[MAX_ANCHOR_ID] = {};
  int capturedAnchorIdLen = 0;
  bool capturingAnchorTag = false;
  enum AnchorAttrState {
    ATTR_FIND_NAME,
    ATTR_READ_NAME,
    ATTR_AFTER_NAME,
    ATTR_BEFORE_VALUE,
    ATTR_CAPTURE_D,
    ATTR_CAPTURE_S
  } attrState = ATTR_FIND_NAME;
  uint8_t attrNameLen = 0;
  bool currentAttrIsId = false;
  bool inAttrQuote =
      false;  // true while inside a quoted attribute value (prevents '/' from being treated as self-close)
  char attrQuoteChar = 0;
  uint8_t nonVisibleDepth = 0;
  bool insideBody = false;

  // Attribute scan for the shared skipped-subtree rule. onOpenTag() fires at the end of the tag
  // NAME, before any attribute is seen, so the decision is applied at '>' instead.
  static constexpr int MAX_ATTR_NAME = 12;
  static constexpr int MAX_ATTR_VALUE = 16;
  char skipAttrName[MAX_ATTR_NAME] = {};
  char skipAttrValue[MAX_ATTR_VALUE] = {};
  uint8_t skipAttrNameLen = 0;
  uint8_t skipAttrValueLen = 0;
  enum SkipAttrState {
    SKIP_ATTR_NAME,
    SKIP_ATTR_PRE_EQ,
    SKIP_ATTR_PRE_VALUE,
    SKIP_ATTR_VALUE
  } skipAttrState = SKIP_ATTR_NAME;
  char skipAttrQuote = 0;
  bool tagStartsSkippedSubtree = false;
  bool tagSelfClosing = false;

  // Comments, processing instructions and CDATA sections. ChapterXPathResolver treats each as a
  // text-node boundary and counts CDATA content as visible text, so an anchor it emits after one
  // of these is only resolvable here if this scanner agrees.
  enum MarkupMode { MARKUP_NONE, MARKUP_DECIDE, MARKUP_BANG, MARKUP_COMMENT, MARKUP_PI, MARKUP_CDATA };
  MarkupMode markupMode = MARKUP_NONE;
  static constexpr int MAX_MARKUP_PREFIX = 8;  // "[CDATA[" plus room for the decision byte
  char markupPrefix[MAX_MARKUP_PREFIX + 1] = {};
  uint8_t markupPrefixLen = 0;
  uint8_t markupEndMatch = 0;
  bool pendingTextNodeBoundary = false;
  bool targetBodyText = false;

  bool isNonVisibleTag() const { return VisibleTextUtils::isNonVisibleElement(tagName); }

  static bool isAttrWhitespace(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

  static bool isAttrNameChar(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == ':' || c == '.';
  }

  void resetAnchorAttrScan() {
    attrState = ATTR_FIND_NAME;
    attrNameLen = 0;
    currentAttrIsId = false;
  }

  void finishCapturedAnchorId() {
    capturedAnchorId[capturedAnchorIdLen] = '\0';
    capturingAnchorTag = false;
    resetAnchorAttrScan();
  }

  void beginAnchorIdScan() {
    capturingAnchorTag = true;
    resetAnchorAttrScan();
  }

  void endAnchorIdScan() {
    if (capturingAnchorTag) {
      capturedAnchorIdLen = 0;
    }
    capturingAnchorTag = false;
    resetAnchorAttrScan();
  }

  void appendCapturedAnchorId(uint8_t c) {
    if (capturedAnchorIdLen + 1 < MAX_ANCHOR_ID) {
      capturedAnchorId[capturedAnchorIdLen++] = c;
    }
  }

  void scanAnchorAttribute(uint8_t c) {
    switch (attrState) {
      case ATTR_FIND_NAME:
        if (isAttrNameChar(c)) {
          attrState = ATTR_READ_NAME;
          attrNameLen = 1;
          currentAttrIsId = c == 'i';
        }
        break;
      case ATTR_READ_NAME:
        if (isAttrNameChar(c)) {
          if (attrNameLen == 1) {
            currentAttrIsId = currentAttrIsId && c == 'd';
          } else {
            currentAttrIsId = false;
          }
          attrNameLen++;
        } else {
          currentAttrIsId = currentAttrIsId && attrNameLen == 2;
          if (isAttrWhitespace(c)) {
            attrState = ATTR_AFTER_NAME;
          } else if (c == '=') {
            attrState = ATTR_BEFORE_VALUE;
          } else {
            resetAnchorAttrScan();
          }
        }
        break;
      case ATTR_AFTER_NAME:
        if (isAttrWhitespace(c)) {
          break;
        }
        if (c == '=') {
          attrState = ATTR_BEFORE_VALUE;
        } else if (isAttrNameChar(c)) {
          attrState = ATTR_READ_NAME;
          attrNameLen = 1;
          currentAttrIsId = c == 'i';
        } else {
          resetAnchorAttrScan();
        }
        break;
      case ATTR_BEFORE_VALUE:
        if (isAttrWhitespace(c)) {
          break;
        }
        if (currentAttrIsId && c == '"') {
          capturedAnchorIdLen = 0;
          attrState = ATTR_CAPTURE_D;
        } else if (currentAttrIsId && c == '\'') {
          capturedAnchorIdLen = 0;
          attrState = ATTR_CAPTURE_S;
        } else if (c == '"') {
          attrState = ATTR_CAPTURE_D;
        } else if (c == '\'') {
          attrState = ATTR_CAPTURE_S;
        } else {
          resetAnchorAttrScan();
        }
        break;
      case ATTR_CAPTURE_D:
        if (c == '"') {
          if (currentAttrIsId) {
            finishCapturedAnchorId();
          } else {
            resetAnchorAttrScan();
          }
        } else if (currentAttrIsId) {
          appendCapturedAnchorId(c);
        }
        break;
      case ATTR_CAPTURE_S:
        if (c == '\'') {
          if (currentAttrIsId) {
            finishCapturedAnchorId();
          } else {
            resetAnchorAttrScan();
          }
        } else if (currentAttrIsId) {
          appendCapturedAnchorId(c);
        }
        break;
    }
  }

  void resetSkipAttrScan() {
    skipAttrState = SKIP_ATTR_NAME;
    skipAttrNameLen = 0;
    skipAttrValueLen = 0;
    skipAttrQuote = 0;
  }

  void evaluateSkipAttribute() {
    skipAttrName[skipAttrNameLen] = '\0';
    skipAttrValue[skipAttrValueLen] = '\0';
    if (skipAttrNameLen > 0 && VisibleTextUtils::isSkippedSubtreeAttribute(skipAttrName, skipAttrValue)) {
      tagStartsSkippedSubtree = true;
    }
  }

  // Walks an open tag's attributes byte by byte: name, optional ="value". Names and values longer
  // than the buffers cannot match any rule, so truncation only ever fails to skip.
  void scanSkipAttribute(const uint8_t c) {
    switch (skipAttrState) {
      case SKIP_ATTR_NAME:
        if (isAttrWhitespace(c) || c == '=' || c == '/') {
          if (skipAttrNameLen > 0) {
            if (c == '=') {
              skipAttrState = SKIP_ATTR_PRE_VALUE;
            } else {
              evaluateSkipAttribute();
              skipAttrState = isAttrWhitespace(c) ? SKIP_ATTR_PRE_EQ : SKIP_ATTR_NAME;
              if (skipAttrState == SKIP_ATTR_NAME) resetSkipAttrScan();
            }
          }
          break;
        }
        if (skipAttrNameLen + 1 < MAX_ATTR_NAME) skipAttrName[skipAttrNameLen++] = static_cast<char>(c);
        break;
      case SKIP_ATTR_PRE_EQ:
        if (isAttrWhitespace(c)) break;
        if (c == '=') {
          skipAttrState = SKIP_ATTR_PRE_VALUE;
          break;
        }
        // A bare attribute (<p hidden>) followed by the next name.
        evaluateSkipAttribute();
        resetSkipAttrScan();
        if (!isAttrWhitespace(c) && c != '/') skipAttrName[skipAttrNameLen++] = static_cast<char>(c);
        break;
      case SKIP_ATTR_PRE_VALUE:
        if (isAttrWhitespace(c)) break;
        if (c == '"' || c == '\'') {
          skipAttrQuote = static_cast<char>(c);
          skipAttrState = SKIP_ATTR_VALUE;
          break;
        }
        skipAttrQuote = 0;
        skipAttrState = SKIP_ATTR_VALUE;
        if (skipAttrValueLen + 1 < MAX_ATTR_VALUE) skipAttrValue[skipAttrValueLen++] = static_cast<char>(c);
        break;
      case SKIP_ATTR_VALUE:
        if ((skipAttrQuote != 0 && c == skipAttrQuote) || (skipAttrQuote == 0 && isAttrWhitespace(c))) {
          evaluateSkipAttribute();
          resetSkipAttrScan();
          break;
        }
        if (skipAttrValueLen + 1 < MAX_ATTR_VALUE) skipAttrValue[skipAttrValueLen++] = static_cast<char>(c);
        break;
    }
  }

  // The resolver starts a new text()[N] on the first non-empty text after a markup boundary, so
  // the index is advanced here rather than eagerly at the boundary itself.
  void applyPendingTextNodeBoundary() {
    if (!pendingTextNodeBoundary) return;
    pendingTextNodeBoundary = false;
    if (!revPFound || revDone) return;
    const bool insideMatchedElement =
        (stepCount > 0) ? (matchedDepth == stepCount && htmlDepth == stepEnteredAtDepth[stepCount - 1])
                        : (paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth);
    if (!insideMatchedElement) return;
    currentTextNode++;
    if (currentTextNode == targetTextNode && revChar <= 0) {
      targetVisChars = totalVisChars;
      revDone = true;
    }
  }

  void onVisibleCodepoint() {
    applyPendingTextNodeBoundary();
    totalVisChars++;
    if (revPFound && !revDone) {
      // Ancestry mode: count only while inside the fully-matched element and in the target text node.
      // Legacy mode: count only while still inside the matched paragraph and in the target text node.
      const bool inTargetNode =
          (stepCount > 0)
              ? (matchedDepth == stepCount && htmlDepth == stepEnteredAtDepth[stepCount - 1] &&
                 currentTextNode == targetTextNode)
              : (paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth && currentTextNode == targetTextNode);
      if (inTargetNode) {
        revVisChars++;
        if (revVisChars >= revChar) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
    }
  }

  void onVisibleText(const char* text) {
    if (!text) return;
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text);
    while (*ptr != 0) {
      utf8NextCodepoint(&ptr);
      onVisibleCodepoint();
    }
  }

  void flushEntityAsLiteral() {
    for (size_t i = 0; i < entityLen; i++) onVisibleCodepoint();
  }

  void finishEntity() {
    entityBuffer[entityLen] = '\0';
    const char* resolved = lookupHtmlEntity(entityBuffer, entityLen);
    if (resolved)
      onVisibleText(resolved);
    else if (entityLen >= 3 && entityBuffer[1] == '#')
      // Numeric character reference (&#NNN; / &#xHH;): expat -- which builds the page LUT --
      // decodes it to a single codepoint. Count one here too, not the raw "&#NNN" characters.
      onVisibleCodepoint();
    else
      flushEntityAsLiteral();
    globalInEntity = false;
    entityLen = 0;
  }

  void onLegacyP() {
    pCount++;
    if (!revPFound && revParagraph > 0 && pCount >= revParagraph) {
      revPFound = true;
      revVisChars = 0;
      paragraphHtmlDepth = htmlDepth;
      currentTextNode = 1;
      if (revChar <= 0 && targetTextNode <= 1) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
  }

  void onOpenTag() {
    htmlDepth++;
    // A markup boundary only starts a new text node for text that directly follows it; an
    // intervening tag supersedes it (and stops a boundary seen before the match from leaking in).
    pendingTextNodeBoundary = false;

    if (strcasecmp(tagName, "body") == 0) {
      insideBody = true;
      bodyHtmlDepth = htmlDepth;
      if (targetBodyText) {
        revPFound = true;
        paragraphHtmlDepth = htmlDepth;
        currentTextNode = 1;
        if (revChar <= 0 && targetTextNode <= 1) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
      return;
    }
    if (!insideBody) return;

    if (nonVisibleDepth > 0 || isNonVisibleTag()) {
      nonVisibleDepth++;
      return;
    }

    if (stepCount == 0) {
      if (strcasecmp(tagName, "p") == 0) onLegacyP();
      return;
    }

    // Capture a child <a id> inside the fully-matched element even after target char is found.
    if (revPFound && matchedDepth == stepCount && capturedAnchorIdLen == 0 && strcasecmp(tagName, "a") == 0) {
      beginAnchorIdScan();
    }

    if (revDone) return;

    if (strcasecmp(tagName, "p") == 0) pCount++;
    if (strcasecmp(tagName, "li") == 0) liCount++;

    if (matchedDepth < stepCount) {
      const XPathStep& target = steps[matchedDepth];
      if (strcasecmp(tagName, target.tag) == 0) {
        // Count only direct children of the previously matched ancestor step.
        // For step 0 any depth is valid; subsequent steps must be exactly one level deeper.
        const bool atCorrectDepth = (matchedDepth == 0) ? (relaxFirstStepDepth || htmlDepth == bodyHtmlDepth + 1)
                                                        : (htmlDepth == stepEnteredAtDepth[matchedDepth - 1] + 1);
        if (!atCorrectDepth) return;
        siblingCounters[matchedDepth]++;
        if (target.siblingIndex == 0 || siblingCounters[matchedDepth] == target.siblingIndex) {
          insideStep[matchedDepth] = true;
          stepEnteredAtDepth[matchedDepth] = htmlDepth;
          matchedDepth++;
          if (matchedDepth == stepCount) {
            beginAnchorIdScan();
            paragraphAtMatch = pCount;
            liCountAtMatch = liCount;
            revPFound = true;
            capturedAnchorIdLen = 0;
            revVisChars = 0;
            currentTextNode = 1;  // Reset text node counter for this element
            if (revChar <= 0 && targetTextNode <= 1) {
              targetVisChars = totalVisChars;
              revDone = true;
            }
          }
        }
      }
    }
  }

  void onCloseTag() {
    pendingTextNodeBoundary = false;
    if (strcasecmp(tagName, "body") == 0) {
      insideBody = false;
      if (htmlDepth > 0) htmlDepth--;
      return;
    }
    if (!insideBody) {
      if (htmlDepth > 0) htmlDepth--;
      return;
    }

    if (nonVisibleDepth > 0) {
      nonVisibleDepth--;
      if (htmlDepth > 0) htmlDepth--;
      return;
    }

    // Legacy mode: each direct child element closing advances the text node index.
    if (stepCount == 0 && revPFound && !revDone && paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth + 1) {
      currentTextNode++;
      if (currentTextNode == targetTextNode && revChar <= 0) {
        targetVisChars = totalVisChars;
        revDone = true;
      }
    }
    // Legacy mode: stop tracking when the matched paragraph itself closes.
    if (stepCount == 0 && revPFound && !revDone && paragraphHtmlDepth >= 0 && htmlDepth == paragraphHtmlDepth) {
      revPFound = false;
      paragraphHtmlDepth = -1;
    }

    // Ancestry mode: advance text node when a direct child of the fully-matched element closes.
    if (stepCount > 0 && matchedDepth == stepCount && revPFound && !revDone) {
      const int elementDepth = stepEnteredAtDepth[stepCount - 1];
      if (htmlDepth == elementDepth + 1) {
        currentTextNode++;
        if (currentTextNode == targetTextNode && revChar <= 0) {
          targetVisChars = totalVisChars;
          revDone = true;
        }
      }
    }

    if (stepCount > 0 && matchedDepth > 0) {
      const int step = matchedDepth - 1;
      if (insideStep[step] && htmlDepth == stepEnteredAtDepth[step]) {
        insideStep[step] = false;
        matchedDepth--;
        // If the fully-matched element just closed without finding the target, abort.
        if (matchedDepth < stepCount && revPFound && !revDone) {
          revPFound = false;
        }
        for (int i = matchedDepth + 1; i < stepCount; i++) {
          siblingCounters[i] = 0;
          insideStep[i] = false;
          stepEnteredAtDepth[i] = -1;
        }
      }
    }
    if (htmlDepth > 0) htmlDepth--;
  }

  // Leaves the construct AND the tag scanner it shares '<' with: without clearing globalInTag the
  // text after a comment would be read as tag content and never counted.
  void endMarkup(const bool isTextNodeBoundary) {
    markupMode = MARKUP_NONE;
    globalInTag = false;
    tagState = TAG_IDLE;
    markupEndMatch = 0;
    if (isTextNodeBoundary) pendingTextNodeBoundary = true;
  }

  // Runs for every byte from '<' until the construct is decided, and then to its terminator.
  // Ordinary tags hand control straight back to processByteInTag().
  void processMarkupByte(const uint8_t c, const bool afterCR) {
    switch (markupMode) {
      case MARKUP_DECIDE:
        if (c == '?') {
          markupMode = MARKUP_PI;
          markupEndMatch = 0;
          return;
        }
        if (c == '!') {
          markupMode = MARKUP_BANG;
          markupPrefixLen = 0;
          return;
        }
        // An ordinary tag: replay this byte through the tag scanner.
        markupMode = MARKUP_NONE;
        writeOutsideMarkup(c, afterCR);
        return;

      case MARKUP_BANG:
        if (markupPrefixLen < MAX_MARKUP_PREFIX) markupPrefix[markupPrefixLen++] = static_cast<char>(c);
        markupPrefix[markupPrefixLen] = '\0';
        if (markupPrefixLen == 2 && std::strncmp(markupPrefix, "--", 2) == 0) {
          markupMode = MARKUP_COMMENT;
          markupEndMatch = 0;
        } else if (markupPrefixLen == 7 && std::strncmp(markupPrefix, "[CDATA[", 7) == 0) {
          markupMode = MARKUP_CDATA;
          markupEndMatch = 0;
          // CDATA content is text, and the section itself is a node boundary.
          pendingTextNodeBoundary = true;
        } else if ((markupPrefixLen >= 2 && markupPrefix[0] != '-' && markupPrefix[0] != '[') ||
                   markupPrefixLen == MAX_MARKUP_PREFIX) {
          // <!DOCTYPE …> and friends: no text, ends at the first '>'.
          markupMode = MARKUP_BANG;
          if (c == '>') endMarkup(false);
        } else if (c == '>') {
          endMarkup(false);
        }
        return;

      case MARKUP_COMMENT:
        if ((markupEndMatch < 2 && c == '-') || (markupEndMatch == 2 && c == '>')) {
          markupEndMatch++;
          if (markupEndMatch == 3) endMarkup(true);
          return;
        }
        markupEndMatch = (c == '-') ? 1 : 0;
        return;

      case MARKUP_PI:
        if (markupEndMatch == 0 && c == '?') {
          markupEndMatch = 1;
          return;
        }
        if (markupEndMatch == 1 && c == '>') {
          endMarkup(true);
          return;
        }
        markupEndMatch = (c == '?') ? 1 : 0;
        return;

      case MARKUP_CDATA:
        if ((markupEndMatch < 2 && c == ']') || (markupEndMatch == 2 && c == '>')) {
          markupEndMatch++;
          if (markupEndMatch == 3) endMarkup(true);
          return;
        }
        // A ']' that did not start the terminator is content, so count what was held back.
        for (uint8_t i = 0; i < markupEndMatch; i++) countCdataByte(']', false);
        markupEndMatch = (c == ']') ? 1 : 0;
        if (markupEndMatch == 0) countCdataByte(c, afterCR);
        return;

      case MARKUP_NONE:
        break;
    }
  }

  // CDATA has no entities and no markup: every byte is literal text, counted with the same
  // codepoint and line-ending rules as ordinary character data.
  void countCdataByte(const uint8_t c, const bool afterCR) {
    if (!insideBody || nonVisibleDepth > 0) return;
    if (c == '\n' && afterCR) return;
    if ((c & 0xC0) != 0x80) onVisibleCodepoint();
    prevCR = (c == '\r');
  }

  void processByteInTag(uint8_t c) {
    switch (tagState) {
      case TAG_IDLE:
        if (c == '/') {
          tagIsClose = true;
          tagState = TAG_IN_NAME;
        } else if (c != '!' && c != '?') {
          tagIsClose = false;
          tagName[0] = static_cast<char>(c);
          tagNameLen = 1;
          tagState = TAG_IN_NAME;
        }
        break;
      case TAG_IN_NAME:
        if (c == '>' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
          tagName[tagNameLen] = '\0';
          if (tagNameLen > 0) {
            if (tagIsClose)
              onCloseTag();
            else
              onOpenTag();
            // Self-closing open tag (<br/>). Don't double-fire for close tags (</br/>).
            if (c == '/' && !tagIsClose) onCloseTag();
          }
          tagNameLen = 0;
          tagState = (c == '>') ? TAG_IDLE : TAG_ATTRS;
        } else if (tagNameLen + 1 < static_cast<int>(sizeof(tagName))) {
          tagName[tagNameLen++] = static_cast<char>(c);
        }
        break;
      case TAG_ATTRS:
        if (!tagIsClose) {
          scanSkipAttribute(c);
          if (!inAttrQuote) {
            // A self-closing tag has no matching close tag, so a skip started here would never
            // be cleared.
            if (c == '/') {
              tagSelfClosing = true;
            } else if (!isAttrWhitespace(c)) {
              tagSelfClosing = false;
            }
          }
        }
        // Track quoted attribute values so '/' inside them is not mistaken for self-closing.
        if (!inAttrQuote) {
          if (c == '"' || c == '\'') {
            inAttrQuote = true;
            attrQuoteChar = c;
          }
        } else if (c == attrQuoteChar) {
          inAttrQuote = false;
          attrQuoteChar = 0;
        }
        if (capturingAnchorTag) {
          scanAnchorAttribute(c);
        }
        // Only treat '/' as self-closing when outside a quoted attribute value.
        if (c == '/' && !inAttrQuote) {
          endAnchorIdScan();
          onCloseTag();
        }
        break;
    }
  }

 public:
  explicit ParagraphStreamer(size_t targetByte) : fwdTarget(targetByte), revChar(0) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(int paragraph, int charOff, int textNodeIdx = 1)
      : fwdTarget(SIZE_MAX), revChar(charOff), revParagraph(paragraph), targetTextNode(textNodeIdx) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(const XPathStep* xpathSteps, int xpathStepCount, int charOff, int textNodeIdx = 1,
                    bool relaxFirstStep = false)
      : fwdTarget(SIZE_MAX),
        revChar(charOff),
        steps(xpathSteps),
        stepCount(xpathStepCount),
        targetTextNode(textNodeIdx),
        relaxFirstStepDepth(relaxFirstStep) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  ParagraphStreamer(bool resolveBodyText, int charOff, int textNodeIdx)
      : fwdTarget(SIZE_MAX), revChar(charOff), targetTextNode(textNodeIdx), targetBodyText(resolveBodyText) {
    memset(stepEnteredAtDepth, -1, sizeof(stepEnteredAtDepth));
  }

  size_t write(uint8_t c) override {
    if (!fwdCaptured && bytesWritten >= fwdTarget) {
      fwdResult = pCount;
      fwdCaptured = true;
    }
    bytesWritten++;

    if (globalInEntity) {
      if (entityLen + 1 < MAX_ENTITY_SIZE) {
        entityBuffer[entityLen++] = static_cast<char>(c);
      } else {
        flushEntityAsLiteral();
        globalInEntity = false;
        entityLen = 0;
      }
      if (globalInEntity) {
        if (c == ';') {
          finishEntity();
        } else if (c == '<' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          flushEntityAsLiteral();
          globalInEntity = false;
          entityLen = 0;
        }
      }
      return 1;
    }

    // XML 1.0 §2.11 line-ending normalization: expat (which builds the page LUT) collapses a
    // "\r\n" pair and a lone "\r" to a single "\n". Mirror that so this byte counter -- which the
    // resolved offset is measured against -- stays codepoint-for-codepoint identical. prevCR is
    // set only by the visible-text branch below, so any non-text byte clears it here.
    const bool afterCR = prevCR;
    prevCR = false;

    if (markupMode != MARKUP_NONE) {
      processMarkupByte(c, afterCR);
      return 1;
    }

    writeOutsideMarkup(c, afterCR);
    return 1;
  }

  // Everything that is not inside a comment / PI / CDATA section.
  void writeOutsideMarkup(const uint8_t c, const bool afterCR) {
    if (c == '<') {
      markupMode = MARKUP_DECIDE;
      globalInTag = true;
      tagState = TAG_IDLE;
      tagNameLen = 0;
      tagIsClose = false;
      capturingAnchorTag = false;
      resetAnchorAttrScan();
      resetSkipAttrScan();
      tagStartsSkippedSubtree = false;
      tagSelfClosing = false;
      inAttrQuote = false;
      attrQuoteChar = 0;
    } else if (c == '>') {
      if (tagState == TAG_ATTRS) {
        // A bare final attribute (<p hidden>) has not been evaluated yet.
        if (!tagIsClose && skipAttrNameLen > 0) evaluateSkipAttribute();
        endAnchorIdScan();
      }
      globalInTag = false;
      inAttrQuote = false;
      if (tagState == TAG_IN_NAME && tagNameLen > 0) {
        tagName[tagNameLen] = '\0';
        if (tagIsClose)
          onCloseTag();
        else
          onOpenTag();
        tagNameLen = 0;
      }
      // A self-closing tag with attributes (<br />) fires onOpenTag() when its name ends but has
      // no close tag, so htmlDepth -- and any nonVisibleDepth inside a skipped subtree -- would
      // never unwind. tagName still holds the name here.
      if (tagState == TAG_ATTRS && tagSelfClosing && !tagIsClose) {
        onCloseTag();
      }
      // onOpenTag() ran before the attributes were seen, so the shared skipped-subtree rule is
      // applied here. onCloseTag() unwinds nonVisibleDepth symmetrically.
      if (tagStartsSkippedSubtree && !tagIsClose && !tagSelfClosing) {
        nonVisibleDepth++;
      }
      tagStartsSkippedSubtree = false;
      tagSelfClosing = false;
      tagState = TAG_IDLE;
    } else if (globalInTag) {
      processByteInTag(c);
    } else if (!insideBody || nonVisibleDepth > 0) {
      // Ignore head/style/script/title text. KOReader XPaths are body-relative, and CSS text
      // should not contribute to intra-spine progress.
    } else {
      if (c == '&') {
        globalInEntity = true;
        entityBuffer[0] = '&';
        entityLen = 1;
      } else if (c == '\n' && afterCR) {
        // Second half of a CRLF: the newline was already counted on the preceding CR.
      } else {
        const bool startsCodepoint = (c & 0xC0) != 0x80;
        if (startsCodepoint) onVisibleCodepoint();
        prevCR = (c == '\r');  // a lone/leading CR is the newline; swallow any '\n' that follows
      }
    }
  }

 public:
  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) write(buffer[i]);
    return size;
  }

  int paragraphCount() const { return fwdCaptured ? fwdResult : pCount; }
  int getParagraphAtMatch() const { return paragraphAtMatch; }
  int getListItemAtMatch() const { return liCountAtMatch; }
  const char* getCapturedAnchorId() const { return capturedAnchorIdLen > 0 ? capturedAnchorId : nullptr; }
  size_t totalBytes() const { return bytesWritten; }
  bool found() const { return revDone; }
  size_t getTotalVisChars() const { return totalVisChars; }
  size_t getTargetVisChars() const { return targetVisChars; }
  float progress() const {
    return totalVisChars > 0 ? static_cast<float>(targetVisChars) / static_cast<float>(totalVisChars) : 0.0f;
  }
};

// Streams `xhtml` through the ancestry-mode streamer and reports the visible-codepoint offset the
// XPath resolves to, or nullopt when it does not resolve. The round-trip counterpart of
// ChapterXPathResolver::findXPathForVisibleTextOffset().
inline std::optional<size_t> resolveXPathVisibleOffset(const std::string_view xhtml, const std::string& xpath) {
  XPathStep steps[MAX_XPATH_DEPTH];
  const int stepCount = parseXPathSteps(xpath, steps);
  if (stepCount <= 0) return std::nullopt;

  const int charOffset = parseCharOffset(xpath);
  const int textNode = parseTextNodeIndex(xpath);
  for (const bool relaxed : {false, true}) {
    ParagraphStreamer streamer(steps, stepCount, charOffset, textNode, relaxed);
    streamer.write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
    if (streamer.found()) return streamer.getTargetVisChars();
  }
  return std::nullopt;
}

}  // namespace koreader_sync
