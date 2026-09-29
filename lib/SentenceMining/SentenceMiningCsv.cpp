#include "SentenceMiningCsv.h"

#include <cstdint>
#include <cstdio>

namespace sentencemining {

namespace {

// Sentence ends: CJK full-width marks and their ASCII counterparts. '.' also ends "Mr." and
// "e.g." -- accepted: a sentence cut short there is still a usable card.
constexpr std::string_view SENTENCE_ENDS[] = {"。", "！", "？", "．", "!", "?", "."};
// Closing marks that belong to the sentence they follow: 「…。」 ends after the bracket.
constexpr std::string_view CLOSERS[] = {"」", "』", "）", ")", "\"", "”", "’", "'"};

bool isContinuationByte(const unsigned char c) { return (c & 0xC0) == 0x80; }

// Length of the UTF-8 sequence starting at text[i] (1 for a stray continuation byte).
size_t charLength(std::string_view text, const size_t i) {
  const auto c = static_cast<unsigned char>(text[i]);
  size_t n = 1;
  if (c >= 0xF0) {
    n = 4;
  } else if (c >= 0xE0) {
    n = 3;
  } else if (c >= 0xC0) {
    n = 2;
  }
  return i + n <= text.size() ? n : 1;
}

// If one of `marks` starts at text[i], its byte length; otherwise 0.
template <size_t N>
size_t markAt(std::string_view text, const size_t i, const std::string_view (&marks)[N]) {
  for (const auto mark : marks) {
    if (text.substr(i, mark.size()) == mark) return mark.size();
  }
  return 0;
}

// Byte offset just past a sentence end at text[i] and any closers after it; 0 when none at i.
size_t endAt(std::string_view text, size_t i) {
  const size_t len = markAt(text, i, SENTENCE_ENDS);
  if (len == 0) return 0;
  i += len;
  for (size_t closer = markAt(text, i, CLOSERS); closer > 0; closer = markAt(text, i, CLOSERS)) i += closer;
  return i;
}

size_t codepoints(std::string_view text) {
  size_t n = 0;
  for (const char c : text) n += isContinuationByte(static_cast<unsigned char>(c)) ? 0 : 1;
  return n;
}

// The last `count` codepoints of text.
std::string_view lastCodepoints(std::string_view text, size_t count) {
  size_t i = text.size();
  while (i > 0 && count > 0) {
    --i;
    if (!isContinuationByte(static_cast<unsigned char>(text[i]))) --count;
  }
  return text.substr(i);
}

// The first `count` codepoints of text.
std::string_view firstCodepoints(std::string_view text, size_t count) {
  size_t i = 0;
  while (i < text.size() && count > 0) {
    i += charLength(text, i);
    --count;
  }
  return text.substr(0, i);
}

bool isSpace(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Page text arrives with paragraph breaks and layout spacing; a card wants one line.
std::string collapseWhitespace(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  for (const char c : text) {
    if (isSpace(c)) {
      pendingSpace = true;
      continue;
    }
    if (pendingSpace && !out.empty()) out += ' ';
    pendingSpace = false;
    out += c;
  }
  return out;
}

}  // namespace

std::string headerLines() {
  return "#separator:comma\n"
         "#html:true\n"
         "#guid column:1\n"
         "#tags column:10\n"
         "#columns:GUID,Word,Reading,Sentence,Definition,Book,Author,Date,Dictionary,Tags\n";
}

std::string csvField(std::string_view value) {
  bool needsQuotes = false;
  for (const char c : value) {
    if (c == ',' || c == '"' || c == '\n' || c == '\r') {
      needsQuotes = true;
      break;
    }
  }
  if (!needsQuotes) return std::string(value);
  std::string out;
  out.reserve(value.size() + 2);
  out += '"';
  for (const char c : value) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

std::string htmlEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

std::string makeGuid(std::string_view word, std::string_view sentence) {
  // FNV-1a, 64-bit: stable across builds and platforms, which a std::hash is not.
  uint64_t hash = 0xcbf29ce484222325ULL;
  const auto mix = [&hash](std::string_view text) {
    for (const char c : text) {
      hash ^= static_cast<unsigned char>(c);
      hash *= 0x100000001b3ULL;
    }
  };
  mix(word);
  mix("\x1f");  // a separator no text contains, so ("ab","c") and ("a","bc") differ
  mix(sentence);
  char buf[20];
  snprintf(buf, sizeof(buf), "m%016llx", static_cast<unsigned long long>(hash));
  return buf;
}

std::string tagFromTitle(std::string_view title) {
  std::string out;
  out.reserve(title.size());
  for (size_t i = 0; i < title.size();) {
    if (isSpace(title[i])) {
      if (!out.empty() && out.back() != '_') out += '_';
      ++i;
    } else if (title.substr(i, 3) == "　") {  // ideographic space
      if (!out.empty() && out.back() != '_') out += '_';
      i += 3;
    } else {
      out += title[i++];
    }
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  return out;
}

bool hasSentenceEnd(std::string_view after) {
  for (size_t i = 0; i < after.size(); i += charLength(after, i)) {
    if (markAt(after, i, SENTENCE_ENDS) > 0) return true;
  }
  return false;
}

std::string sentenceHtml(std::string_view before, std::string_view word, std::string_view after) {
  // Back to just past the last sentence end before the word.
  size_t start = 0;
  for (size_t i = 0; i < before.size(); i += charLength(before, i)) {
    if (const size_t past = endAt(before, i)) start = past;
  }
  std::string lead = collapseWhitespace(before.substr(start));

  // Forward through the first sentence end after it (and its closing marks).
  size_t stop = after.size();
  for (size_t i = 0; i < after.size(); i += charLength(after, i)) {
    if (const size_t past = endAt(after, i)) {
      stop = past;
      break;
    }
  }
  // Keep the word's trailing space if the text had one: "Did anyone see" must not become
  // "Did anyonesee" once the trail is collapsed on its own.
  std::string trail = collapseWhitespace(after.substr(0, stop));
  if (!trail.empty() && !after.empty() && isSpace(after.front())) trail.insert(0, " ");
  if (!lead.empty() && !before.empty() && isSpace(before.back())) lead += ' ';

  // Budget the codepoints around the word: half before, the rest after.
  const size_t wordLen = codepoints(word);
  const size_t budget = MAX_SENTENCE_CODEPOINTS > wordLen ? MAX_SENTENCE_CODEPOINTS - wordLen : 0;
  size_t leadBudget = budget / 2;
  const size_t trailLen = codepoints(trail);
  if (trailLen < budget - leadBudget) leadBudget = budget - trailLen;  // unused trail room goes to lead
  const std::string_view leadView = lastCodepoints(lead, leadBudget);
  const std::string_view trailView = firstCodepoints(trail, budget - codepoints(leadView));

  std::string out;
  out.reserve(leadView.size() + word.size() + trailView.size() + 16);
  out += htmlEscape(leadView);
  out += "<b>";
  out += htmlEscape(word);
  out += "</b>";
  out += htmlEscape(trailView);
  return out;
}

std::string capUtf8(std::string_view text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return std::string(text);
  constexpr std::string_view ELLIPSIS = "…";
  if (maxBytes < ELLIPSIS.size()) return {};
  size_t cut = maxBytes - ELLIPSIS.size();
  while (cut > 0 && isContinuationByte(static_cast<unsigned char>(text[cut]))) --cut;
  std::string out(text.substr(0, cut));
  out += ELLIPSIS;
  return out;
}

std::string formatRow(const Card& card) {
  const std::string guid = makeGuid(card.word, card.sentence);
  const std::string bookTag = tagFromTitle(card.book);
  const std::string tags = bookTag.empty() ? "matcha" : "matcha " + bookTag;
  const std::string definition = capUtf8(card.definition, MAX_DEFINITION_BYTES);

  std::string row;
  row.reserve(guid.size() + card.word.size() + card.reading.size() + card.sentence.size() + definition.size() +
              card.book.size() + card.author.size() + card.date.size() + card.dictionary.size() + tags.size() + 32);
  const std::string_view fields[] = {guid,      card.word,   card.reading, card.sentence,   definition,
                                     card.book, card.author, card.date,    card.dictionary, tags};
  bool first = true;
  for (const auto field : fields) {
    if (!first) row += ',';
    first = false;
    row += csvField(field);
  }
  row += '\n';
  return row;
}

std::string exportPath(std::string_view language) {
  std::string code;
  for (const char c : language) {
    if (code.size() >= 16) break;
    if (c >= 'A' && c <= 'Z') {
      code += static_cast<char>(c - 'A' + 'a');
    } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
      code += c;
    } else if (c == '_') {
      code += '-';  // pt_BR and pt-BR name the same language
    }
  }
  if (code.empty()) code = "other";
  return std::string(EXPORT_DIR) + "/sentences-" + code + ".csv";
}

std::string languageForDictionary(std::string_view folderName, std::string_view bookLanguage) {
  const auto isCodeChar = [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
  };
  const size_t slash = folderName.find('/');
  if (slash != std::string_view::npos && slash >= 2 && slash <= 8) {
    const std::string_view head = folderName.substr(0, slash);
    bool code = true;
    for (const char c : head) code = code && isCodeChar(c);
    if (code) return std::string(head);
  }
  const size_t sub = bookLanguage.find_first_of("-_");
  return std::string(bookLanguage.substr(0, sub));
}

std::string capHtml(std::string_view html) {
  std::string out = capUtf8(html, MAX_DEFINITION_BYTES);
  if (out.size() == html.size()) return out;
  // A cut inside "<span class=..." would leave an open tag that swallows the rest of the card.
  const size_t lastOpen = out.rfind('<');
  const size_t lastClose = out.rfind('>');
  if (lastOpen != std::string::npos && (lastClose == std::string::npos || lastClose < lastOpen)) {
    out.erase(lastOpen);
    out += "\xe2\x80\xa6";  // the ellipsis capUtf8 appended went with the cut
  }
  return out;
}

std::string definitionHtml(std::string_view plain) {
  // Bounded first so the escape below never builds more than one capped row's worth of text.
  const std::string capped = capUtf8(plain, MAX_DEFINITION_BYTES);
  std::string out;
  out.reserve(capped.size() + capped.size() / 8);
  size_t lineStart = 0;
  while (lineStart <= capped.size()) {
    const size_t lineEnd = capped.find('\n', lineStart);
    const std::string_view line =
        std::string_view(capped).substr(lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
    if (!out.empty()) out += "<br>";
    out += htmlEscape(line);
    if (lineEnd == std::string::npos) break;
    lineStart = lineEnd + 1;
  }
  // Runs of blank lines between entries become one visual gap, not a stack of <br>s.
  for (size_t p = out.find("<br><br><br>"); p != std::string::npos; p = out.find("<br><br><br>", p)) out.erase(p, 4);
  return out;
}

}  // namespace sentencemining
