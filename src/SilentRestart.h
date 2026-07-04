#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

void silentRestart();          // home screen
void silentRestartToReader();  // currently-open EPUB (APP_STATE.openEpubPath)

// Straight into the Translation activity, re-reading the page text the activity
// stashed at TRANSLATE_STASH_PATH before restarting. Used when the TLS/WiFi heap
// gates fail on a fragmented heap: right after boot the largest allocatable block
// is ~110KB, so a translation that is impossible mid-session succeeds trivially.
// If the stash is missing/empty at boot, setup() falls back to the reader.
void silentRestartToTranslation();

// One page of extracted source text, written by EpubReaderTranslationActivity
// just before silentRestartToTranslation() and consumed (read + deleted) by
// setup(). On SD, not RTC_NOINIT: a page of CJK text (2-6KB) doesn't fit the
// ~2.6KB of RTC slow memory left.
constexpr const char* TRANSLATE_STASH_PATH = "/system/translate_pending.txt";
