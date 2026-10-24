#include <ctime>

#include "StatsSync.h"

namespace {
// Server-side history bitmap: 730 days, one bit each.
constexpr size_t HISTORY_BYTES = 92;

void base64(const uint8_t* data, size_t length, std::string& out) {
  static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  out.clear();
  out.reserve((length + 2) / 3 * 4);
  for (size_t i = 0; i < length; i += 3) {
    const uint32_t chunk = data[i] << 16 | (i + 1 < length ? data[i + 1] << 8 : 0) | (i + 2 < length ? data[i + 2] : 0);
    out += ALPHABET[chunk >> 18 & 63];
    out += ALPHABET[chunk >> 12 & 63];
    out += i + 1 < length ? ALPHABET[chunk >> 6 & 63] : '=';
    out += i + 2 < length ? ALPHABET[chunk & 63] : '=';
  }
}

// Index into the server's Monday-first `dow`; 2000-01-01 (day 0) was a Saturday.
int weekday(const int32_t day) { return (day + 5) % 7; }

void weekdays(JsonObject doc, const uint32_t (&dow)[7]) {
  JsonArray out = doc["dow"].to<JsonArray>();
  for (const uint32_t s : dow) out.add(s);
  // Time of day is not recorded on the device.
  JsonArray tod = doc["tod"].to<JsonArray>();
  for (int i = 0; i < 4; i++) tod.add(0);
}

// Unix seconds at UTC midnight of a day, the server's date unit.
uint32_t unixDate(const int32_t day) { return static_cast<uint32_t>(day + 10957) * 86400; }

void dateString(const int32_t day, char out[11]) {
  const time_t t = static_cast<time_t>(unixDate(day));
  tm parts{};
  gmtime_r(&t, &parts);
  strftime(out, 11, "%Y-%m-%d", &parts);
}

}  // namespace

int32_t statsSync::daysSince2000(const uint16_t year, const uint8_t month, const uint8_t day) {
  // Howard Hinnant's days_from_civil, shifted from 1970 to 2000.
  const int y = year - (month <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int>(doe) - 719468 - 10957;
}

void statsSync::buildGlobal(JsonDocument& doc, const std::vector<Day>& days, const Totals& totals, const size_t from,
                            const size_t count) {
  uint8_t history[HISTORY_BYTES] = {};
  for (const auto& d : days) {
    const int32_t bit = totals.today - d.day;
    if (d.seconds > 0 && bit >= 0 && bit < static_cast<int32_t>(HISTORY_BYTES * 8)) {
      history[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    }
  }
  std::string encoded;
  base64(history, sizeof(history), encoded);

  doc["device_id"] = "crosspoint-reader";
  doc["device"] = "CrossPoint";
  doc["v"] = 5;
  doc["sessions"] = 0;  // counted per book only
  doc["seconds"] = totals.seconds;
  doc["pages"] = 0;  // not recorded on the device
  doc["completed"] = totals.booksFinished;
  doc["anchor_day"] = totals.today;
  doc["history_b64"] = encoded;
  doc["streak"] = totals.longestStreak;
  uint32_t dow[7] = {};
  for (const auto& d : days) dow[weekday(d.day)] += d.seconds;
  weekdays(doc.as<JsonObject>(), dow);
  JsonArray daily = doc["daily"].to<JsonArray>();
  for (size_t i = from; i < from + count && i < days.size(); i++) {
    char date[11];
    dateString(days[i].day, date);
    JsonObject entry = daily.add<JsonObject>();
    entry["date"] = date;
    entry["seconds"] = days[i].seconds;
  }
}

void statsSync::addBook(JsonDocument& doc, const std::string& documentHash, const std::vector<BookDay>& days,
                        const uint32_t sessions, const bool finished) {
  doc["device_id"] = "crosspoint-reader";
  JsonArray items = doc["items"].is<JsonArray>() ? doc["items"].as<JsonArray>() : doc["items"].to<JsonArray>();
  JsonObject item = items.add<JsonObject>();
  uint32_t seconds = 0;
  uint32_t dow[7] = {};
  for (const auto& d : days) {
    seconds += d.minutes * 60u;
    dow[weekday(daysSince2000(d.year, d.month, d.day))] += d.minutes * 60u;
  }
  const auto dayOf = [](const BookDay& d) { return daysSince2000(d.year, d.month, d.day); };
  item["document"] = documentHash;
  item["v"] = 5;
  item["sessions"] = sessions;
  item["seconds"] = seconds;
  item["pages"] = 0;
  item["completed"] = finished;
  item["avg_fwd"] = 0;
  item["pace_n"] = 0;
  item["eta"] = 0;
  item["start_manual"] = false;
  item["finish_manual"] = false;
  item["start_date"] = days.empty() ? 0 : unixDate(dayOf(days.front()));
  item["finished_date"] = finished && !days.empty() ? unixDate(dayOf(days.back())) : 0;
  weekdays(item, dow);
}
