#include <ArduinoJson.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "stats/DailyStatsParser.h"
#include "stats/StatsSync.h"

using statsSync::Day;

namespace {
int32_t day(const uint16_t y, const uint8_t m, const uint8_t d) { return statsSync::daysSince2000(y, m, d); }

void dates() {
  assert(day(2000, 1, 1) == 0);
  assert(day(2000, 3, 1) == 60);  // 2000 was a leap year
  assert(day(2026, 10, 24) == 9793);
  assert(day(2026, 3, 1) - day(2026, 2, 28) == 1);
}

void global() {
  const int32_t today = day(2026, 10, 24);
  // Read today, yesterday and three days ago (a Tuesday), plus one day outside the 730-day window.
  const std::vector<Day> days = {{today - 800, 60}, {today - 3, 600}, {today - 1, 1200}, {today, 300}};
  statsSync::Totals totals;
  totals.today = today;
  totals.seconds = 2160;
  totals.booksFinished = 4;
  totals.longestStreak = 2;

  JsonDocument doc;
  statsSync::buildGlobal(doc, days, totals, 1, 2);
  assert(doc["anchor_day"] == today);
  assert(doc["seconds"] == 2160);
  assert(doc["completed"] == 4);
  assert(doc["streak"] == 2);
  assert(doc["v"] == 5);

  // Only the requested slice goes into `daily`, as ISO dates.
  JsonArrayConst daily = doc["daily"].as<JsonArrayConst>();
  assert(daily.size() == 2);
  assert(std::string(daily[0]["date"].as<const char*>()) == "2026-10-21");
  assert(daily[0]["seconds"] == 600);
  assert(std::string(daily[1]["date"].as<const char*>()) == "2026-10-23");

  // Bit N = today - N, LSB first: today (bit 0), yesterday (bit 1), three days ago (bit 3).
  // The day outside the window sets nothing. 0b00001011 = 0x0B -> "Cw" in base64.
  const std::string history = doc["history_b64"].as<const char*>();
  assert(history.size() == 124);  // 92 bytes
  assert(history.rfind("CwAA", 0) == 0);
  assert(history.find_first_not_of('A', 4) == history.size() - 1);  // rest zero, then "="

  // 2026-10-24 is a Saturday; the server's weekdays run Monday first.
  JsonArrayConst dow = doc["dow"].as<JsonArrayConst>();
  assert(dow[5] == 300);   // Saturday
  assert(dow[4] == 1200);  // Friday
  assert(dow[2] == 600);   // Wednesday
  assert(dow[6] == 0);     // Sunday
  assert(doc["tod"].as<JsonArrayConst>().size() == 4);
}

void book() {
  // 2026-10-01 is a Thursday, 2026-10-06 a Tuesday.
  const std::vector<BookDay> days = {{2026, 10, 1, 10}, {2026, 10, 6, 15}};
  JsonDocument doc;
  statsSync::addBook(doc, "0123456789abcdef0123456789abcdef", days, 3, true);
  JsonObjectConst item = doc["items"][0];
  assert(item["seconds"] == 1500);
  assert(item["sessions"] == 3);
  assert(item["completed"] == true);
  assert(item["start_date"] == 1790812800u);     // 2026-10-01 00:00 UTC
  assert(item["finished_date"] == 1791244800u);  // 2026-10-06, the last reading day
  assert(item["dow"][3] == 600);                 // Thursday
  assert(item["dow"][1] == 900);                 // Tuesday

  // A second book goes into the same body.
  statsSync::addBook(doc, "fedcba9876543210fedcba9876543210", days, 1, false);
  assert(doc["items"].size() == 2);
  assert(doc["items"][0]["completed"] == true);
  assert(doc["items"][1]["document"] == "fedcba9876543210fedcba9876543210");
  assert(doc["items"][1]["finished_date"] == 0);
}

// Only this reader's days come back, in the window, fed in small chunks the way TLS delivers them.
void download() {
  const std::string body =
      R"({"devices":[)"
      R"({"device_id":"kobo","device":"Kobo","stats":{"daily":[{"date":"2026-10-20","seconds":600}]}},)"
      R"({"device_id":"crosspoint-reader","device":"CrossPoint","updated_at":1,)"
      R"("stats":{"v":5,"dow":[0,0,0,0,0,0,0],"daily":[)"
      R"({"date":"2024-01-01","seconds":600},)"  // before the window
      R"({"date":"2026-10-23","seconds":2460},)"
      R"({"date":"2026-10-24","seconds":30},)"  // under a minute
      R"({"date":"bad","seconds":60},)"
      R"({"date":"2026-10-25","seconds":120}]}}],)"  // after today
      R"("bookorbit":{"daily":[{"date":"2026-10-22","seconds":600}]}})";
  std::vector<DailyReading> days;
  days.reserve(4);
  DailyStatsParser parser(statsSync::DEVICE_ID, days, day(2026, 10, 24) - 729, day(2026, 10, 24));
  for (size_t i = 0; i < body.size(); i += 7) parser.feed(body.data() + i, std::min<size_t>(7, body.size() - i));
  assert(parser.complete());
  assert(days.size() == 1);
  assert(days[0].year == 2026 && days[0].month == 10 && days[0].day == 23 && days[0].minutesRead == 41);

  std::vector<DailyReading> cut;
  cut.reserve(1);
  DailyStatsParser truncated(statsSync::DEVICE_ID, cut, 0, day(2026, 10, 24));
  truncated.feed(body.data(), body.size() / 2);
  assert(!truncated.complete());
}
}  // namespace

int main() {
  dates();
  global();
  book();
  download();
  puts("stats sync tests passed");
  return 0;
}
