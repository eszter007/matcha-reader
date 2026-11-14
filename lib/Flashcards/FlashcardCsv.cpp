#include "FlashcardCsv.h"

namespace flashcards {

CsvRecordParser::CsvRecordParser(const uint16_t keepMask, const size_t maxFieldBytes)
    : keepMask_(keepMask), maxFieldBytes_(maxFieldBytes) {}

void CsvRecordParser::clearFields() {
  for (auto& f : fields_) f.clear();
}

void CsvRecordParser::endField() {
  if (column_ < COLUMNS) column_++;
}

bool CsvRecordParser::endRecord() {
  const bool data = !comment_ && anyData_;
  if (data) recordStart_ = pendingStart_;
  column_ = 0;
  atRecordStart_ = true;
  comment_ = false;
  inQuotes_ = false;
  quoteSeen_ = false;
  anyData_ = false;
  return data;
}

bool CsvRecordParser::feed(const char c) {
  const uint32_t at = offset_++;
  if (atRecordStart_) {
    atRecordStart_ = false;
    pendingStart_ = at;
    clearFields();
    comment_ = c == '#';
  }
  if (comment_) return c == '\n' ? (endRecord(), false) : false;

  if (inQuotes_) {
    if (quoteSeen_) {
      quoteSeen_ = false;
      if (c == '"') {
        // A doubled quote is one literal quote; fall through to store it.
      } else {
        inQuotes_ = false;  // that quote closed the field; handle c as unquoted
      }
    } else if (c == '"') {
      quoteSeen_ = true;
      return false;
    }
  }
  if (!inQuotes_) {
    if (c == '"' && column_ < COLUMNS && fields_[column_].empty()) {
      inQuotes_ = true;
      anyData_ = true;
      return false;
    }
    if (c == ',') {
      endField();
      anyData_ = true;
      return false;
    }
    if (c == '\r') return false;
    if (c == '\n') return endRecord();
  }
  anyData_ = true;
  if (column_ < COLUMNS && (keepMask_ >> column_) & 1) {
    std::string& f = fields_[column_];
    // Cut on a UTF-8 boundary: never start keeping a lead byte that has no room for its tail.
    if (f.size() < maxFieldBytes_ ||
        ((static_cast<unsigned char>(c) & 0xC0) == 0x80 && f.size() < maxFieldBytes_ + 3)) {
      f.push_back(c);
    }
  }
  return false;
}

bool CsvRecordParser::finish() {
  if (atRecordStart_) return false;
  return endRecord();
}

uint64_t guidToId(const std::string_view guid) {
  if (guid.size() != 17 || guid[0] != 'm') return 0;
  uint64_t id = 0;
  for (size_t i = 1; i < guid.size(); i++) {
    const char c = guid[i];
    uint64_t v;
    if (c >= '0' && c <= '9') {
      v = static_cast<uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v = static_cast<uint64_t>(c - 'a' + 10);
    } else {
      return 0;
    }
    id = (id << 4) | v;
  }
  return id;
}

std::string stripExportHtml(const std::string_view html, const std::string_view boldOpen,
                            const std::string_view boldClose, const std::string_view lineBreak) {
  struct Swap {
    std::string_view from;
    std::string_view to;
  };
  const Swap swaps[] = {{"<b>", boldOpen},    {"</b>", boldClose}, {"<br>", lineBreak},
                        {"<br/>", lineBreak}, {"&amp;", "&"},      {"&lt;", "<"},
                        {"&gt;", ">"},        {"&quot;", "\""},    {"&#39;", "'"}};
  std::string out;
  out.reserve(html.size());
  size_t i = 0;
  while (i < html.size()) {
    bool swapped = false;
    if (html[i] == '<' || html[i] == '&') {
      for (const auto& s : swaps) {
        if (html.substr(i, s.from.size()) == s.from) {
          out.append(s.to);
          i += s.from.size();
          swapped = true;
          break;
        }
      }
    }
    if (!swapped) out.push_back(html[i++]);
  }
  return out;
}

int32_t dayNumber(const std::string_view iso) {
  if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return -1;
  const auto num = [&iso](const size_t at, const size_t len) {
    int v = 0;
    for (size_t i = at; i < at + len; i++) {
      if (iso[i] < '0' || iso[i] > '9') return -1;
      v = v * 10 + (iso[i] - '0');
    }
    return v;
  };
  int y = num(0, 4);
  const int m = num(5, 2), d = num(8, 2);
  if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
  // Howard Hinnant's days_from_civil.
  y -= m <= 2;
  const int era = y / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

}  // namespace flashcards
