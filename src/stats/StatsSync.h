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

// The id every CrossPoint reader uploads under, so the server sees one device per account.
constexpr char DEVICE_ID[] = "crosspoint-reader";

// The server accepts at most this many days per global upload.
constexpr size_t DAYS_PER_UPLOAD = 20;
// Matches the server's 730-day history bitmap; older days are not uploaded.
constexpr int32_t HISTORY_DAYS = 730;

int32_t daysSince2000(uint16_t year, uint8_t month, uint8_t day);

// Global snapshot for days[from, from + count). `days` is sorted ascending; the history bitmap and
// weekday totals cover all of it, `daily` only the slice.
void buildGlobal(JsonDocument& doc, const std::vector<Day>& days, const Totals& totals, size_t from, size_t count);

// Items per /stats/books upload. The server takes 20; 5 items of 20 days keep the body near 5 KB while TLS
// holds its buffers.
constexpr size_t ITEMS_PER_UPLOAD = 5;
// Days the server takes per item; a book with more new days goes in several items.
constexpr size_t DAYS_PER_ITEM = 20;

// Appends one book's snapshot from its BookStats days (sorted ascending) to a /stats/books body, with
// `dailyCount` of those days from `daily` as the item's per-day seconds. finished: the book is on the finished
// list; finishDay is the day the device stored, else the last reading day stands in for it.
void addBook(JsonDocument& doc, const std::string& documentHash, const std::vector<BookDay>& days, uint32_t sessions,
             bool finished, const BookDay* daily = nullptr, size_t dailyCount = 0, const BookDay* finishDay = nullptr);

// Uploads the stats of every book read since the last upload (all of them the first time), then the daily
// totals. Only for crosspoint-sync servers with credentials. Failures are logged and never fail the
// caller's sync: progress matters more than stats. The device id is fixed, as for progress, so the
// server treats every CrossPoint reader on one account as the same device. bookPath is the open book; it is
// sent even when read before the last upload, under documentHash.
// True when everything was sent.
bool run(const std::string& bookPath, const std::string& documentHash);

// Downloads the daily minutes this reader's account uploaded (the last HISTORY_DAYS) and saves them
// for ReadingStatsStore::compact() to merge at the next boot. Same server, credentials and clock
// rules as run(); false when nothing was saved.
bool download();

// Insights' "All devices" setting is on and the signed-in account has downloaded at least once, so the
// saved overview belongs to it: another account's, or one from before a sign-out, is never shown.
bool showAllDevices();

}  // namespace statsSync
