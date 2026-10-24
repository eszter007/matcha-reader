#pragma once

#include <ArduinoJson.h>
#include <BookStats.h>

#include <cstdint>
#include <string>
#include <vector>

// Uploads reading time to crosspoint-sync's CrossInk stats endpoints (PUT /api/v1/stats/global and
// /api/v1/stats/books), so the CrossPoint app shows measured minutes instead of estimating them.
namespace statsSync {

// One day of reading, keyed by days since 2000-01-01 (the server's anchor_day unit).
struct Day {
  int32_t day;
  uint32_t seconds;
};

// Totals that go with every global upload.
struct Totals {
  int32_t today = 0;  // days since 2000-01-01; anchors the history bitmap
  uint32_t seconds = 0;
  uint16_t booksFinished = 0;
  int longestStreak = 0;  // the server's `streak` is the all-time longest
};

// The server accepts at most this many days per global upload.
constexpr size_t DAYS_PER_UPLOAD = 20;
// Matches the server's 730-day history bitmap; older days are not uploaded.
constexpr int32_t HISTORY_DAYS = 730;

int32_t daysSince2000(uint16_t year, uint8_t month, uint8_t day);

// Global snapshot for days[from, from + count). `days` is sorted ascending; the history bitmap and
// weekday totals cover all of it, `daily` only the slice.
void buildGlobal(JsonDocument& doc, const std::vector<Day>& days, const Totals& totals, size_t from, size_t count);

// Books per /stats/books upload. The server takes 20; 10 keeps the body near 4 KB while TLS holds its buffers.
constexpr size_t BOOKS_PER_UPLOAD = 10;

// Appends one book's snapshot from its BookStats days (sorted ascending) to a /stats/books body. finished: the
// book is on the finished list; its last reading day stands in for the finish date, which the device does not
// record.
void addBook(JsonDocument& doc, const std::string& documentHash, const std::vector<BookDay>& days, uint32_t sessions,
             bool finished);

// Uploads the stats of every book read since the last upload (all of them the first time), then the daily
// totals. Only for crosspoint-sync servers with credentials. Failures are logged and never fail the
// caller's sync: progress matters more than stats. The device id is fixed, as for progress, so the
// server treats every CrossPoint reader on one account as the same device. bookPath is the open book; it is
// sent even when read before the last upload, under documentHash.
void run(const std::string& bookPath, const std::string& documentHash);

}  // namespace statsSync
