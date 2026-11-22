#pragma once
// Pure rules for updates (no Arduino, host-tested in test/update): release tags as numbers, when the daily
// "Check for updates" is due, whether a release seen online is newer than the running one, and which reader
// releases can drive the newer Sticky's SSD2677 screen.

#include <cstdint>

namespace updrules {

// "v3.0.6", "3.0.6" or "v2.0.0-ms3" as one comparable number (major, minor, patch, then the -msN build of a
// reader release); 0 when the text is not a version.
long long tagNumber(const char* tag);

// The daily check, at the first wake of the day: due 20 hours after the last one (MicroSlate has no time zone, so
// "a new day" is not a date; 20 hours keeps a slightly earlier wake the next morning). `now`, `last` and `failed`
// (the last try that found no network or no answer, 0: none) are Unix times. A try that failed waits an hour. A
// clock that is not set yet (before 2026) or went back counts as due.
constexpr uint32_t kDueS = 20u * 60u * 60u;
constexpr uint32_t kRetryS = 60u * 60u;
bool checkDue(uint32_t now, uint32_t last, uint32_t failed);

// A MicroSlate release seen online is newer than the running firmware.
bool firmwareNewer(const char* seen, const char* running);

// The newer Sticky's SSD2677 screen needs a reader built on a FreeInk with its driver: CrossPoint or CrossInk
// v2.0.0-ms1 or later. Any release fits the older screen (ssd2677 false). An unknown reader or tag does not fit.
bool readerFitsScreen(const char* id, const char* tag, bool ssd2677);

}  // namespace updrules
