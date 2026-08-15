#include "DefinitionTextRenderer.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <vector>

#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace DefinitionText {

namespace {
// One rendered line is bounded by screen width (~25-40 CJK chars); 512 bytes is generous.
// Remainders longer than this skip the "fits on one line" fast path -- they could never fit.
constexpr size_t LINE_BUF_CAP = 512;
constexpr char kBlockHeadingPrefix[] =
    "\x1f"
    "H";
constexpr char kBoldLinePrefix[] =
    "\x1f"
    "B";
constexpr char kItalicLinePrefix[] =
    "\x1f"
    "I";

void trim(std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    text.clear();
    return;
  }
  const size_t last = text.find_last_not_of(" \t\r\n");
  text = text.substr(first, last - first + 1);
}

bool isKanaOnly(const std::string& text) {
  bool hasKana = false;
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    uint32_t cp = 0;
    size_t len = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
      cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      len = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
      cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      len = 3;
    } else {
      return false;
    }
    if (cp < 0x3040 || cp > 0x30FF) return false;
    hasKana = true;
    i += len;
  }
  return hasKana;
}

std::string uppercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](const char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
  return text;
}

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](const char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
  return text;
}

std::string formatTag(std::string tag) {
  trim(tag);
  const std::string lower = lowercase(tag);
  if (!lower.empty() && std::all_of(lower.begin(), lower.end(), [](const unsigned char c) { return std::isdigit(c); }))
    return {};
  if (lower.rfind("5-dan", 0) == 0) return "5-DAN VERB" + uppercase(tag.substr(5));
  if (lower.rfind("1-dan", 0) == 0) return "1-DAN VERB" + uppercase(tag.substr(5));
  if (lower == "suru") return "SURU VERB";
  if (lower == "kuru") return "KURU VERB";
  return uppercase(tag);
}

std::string formatGrammarLevel(std::string level) {
  trim(level);
  if (level == "上級編") return "Advanced grammar";
  if (level == "中級編") return "Intermediate grammar";
  if (level == "基本") return "Basic grammar";
  return uppercase(level);
}

bool containsJapanese(const std::string& text, size_t startOffset, size_t end);

void formatGrammarBody(std::string& text, const std::string& headword) {
  enum class Section : uint8_t { None, Explanation, Meaning, Examples, Connection };

  std::string formatted;
  formatted.reserve(text.size());
  std::string grammarHeadword = headword;
  Section section = Section::None;
  int exampleNumber = 0;
  int patternNumber = 0;
  const auto appendLine = [&formatted](const std::string& line) {
    if (!formatted.empty()) formatted.push_back('\n');
    formatted += line;
  };
  const auto appendHeading = [&](const char* heading) {
    if (!formatted.empty() && formatted.back() != '\n') {
      appendLine("");
      appendLine("");
    }
    appendLine(std::string(kBlockHeadingPrefix) + heading);
  };
  const auto stripExampleLabel = [](std::string& line) {
    if (line.empty() || line.front() != '(') return;
    const size_t close = line.find(").");
    if (close == std::string::npos) return;
    line.erase(0, close + 2);
    trim(line);
    while (line.rfind("\xe3\x80\x80", 0) == 0) line.erase(0, 3);  // Japanese full-width space.
  };

  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t end = text.find('\n', pos);
    std::string line = text.substr(pos, end == std::string::npos ? text.size() - pos : end - pos);
    trim(line);
    pos = end == std::string::npos ? text.size() + 1 : end + 1;
    if (line.empty()) continue;
    if (line == "---") {
      section = Section::None;
      continue;
    }
    if (line.rfind("文法項目 |", 0) == 0) {
      if (grammarHeadword.empty()) {
        const size_t firstPipe = line.find('|');
        const size_t lastPipe = line.rfind('|');
        if (firstPipe != std::string::npos && lastPipe > firstPipe) {
          grammarHeadword = line.substr(firstPipe + 1, lastPipe - firstPipe - 1);
          trim(grammarHeadword);
        }
      }
      section = Section::None;
      continue;
    }

    if (line == "[解説]") {
      section = Section::Explanation;
      appendHeading("解説 · EXPLANATION");
      continue;
    }
    if (line == "[意味]") {
      section = Section::Meaning;
      appendHeading("意味 · MEANING");
      continue;
    }
    if (line == "[例文A]" || line == "[例文B]") {
      if (section != Section::Examples) appendHeading("例文 · EXAMPLES");
      section = Section::Examples;
      continue;
    }
    if (line == "[接続]") {
      section = Section::Connection;
      appendHeading("接続 · PATTERN");
      continue;
    }

    switch (section) {
      case Section::Explanation: {
        if (!grammarHeadword.empty() && line.rfind(grammarHeadword, 0) == 0) {
          line.erase(0, grammarHeadword.size());
          trim(line);
          if (!line.empty() && line.front() == '(') {
            const size_t close = line.find(')');
            if (close != std::string::npos) {
              line.erase(0, close + 1);
              trim(line);
            }
          }
        }
        appendLine(line);
        break;
      }
      case Section::Meaning: {
        size_t semicolon = 0;
        while ((semicolon = line.find(';', semicolon)) != std::string::npos) {
          line.replace(semicolon, 1, " \xc2\xb7");
          semicolon += 3;
        }
        appendLine(std::string(kItalicLinePrefix) + line);
        break;
      }
      case Section::Examples:
        if (containsJapanese(line, 0, line.size())) {
          stripExampleLabel(line);
          if (exampleNumber > 0) appendLine("");
          const char label = static_cast<char>('a' + exampleNumber++ % 26);
          appendLine(std::string(1, label) + ")" + line);
        } else {
          appendLine(std::string(kItalicLinePrefix) + "   " + line);
        }
        break;
      case Section::Connection: {
        std::vector<std::string> cells;
        size_t cellStart = 0;
        while (cellStart <= line.size()) {
          const size_t pipe = line.find('|', cellStart);
          std::string cell =
              line.substr(cellStart, pipe == std::string::npos ? line.size() - cellStart : pipe - cellStart);
          trim(cell);
          if (!cell.empty()) cells.push_back(std::move(cell));
          if (pipe == std::string::npos) break;
          cellStart = pipe + 1;
        }
        if (cells.empty()) break;
        const size_t romanEnd = cells.front().find(')');
        if (!cells.front().empty() && cells.front().front() == '(' && romanEnd != std::string::npos) {
          cells.front().erase(0, romanEnd + 1);
          trim(cells.front());
          if (patternNumber > 0) appendLine("");
          std::string pattern = std::to_string(++patternNumber) + ". " + cells.front();
          if (cells.size() > 1) pattern += " + " + cells[1];
          appendLine(std::string(kBoldLinePrefix) + pattern);
          if (cells.size() > 2) appendLine(std::string(kItalicLinePrefix) + "   " + cells[2]);
        } else {
          appendLine("   " + cells.front());
          if (cells.size() > 1) appendLine(std::string(kItalicLinePrefix) + "   " + cells[1]);
        }
        break;
      }
      case Section::None:
        break;
    }
  }

  text = std::move(formatted);
#ifndef NDEBUG
  assert(text.find("文法項目 |") == std::string::npos);
  assert(text.find("[例文") == std::string::npos);
  assert(text.find("[接続]") == std::string::npos);
#endif
}

bool parseTags(const std::string& line, const size_t start, std::string& grammar) {
  grammar.clear();
  size_t pos = start;
  while (pos < line.size()) {
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) pos++;
    if (pos == line.size()) return !grammar.empty();
    if (line[pos] != '[') return false;
    const size_t close = line.find(']', pos + 1);
    if (close == std::string::npos) return false;
    std::string formatted = formatTag(line.substr(pos + 1, close - pos - 1));
    if (!formatted.empty()) {
      if (!grammar.empty()) grammar += " \xc2\xb7 ";
      grammar += formatted;
    }
    pos = close + 1;
  }
  return !grammar.empty();
}

bool containsJapanese(const std::string& text, const size_t startOffset = 0, size_t end = std::string::npos) {
  end = std::min(end, text.size());
  for (size_t i = startOffset; i < end;) {
    const auto c = static_cast<unsigned char>(text[i]);
    uint32_t cp = c;
    size_t len = 1;
    if ((c & 0xF0) == 0xE0 && i + 2 < end) {
      cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      len = 3;
    } else if ((c & 0xF0) == 0xF0 && i + 3 < end) {
      len = 4;
    } else if ((c & 0xE0) == 0xE0) {
      return false;
    }
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xFF66 && cp <= 0xFF9F)) return true;
    i += len;
  }
  return false;
}

bool isNumberedSense(const std::string& text, size_t start, const size_t end) {
  while (start < end && (text[start] == ' ' || text[start] == '\t')) start++;
  const size_t digits = start;
  while (start < end && text[start] >= '0' && text[start] <= '9') start++;
  return start > digits && start + 1 < end && text[start] == '.' && text[start + 1] == ' ';
}

inline size_t utf8CharLen(const unsigned char c0) {
  if (c0 >= 0xF0) return 4;
  if (c0 >= 0xE0) return 3;
  if (c0 >= 0xC0) return 2;
  return 1;
}
}  // namespace

void extractEntryMetadata(std::string& text, const std::string& fallbackHeadword, EntryMetadata& metadata) {
  metadata = {};
  bool grammarEntry = false;
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
    text.erase(0, 1);

  static constexpr char kOpen[] = "\xe3\x80\x90";
  static constexpr char kClose[] = "\xe3\x80\x91";
  for (int pass = 0; pass < 3 && !text.empty(); pass++) {
    const size_t lineEnd = text.find('\n');
    const size_t lineLength = lineEnd == std::string::npos ? text.size() : lineEnd;
    std::string firstLine = text.substr(0, lineLength);
    trim(firstLine);
    const bool sourceLine = firstLine.find("JMdict") != std::string::npos ||
                            firstLine.find("JMnedict") != std::string::npos ||
                            firstLine.find("Tatoeba") != std::string::npos;
    const size_t marker = firstLine.find(kOpen);
    if (sourceLine) {
      const size_t sourceEnd = std::min({marker, firstLine.find('['), firstLine.find('=')});
      metadata.source = firstLine.substr(0, sourceEnd);
      trim(metadata.source);
    }
    static constexpr char kGrammarHeader[] = "文法項目 |";
    if (firstLine.rfind(kGrammarHeader, 0) == 0) {
      const size_t levelPipe = firstLine.rfind('|');
      if (levelPipe != std::string::npos) metadata.source = formatGrammarLevel(firstLine.substr(levelPipe + 1));
      text.erase(0, lineEnd == std::string::npos ? text.size() : lineEnd + 1);
      while (!text.empty() &&
             (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.erase(0, 1);
      grammarEntry = true;
      break;
    }
    if (marker != std::string::npos) {
      const size_t close = firstLine.find(kClose, marker + sizeof(kOpen) - 1);
      if (close != std::string::npos) {
        metadata.reading = firstLine.substr(marker + sizeof(kOpen) - 1, close - marker - (sizeof(kOpen) - 1));
        text.erase(0, lineEnd == std::string::npos ? text.size() : lineEnd + 1);
        while (!text.empty() &&
               (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
          text.erase(0, 1);
        continue;
      }
    }

    const size_t tagStart = firstLine.find('[');
    std::string grammar;
    if (tagStart != std::string::npos && parseTags(firstLine, tagStart, grammar) && (sourceLine || tagStart == 0)) {
      metadata.grammar = std::move(grammar);
      text.erase(0, lineEnd == std::string::npos ? text.size() : lineEnd + 1);
      while (!text.empty() &&
             (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.erase(0, 1);
      continue;
    }

    // The generated dictionary sometimes puts a source-only line (e.g. `JMdict= 思う`) before
    // the reading marker on the next line. It is panel chrome, not part of the definition body.
    if (sourceLine && tagStart == std::string::npos && marker == std::string::npos) {
      text.erase(0, lineEnd == std::string::npos ? text.size() : lineEnd + 1);
      while (!text.empty() &&
             (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
        text.erase(0, 1);
      continue;
    }
    break;
  }

  if (!grammarEntry && metadata.reading.empty() && isKanaOnly(fallbackHeadword)) metadata.reading = fallbackHeadword;
}

void formatEntryBody(std::string& text, const std::string& omitLeading) {
  if (text.empty()) return;
  if (!omitLeading.empty()) {
    formatGrammarBody(text, omitLeading);
    return;
  }

  static constexpr char kGrammarSeparator[] = "\n\n— Grammar: ";
  const size_t grammarAt = text.find(kGrammarSeparator);
  if (grammarAt != std::string::npos) {
    std::string grammar = text.substr(grammarAt + 2);
    text.erase(grammarAt);
    formatEntryBody(text, {});
    formatGrammarBody(grammar, {});
    if (!text.empty() && !grammar.empty()) text += "\n\n";
    text += grammar;
#ifndef NDEBUG
    assert(text.find(kGrammarSeparator) == std::string::npos);
#endif
    return;
  }

  std::string formatted;
  formatted.reserve(text.size() + 64);
  std::string glosses;
  int senseNumber = 1;
  int nameNumber = 1;
  static constexpr char kBullet[] = "\xe2\x80\xa2";
  const bool isNameDictionary = text.find(kBullet) == std::string::npos;

  const auto appendLine = [&formatted](const std::string& line) {
    if (!formatted.empty()) formatted.push_back('\n');
    formatted += line;
  };
  const auto flushGlosses = [&] {
    if (glosses.empty()) return;
    appendLine(std::to_string(senseNumber++) + ". " + glosses);
    glosses.clear();
  };

  size_t pos = 0;
  bool firstContentLine = true;
  while (pos <= text.size()) {
    const size_t end = text.find('\n', pos);
    std::string line = text.substr(pos, end == std::string::npos ? text.size() - pos : end - pos);
    trim(line);
    pos = end == std::string::npos ? text.size() + 1 : end + 1;

    if (line.empty()) {
      flushGlosses();
      size_t next = pos;
      std::string nextLine;
      while (next <= text.size()) {
        const size_t nextEnd = text.find('\n', next);
        nextLine = text.substr(next, nextEnd == std::string::npos ? text.size() - next : nextEnd - next);
        trim(nextLine);
        if (!nextLine.empty()) break;
        next = nextEnd == std::string::npos ? text.size() + 1 : nextEnd + 1;
      }
      if (!formatted.empty() && formatted.back() != '\n' && nextLine.compare(0, sizeof(kBullet) - 1, kBullet) == 0) {
        appendLine("");
      }
      continue;
    }

    if (firstContentLine && !omitLeading.empty() && line == omitLeading) {
      firstContentLine = false;
      continue;
    }
    firstContentLine = false;

    if (line == "---") {
      flushGlosses();
      if (!formatted.empty() && formatted.back() != '\n') appendLine("");
      continue;
    }

    // Name dictionaries already provide numbered entries. Keep those as ordinary numbered
    // senses instead of treating them as Japanese/English example pairs with a divider.
    if (isNumberedSense(line, 0, line.size())) {
      flushGlosses();
      appendLine(line);
      continue;
    }

    if (line.compare(0, sizeof(kBullet) - 1, kBullet) == 0) {
      std::string gloss = line.substr(sizeof(kBullet) - 1);
      trim(gloss);
      if (!gloss.empty()) {
        if (!glosses.empty()) glosses += " \xc2\xb7 ";
        glosses += gloss;
      }
      continue;
    }

    static constexpr char kArrow[] = "\xe2\x86\x92";
    if (line.compare(0, sizeof(kArrow) - 1, kArrow) == 0) {
      flushGlosses();
      std::string note = line.substr(sizeof(kArrow) - 1);
      trim(note);
      if (note.rfind("Note:", 0) == 0) {
        note.erase(0, sizeof("Note:") - 1);
        trim(note);
      }
      appendLine(std::string(kArrow) + " " + note);
      continue;
    }

    // Source/readings belong in the panel chrome. The first one is normally removed by
    // extractEntryMetadata(); this also keeps merged sibling entries from leaking their headers.
    if (line.find("JMdict") != std::string::npos || line.find("JMnedict") != std::string::npos ||
        line.find("Tatoeba") != std::string::npos || line.find("【") != std::string::npos) {
      flushGlosses();
      continue;
    }

    if (isNameDictionary) {
      flushGlosses();
      if (isNumberedSense(line, 0, line.size())) {
        appendLine(line);
        size_t numberEnd = 0;
        int existingNumber = 0;
        while (numberEnd < line.size() && line[numberEnd] >= '0' && line[numberEnd] <= '9') {
          existingNumber = existingNumber * 10 + (line[numberEnd] - '0');
          numberEnd++;
        }
        if (numberEnd > 0) nameNumber = std::max(nameNumber, existingNumber + 1);
      } else {
        appendLine(std::to_string(nameNumber++) + ". " + line);
      }
      continue;
    }
    std::string ignoredTags;
    if (line.front() == '[' && parseTags(line, 0, ignoredTags)) {
      flushGlosses();
      continue;
    }

    flushGlosses();
    // Non-bullet lines in this dictionary are example sentences (the following Latin line is
    // their translation). The renderer uses the Japanese/Latin distinction for regular/italic.
    appendLine("  │ " + line);
  }

  flushGlosses();
  text = std::move(formatted);
}

void prewarmStyledText(GfxRenderer& renderer, const int fontId, const std::string& text) {
  std::string cjk[3];
  std::string latin[3];
  const auto appendByScript = [&text](const size_t start, const size_t end, std::string& cjkOut,
                                      std::string& latinOut) {
    for (size_t i = start; i < end;) {
      const auto c0 = static_cast<unsigned char>(text[i]);
      size_t len = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
      len = std::min(len, end - i);
      uint32_t cp = c0;
      if (len == 2) {
        cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      } else if (len == 3) {
        cp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      } else if (len == 4) {
        cp = ((c0 & 0x07) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
             ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(text[i + 3]) & 0x3F);
      }
      (utf8IsCjkCodepoint(cp) ? cjkOut : latinOut).append(text, i, len);
      i += len;
    }
  };

  size_t pos = 0;
  while (pos < text.size()) {
    const size_t end = text.find('\n', pos);
    const size_t lineEnd = end == std::string::npos ? text.size() : end;
    const bool blockHeading = text.compare(pos, sizeof(kBlockHeadingPrefix) - 1, kBlockHeadingPrefix) == 0;
    const bool boldLine = text.compare(pos, sizeof(kBoldLinePrefix) - 1, kBoldLinePrefix) == 0;
    const bool italicLine = text.compare(pos, sizeof(kItalicLinePrefix) - 1, kItalicLinePrefix) == 0;
    const size_t contentStart = pos + ((blockHeading || boldLine || italicLine) ? 2 : 0);
    size_t visibleStart = contentStart;
    while (visibleStart < lineEnd && (text[visibleStart] == ' ' || text[visibleStart] == '\t')) visibleStart++;
    static constexpr char kArrow[] = "\xe2\x86\x92";
    const bool note = text.compare(visibleStart, sizeof(kArrow) - 1, kArrow) == 0;
    const bool example = text.compare(contentStart, 6, "  │ ") == 0;
    const bool translation = italicLine || (example && !containsJapanese(text, contentStart, lineEnd));
    const auto style = (blockHeading || boldLine) ? EpdFontFamily::BOLD
                       : (translation || note)    ? EpdFontFamily::ITALIC
                                                  : EpdFontFamily::REGULAR;
    const auto styleIndex = static_cast<uint8_t>(style);
    appendByScript(contentStart, lineEnd, cjk[styleIndex], latin[styleIndex]);
    if (end == std::string::npos) break;
    pos = end + 1;
  }

  auto* fcm = renderer.getFontCacheManager();
  const auto font = renderer.getFontMap().find(fontId);
  for (uint8_t style = 0; style <= EpdFontFamily::ITALIC; style++) {
    const uint8_t mask = 1 << style;
    renderer.prewarmText(fontId, cjk[style].c_str(), mask);
    // Bold Latin is limited to short labels and patterns. Leaving it on demand keeps the
    // no-SD-fallback path within four slots: CJK+Latin regular, CJK bold, Latin italic.
    if (fcm && font != renderer.getFontMap().end() && style != EpdFontFamily::BOLD && !latin[style].empty()) {
      const auto* data = font->second.getData(static_cast<EpdFontFamily::Style>(style));
      if (auto* decompressor = fcm->getDecompressor()) decompressor->prewarmCache(data, latin[style].c_str());
    }
  }
}

int drawEntryMetadata(GfxRenderer& renderer, const Rect& body, const int fontId, const uint16_t scale,
                      const EntryMetadata& metadata, const int scrollOffset, const int lineHeight) {
  constexpr uint16_t COMPACT_SCALE = 192;  // 75% of the selected lookup size.
  const bool hasReading = !metadata.reading.empty();
  const bool hasGrammar = !metadata.grammar.empty();
  if (!hasReading && !hasGrammar) return body.y;

  const int baseY = body.y;
  int y = body.y - scrollOffset * lineHeight;
  const auto drawVisible = [&body, &renderer](const int fontId, const int x, const int y, const char* text,
                                              const uint16_t scale) {
    if (y >= body.y && y < body.y + body.height) renderer.drawTextScaled(fontId, x, y, text, scale, true);
  };
  if (hasReading) {
    drawVisible(fontId, body.x, y, metadata.reading.c_str(), scale);
    y += renderer.getLineHeightScaled(fontId, scale) + 3;
  }
  if (hasGrammar) {
    drawVisible(fontId, body.x, y, metadata.grammar.c_str(), COMPACT_SCALE);
    y += renderer.getLineHeightScaled(fontId, COMPACT_SCALE) + 5;
  }
  return std::min(baseY + (y - (body.y - scrollOffset * lineHeight)), body.y + body.height);
}

WrapResult DrawWrappedImpl(GfxRenderer& renderer, const int fontId, const std::string& text, const int textX,
                           const int startY, const int lineHeight, const int maxWidth, const int maxY,
                           const int scrollOffset, const uint16_t scale, std::string& lineBuf) {
  WrapResult out;
  int defY = startY;
  int lineIndex = 0;

  size_t nlPos = 0;
  while (nlPos <= text.size()) {
    const size_t nextNl = text.find('\n', nlPos);
    const size_t paraEnd = nextNl == std::string::npos ? text.size() : nextNl;
    const bool isBlockHeading = text.compare(nlPos, sizeof(kBlockHeadingPrefix) - 1, kBlockHeadingPrefix) == 0;
    const bool isBoldLine = text.compare(nlPos, sizeof(kBoldLinePrefix) - 1, kBoldLinePrefix) == 0;
    const bool isItalicLine = text.compare(nlPos, sizeof(kItalicLinePrefix) - 1, kItalicLinePrefix) == 0;
    const size_t contentStart = nlPos + ((isBlockHeading || isBoldLine || isItalicLine) ? 2 : 0);
    const bool isNote = [&text, contentStart, paraEnd] {
      size_t start = contentStart;
      while (start < paraEnd && (text[start] == ' ' || text[start] == '\t')) start++;
      static constexpr char kArrow[] = "\xe2\x86\x92";
      return text.compare(start, sizeof(kArrow) - 1, kArrow) == 0;
    }();
    const bool isExample = text.find("  │ ", contentStart) == contentStart;
    const bool translation = isItalicLine || (isExample && !containsJapanese(text, contentStart, paraEnd));
    const uint16_t paragraphScale = isBlockHeading ? std::min<uint16_t>(scale, 192) : scale;
    // `black=false` means white in every render mode; it is not a gray brush. The first panel
    // render can still be in a grayscale plane, so keep all text visible there as well.
    const bool paragraphBlack = true;
    const EpdFontFamily::Style paragraphStyle = (isBlockHeading || isBoldLine) ? EpdFontFamily::BOLD
                                                : (translation || isNote)      ? EpdFontFamily::ITALIC
                                                                               : EpdFontFamily::REGULAR;
    const EpdFontFamily::Style measurementStyle = isBoldLine ? EpdFontFamily::REGULAR : paragraphStyle;
    const bool alphaLabel = contentStart + 1 < paraEnd && text[contentStart] >= 'a' && text[contentStart] <= 'z' &&
                            text[contentStart + 1] == ')';
    const bool indented = text.compare(contentStart, 3, "   ") == 0;
    const bool hangingSense = isNumberedSense(text, contentStart, paraEnd) || alphaLabel || indented;
    // Align the example rule with the body text after `1. `; its text starts one space farther in.
    const char* hangingPrefix = isExample ? "1.  " : alphaLabel ? "a)" : indented ? "   " : "1. ";
    const int hangingIndent =
        (hangingSense || isNote || isExample)
            ? renderer.getTextWidthScaled(fontId, hangingPrefix, paragraphScale, EpdFontFamily::REGULAR)
            : 0;
    const int continuationIndent =
        isNote ? hangingIndent + renderer.getTextWidthScaled(fontId, "→ ", paragraphScale, EpdFontFamily::REGULAR)
               : hangingIndent;
    // An example is indented on every line, its first included: the marker is stripped before
    // measuring, so there is no leading prefix to stand in for the indent the way "1. " does on a
    // numbered sense.
    const auto lineIndent = [&](const bool firstLine) {
      return (isNote || isExample) ? (firstLine ? hangingIndent : continuationIndent)
                                   : (firstLine ? 0 : continuationIndent);
    };
    const auto availableWidth = [&](const bool firstLine) { return std::max(1, maxWidth - lineIndent(firstLine)); };
    const int exampleRuleX =
        isExample ? textX + renderer.getTextWidthScaled(fontId, "1. ", paragraphScale, EpdFontFamily::REGULAR) : 0;
    static constexpr char kExamplePrefix[] = "  │ ";
    const auto drawWrappedLine = [&](const char* line, const bool firstLine, const int y) {
      renderer.drawTextScaled(fontId, textX + lineIndent(firstLine), y, line, paragraphScale, paragraphBlack,
                              paragraphStyle);
      if (isExample) renderer.drawLine(exampleRuleX, y, exampleRuleX, y + lineHeight - 1, 1, paragraphBlack);
    };
    bool firstParagraphLine = true;
    size_t remStart = contentStart;
    // The marker only tags the paragraph as an example; the rule is drawn separately, and
    // rendering the UTF-8 `│` as well produced a double divider. Dropping it here rather than at
    // draw time keeps the wrapped width and the drawn width the same string -- measuring it would
    // otherwise price the line by whichever fallback font happened to resolve `│`.
    if (isExample && contentStart + sizeof(kExamplePrefix) - 1 <= paraEnd &&
        text.compare(contentStart, sizeof(kExamplePrefix) - 1, kExamplePrefix) == 0) {
      remStart += sizeof(kExamplePrefix) - 1;
    }
    const size_t remEndFixed = paraEnd;
    nlPos = nextNl == std::string::npos ? text.size() + 1 : nextNl + 1;

    if (remStart == remEndFixed) {
      lineIndex++;
      if (lineIndex > scrollOffset) defY += lineHeight / 2;
      continue;
    }

    while (remStart < remEndFixed) {
      const size_t remLen = remEndFixed - remStart;

      // Whole remainder fits on one line? (Only measurable when it fits the buffer; a longer
      // remainder could never fit one line anyway.)
      if (remLen < LINE_BUF_CAP) {
        lineBuf.assign(text, remStart, remLen);
        if (renderer.getTextWidthScaled(fontId, lineBuf.c_str(), paragraphScale, measurementStyle) <=
            availableWidth(firstParagraphLine)) {
          lineIndex++;
          const int lineY =
              isNote && !firstParagraphLine && lineIndex - 1 > scrollOffset ? defY - std::max(1, lineHeight / 3) : defY;
          if (lineIndex > scrollOffset && lineY + lineHeight <= maxY) {
            drawWrappedLine(lineBuf.c_str(), firstParagraphLine, lineY);
            defY = lineY + lineHeight;
            out.linesDrawn++;
          }
          firstParagraphLine = false;
          break;
        }
      }

      // Accumulate characters into the (never-growing) line buffer until too wide.
      lineBuf.clear();
      size_t lastSpaceBreak = std::string::npos;  // bytes of accepted prefix ending after a space
      size_t pos = remStart;
      const int lineWidth = availableWidth(firstParagraphLine);
      while (pos < remEndFixed) {
        const auto c0 = static_cast<unsigned char>(text[pos]);
        size_t charLen = utf8CharLen(c0);
        if (charLen > remEndFixed - pos) charLen = remEndFixed - pos;
        if (lineBuf.size() + charLen >= LINE_BUF_CAP) break;  // buffer full -> hard break, no realloc
        lineBuf.append(text, pos, charLen);
        if (renderer.getTextWidthScaled(fontId, lineBuf.c_str(), paragraphScale, measurementStyle) > lineWidth) {
          // Never orphan sentence-ending punctuation at the start of the next line.
          uint32_t cp = 0;
          if (charLen == 3) {
            cp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[pos + 2]) & 0x3F);
          } else if (charLen == 1) {
            cp = c0;
          }
          const bool keepPunct = cp == 0x3002 || cp == 0x3001 || cp == 0xFF01 || cp == 0xFF1F || cp == '.' ||
                                 cp == ',' || cp == '!' || cp == '?';
          // keepPunct: the punctuation stays in lineBuf on this line; the post-loop
          // advance uses lineBuf.size(), so pos itself needs no update before break.
          if (!keepPunct) {
            lineBuf.resize(lineBuf.size() - charLen);  // reject the overflowing character
          }
          break;
        }
        // Labels that are still part of the text must not be split off it. An example's marker
        // was dropped from `remStart`, so its first line starts at real text and needs no guard.
        const size_t unbreakablePrefix = !firstParagraphLine ? 0 : alphaLabel ? 2 : indented ? 3 : 0;
        if (text[pos] == ' ' && pos >= remStart + unbreakablePrefix) {
          lastSpaceBreak = lineBuf.size();
        }
        pos += charLen;
      }

      if (lineBuf.empty()) {
        // Single character wider than maxWidth -- force it onto its own line.
        size_t cl = utf8CharLen(static_cast<unsigned char>(text[remStart]));
        if (cl > remLen) cl = remLen;
        lineBuf.assign(text, remStart, cl);
        remStart += cl;
      } else if (lastSpaceBreak != std::string::npos && lastSpaceBreak > 0) {
        // Break at the last space to keep Latin words intact.
        remStart += lastSpaceBreak;
        lineBuf.resize(lastSpaceBreak);
        if (remStart < remEndFixed && text[remStart] == ' ') remStart++;
      } else {
        remStart += lineBuf.size();
      }

      lineIndex++;
      const int lineY =
          isNote && !firstParagraphLine && lineIndex - 1 > scrollOffset ? defY - std::max(1, lineHeight / 3) : defY;
      if (lineIndex > scrollOffset && lineY + lineHeight <= maxY) {
        drawWrappedLine(lineBuf.c_str(), firstParagraphLine, lineY);
        defY = lineY + lineHeight;
        out.linesDrawn++;
      }
      firstParagraphLine = false;
    }
  }

  out.totalLines = lineIndex;
  return out;
}

WrapResult drawWrapped(GfxRenderer& renderer, const int fontId, const std::string& text, const int textX,
                       const int startY, const int lineHeight, const int maxWidth, const int maxY,
                       const int scrollOffset, const uint16_t scale) {
  // The single allocation of this whole function, guarded: if even one line buffer doesn't
  // fit, drawing text under a heap this starved would abort() inside the renderer anyway --
  // show nothing (header/word still render) rather than crash. -fno-exceptions makes an
  // unguarded reserve() an abort, not an error.
  if (ESP.getMaxAllocHeap() < LINE_BUF_CAP + 4 * 1024) {
    LOG_ERR("DEFTXT", "Skipping definition render, heap too low (maxAlloc=%u)", ESP.getMaxAllocHeap());
    return {};
  }
  std::string lineBuf;
  lineBuf.reserve(LINE_BUF_CAP);
  return DrawWrappedImpl(renderer, fontId, text, textX, startY, lineHeight, maxWidth, maxY, scrollOffset, scale,
                         lineBuf);
}

}  // namespace DefinitionText
